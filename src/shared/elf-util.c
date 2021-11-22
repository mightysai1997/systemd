/* SPDX-License-Identifier: LGPL-2.1-or-later */

#if HAVE_ELFUTILS

#include <dwarf.h>
#include <elfutils/libdwelf.h>
#include <elfutils/libdwfl.h>
#include <libelf.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc-util.h"
#include "dlfcn-util.h"
#include "elf-util.h"
#include "errno-util.h"
#include "fileio.h"
#include "fd-util.h"
#include "format-util.h"
#include "hexdecoct.h"
#include "macro.h"
#include "process-util.h"
#include "rlimit-util.h"
#include "string-util.h"
#include "util.h"

#define FRAMES_MAX 64
#define THREADS_MAX 64
#define ELF_PACKAGE_METADATA_ID 0xcafe1a7e

static void *dw_dl = NULL;
static void *elf_dl = NULL;

struct stack_context {
        FILE *f;
        Dwfl *dwfl;
        Elf *elf;
        unsigned n_thread;
        unsigned n_frame;
        JsonVariant **package_metadata;
        Set **modules;
};

/* libdw symbols */
Dwarf_Attribute *(*sym_dwarf_attr_integrate)(Dwarf_Die *, unsigned int, Dwarf_Attribute *);
const char *(*sym_dwarf_diename)(Dwarf_Die *);
const char *(*sym_dwarf_formstring)(Dwarf_Attribute *);
int (*sym_dwarf_getscopes)(Dwarf_Die *, Dwarf_Addr, Dwarf_Die **);
int (*sym_dwarf_getscopes_die)(Dwarf_Die *, Dwarf_Die **);
Elf *(*sym_dwelf_elf_begin)(int);
ssize_t (*sym_dwelf_elf_gnu_build_id)(Elf *, const void **);
int (*sym_dwarf_tag)(Dwarf_Die *);
Dwfl_Module *(*sym_dwfl_addrmodule)(Dwfl *, Dwarf_Addr);
Dwfl *(*sym_dwfl_begin)(const Dwfl_Callbacks *);
int (*sym_dwfl_build_id_find_elf)(Dwfl_Module *, void **, const char *, Dwarf_Addr, char **, Elf **);
int (*sym_dwfl_core_file_attach)(Dwfl *, Elf *);
int (*sym_dwfl_core_file_report)(Dwfl *, Elf *, const char *);
void (*sym_dwfl_end)(Dwfl *);
const char *(*sym_dwfl_errmsg)(int);
int (*sym_dwfl_errno)(void);
bool (*sym_dwfl_frame_pc)(Dwfl_Frame *, Dwarf_Addr *, bool *);
ptrdiff_t (*sym_dwfl_getmodules)(Dwfl *, int (*)(Dwfl_Module *, void **, const char *, Dwarf_Addr, void *), void *, ptrdiff_t);
int (*sym_dwfl_getthreads)(Dwfl *, int (*)(Dwfl_Thread *, void *), void *);
Dwarf_Die *(*sym_dwfl_module_addrdie)(Dwfl_Module *, Dwarf_Addr, Dwarf_Addr *);
const char *(*sym_dwfl_module_addrname)(Dwfl_Module *, GElf_Addr);
int (*sym_dwfl_module_build_id)(Dwfl_Module *, const unsigned char **, GElf_Addr *);
Elf *(*sym_dwfl_module_getelf)(Dwfl_Module *, GElf_Addr *);
const char *(*sym_dwfl_module_info)(Dwfl_Module *, void ***, Dwarf_Addr *, Dwarf_Addr *, Dwarf_Addr *, Dwarf_Addr *, const char **, const char **);
int (*sym_dwfl_offline_section_address)(Dwfl_Module *, void **, const char *, Dwarf_Addr, const char *, GElf_Word, const GElf_Shdr *, Dwarf_Addr *);
int (*sym_dwfl_report_end)(Dwfl *, int (*)(Dwfl_Module *, void *, const char *, Dwarf_Addr, void *), void *);
int (*sym_dwfl_standard_find_debuginfo)(Dwfl_Module *, void **, const char *, Dwarf_Addr, const char *, const char *, GElf_Word, char **);
int (*sym_dwfl_thread_getframes)(Dwfl_Thread *, int (*)(Dwfl_Frame *, void *), void *);
pid_t (*sym_dwfl_thread_tid)(Dwfl_Thread *);

/* libelf symbols */
Elf *(*sym_elf_begin)(int, Elf_Cmd, Elf *);
int (*sym_elf_end)(Elf *);
Elf_Data *(*sym_elf_getdata_rawchunk)(Elf *, int64_t, size_t, Elf_Type);
int (*sym_elf_getphdrnum)(Elf *, size_t *);
const char *(*sym_elf_errmsg)(int);
Elf *(*sym_elf_memory)(char *, size_t);
unsigned int (*sym_elf_version)(unsigned int);
GElf_Phdr *(*sym_gelf_getphdr)(Elf *, int, GElf_Phdr *);
size_t (*sym_gelf_getnote)(Elf_Data *, size_t, GElf_Nhdr *, size_t *, size_t *);

static int dlopen_dw(void) {
        int r;

        r = dlopen_many_sym_or_warn(
                        &dw_dl, "libdw.so.1", LOG_DEBUG,
                        DLSYM_ARG(dwarf_getscopes),
                        DLSYM_ARG(dwarf_getscopes_die),
                        DLSYM_ARG(dwarf_tag),
                        DLSYM_ARG(dwarf_attr_integrate),
                        DLSYM_ARG(dwarf_formstring),
                        DLSYM_ARG(dwarf_diename),
                        DLSYM_ARG(dwelf_elf_gnu_build_id),
                        DLSYM_ARG(dwelf_elf_begin),
                        DLSYM_ARG(dwfl_addrmodule),
                        DLSYM_ARG(dwfl_frame_pc),
                        DLSYM_ARG(dwfl_module_addrdie),
                        DLSYM_ARG(dwfl_module_addrname),
                        DLSYM_ARG(dwfl_module_info),
                        DLSYM_ARG(dwfl_module_build_id),
                        DLSYM_ARG(dwfl_module_getelf),
                        DLSYM_ARG(dwfl_begin),
                        DLSYM_ARG(dwfl_core_file_report),
                        DLSYM_ARG(dwfl_report_end),
                        DLSYM_ARG(dwfl_getmodules),
                        DLSYM_ARG(dwfl_core_file_attach),
                        DLSYM_ARG(dwfl_end),
                        DLSYM_ARG(dwfl_errno),
                        DLSYM_ARG(dwfl_errmsg),
                        DLSYM_ARG(dwfl_build_id_find_elf),
                        DLSYM_ARG(dwfl_standard_find_debuginfo),
                        DLSYM_ARG(dwfl_thread_tid),
                        DLSYM_ARG(dwfl_thread_getframes),
                        DLSYM_ARG(dwfl_getthreads),
                        DLSYM_ARG(dwfl_offline_section_address));
        if (r <= 0)
                return r;

        return 1;
}

static int dlopen_elf(void) {
        int r;

        r = dlopen_many_sym_or_warn(
                        &elf_dl, "libelf.so.1", LOG_DEBUG,
                        DLSYM_ARG(elf_begin),
                        DLSYM_ARG(elf_end),
                        DLSYM_ARG(elf_getphdrnum),
                        DLSYM_ARG(elf_getdata_rawchunk),
                        DLSYM_ARG(elf_errmsg),
                        DLSYM_ARG(elf_memory),
                        DLSYM_ARG(elf_version),
                        DLSYM_ARG(gelf_getphdr),
                        DLSYM_ARG(gelf_getnote));
        if (r <= 0)
                return r;

        return 1;
}

static int frame_callback(Dwfl_Frame *frame, void *userdata) {
        struct stack_context *c = userdata;
        Dwarf_Addr pc, pc_adjusted, bias = 0;
        _cleanup_free_ Dwarf_Die *scopes = NULL;
        const char *fname = NULL, *symbol = NULL;
        Dwfl_Module *module;
        bool is_activation;
        uint64_t module_offset = 0;

        assert(frame);
        assert(c);

        if (c->n_frame >= FRAMES_MAX)
                return DWARF_CB_ABORT;

        if (!sym_dwfl_frame_pc(frame, &pc, &is_activation))
                return DWARF_CB_ABORT;

        pc_adjusted = pc - (is_activation ? 0 : 1);

        module = sym_dwfl_addrmodule(c->dwfl, pc_adjusted);
        if (module) {
                Dwarf_Die *s, *cudie;
                int n;
                Dwarf_Addr start;

                cudie = sym_dwfl_module_addrdie(module, pc_adjusted, &bias);
                if (cudie) {
                        n = sym_dwarf_getscopes(cudie, pc_adjusted - bias, &scopes);
                        for (s = scopes; s < scopes + n; s++) {
                                if (IN_SET(sym_dwarf_tag(s), DW_TAG_subprogram, DW_TAG_inlined_subroutine, DW_TAG_entry_point)) {
                                        Dwarf_Attribute *a, space;

                                        a = sym_dwarf_attr_integrate(s, DW_AT_MIPS_linkage_name, &space);
                                        if (!a)
                                                a = sym_dwarf_attr_integrate(s, DW_AT_linkage_name, &space);
                                        if (a)
                                                symbol = sym_dwarf_formstring(a);
                                        if (!symbol)
                                                symbol = sym_dwarf_diename(s);

                                        if (symbol)
                                                break;
                                }
                        }
                }

                if (!symbol)
                        symbol = sym_dwfl_module_addrname(module, pc_adjusted);

                fname = sym_dwfl_module_info(module, NULL, &start, NULL, NULL, NULL, NULL, NULL);
                module_offset = pc - start;
        }

        if (c->f)
                fprintf(c->f, "#%-2u 0x%016" PRIx64 " %s (%s + 0x%" PRIx64 ")\n", c->n_frame, (uint64_t) pc, strna(symbol), strna(fname), module_offset);
        c->n_frame++;

        return DWARF_CB_OK;
}

static int thread_callback(Dwfl_Thread *thread, void *userdata) {
        struct stack_context *c = userdata;
        pid_t tid;

        assert(thread);
        assert(c);

        if (c->n_thread >= THREADS_MAX)
                return DWARF_CB_ABORT;

        if (c->n_thread != 0 && c->f)
                fputc('\n', c->f);

        c->n_frame = 0;

        if (c->f) {
                tid = sym_dwfl_thread_tid(thread);
                fprintf(c->f, "Stack trace of thread " PID_FMT ":\n", tid);
        }

        if (sym_dwfl_thread_getframes(thread, frame_callback, c) < 0)
                return DWARF_CB_ABORT;

        c->n_thread++;

        return DWARF_CB_OK;
}

static int parse_package_metadata(const char *name, JsonVariant *id_json, Elf *elf, struct stack_context *c) {
        size_t n_program_headers;
        int r;

        assert(name);
        assert(elf);
        assert(c);

        /* When iterating over PT_LOAD we will visit modules more than once */
        if (set_contains(*c->modules, name))
                return DWARF_CB_OK;

        r = sym_elf_getphdrnum(elf, &n_program_headers);
        if (r < 0) /* Not the handle we are looking for - that's ok, skip it */
                return DWARF_CB_OK;

        /* Iterate over all program headers in that ELF object. These will have been copied by
         * the kernel verbatim when the core file is generated. */
        for (size_t i = 0; i < n_program_headers; ++i) {
                size_t note_offset = 0, name_offset, desc_offset;
                GElf_Phdr mem, *program_header;
                GElf_Nhdr note_header;
                Elf_Data *data;

                /* Package metadata is in PT_NOTE headers. */
                program_header = sym_gelf_getphdr(elf, i, &mem);
                if (!program_header || program_header->p_type != PT_NOTE)
                        continue;

                /* Fortunately there is an iterator we can use to walk over the
                 * elements of a PT_NOTE program header. We are interested in the
                 * note with type. */
                data = sym_elf_getdata_rawchunk(elf,
                                            program_header->p_offset,
                                            program_header->p_filesz,
                                            ELF_T_NHDR);
                if (!data)
                        continue;

                while (note_offset < data->d_size &&
                       (note_offset = sym_gelf_getnote(data, note_offset, &note_header, &name_offset, &desc_offset)) > 0) {
                        const char *note_name = (const char *)data->d_buf + name_offset;
                        const char *payload = (const char *)data->d_buf + desc_offset;

                        if (note_header.n_namesz == 0 || note_header.n_descsz == 0)
                                continue;

                        /* Package metadata might have different owners, but the
                         * magic ID is always the same. */
                        if (note_header.n_type == ELF_PACKAGE_METADATA_ID) {
                                _cleanup_(json_variant_unrefp) JsonVariant *v = NULL, *w = NULL;

                                r = json_parse(payload, 0, &v, NULL, NULL);
                                if (r < 0) {
                                        log_error_errno(r, "json_parse on %s failed: %m", payload);
                                        return DWARF_CB_ABORT;
                                }

                                /* First pretty-print to the buffer, so that the metadata goes as
                                 * plaintext in the journal. */
                                if (c->f) {
                                        fprintf(c->f, "Metadata for module %s owned by %s found: ",
                                                name, note_name);
                                        json_variant_dump(v, JSON_FORMAT_NEWLINE|JSON_FORMAT_PRETTY, c->f, NULL);
                                        fputc('\n', c->f);
                                }

                                /* Secondly, if we have a build-id, merge it in the same JSON object
                                 * so that it appears all nicely together in the logs/metadata. */
                                if (id_json) {
                                        r = json_variant_merge(&v, id_json);
                                        if (r < 0) {
                                                log_error_errno(r, "json_variant_merge of package meta with buildid failed: %m");
                                                return DWARF_CB_ABORT;
                                        }
                                }

                                /* Then we build a new object using the module name as the key, and merge it
                                 * with the previous parses, so that in the end it all fits together in a single
                                 * JSON blob. */
                                r = json_build(&w, JSON_BUILD_OBJECT(JSON_BUILD_PAIR(name, JSON_BUILD_VARIANT(v))));
                                if (r < 0) {
                                        log_error_errno(r, "Failed to build JSON object: %m");
                                        return DWARF_CB_ABORT;
                                }
                                r = json_variant_merge(c->package_metadata, w);
                                if (r < 0) {
                                        log_error_errno(r, "json_variant_merge of package meta with buildid failed: %m");
                                        return DWARF_CB_ABORT;
                                }

                                /* Finally stash the name, so we avoid double visits. */
                                r = set_put_strdup(c->modules, name);
                                if (r < 0) {
                                        log_error_errno(r, "set_put_strdup failed: %m");
                                        return DWARF_CB_ABORT;
                                }

                                return DWARF_CB_OK;
                        }
                }
        }

        /* Didn't find package metadata for this module - that's ok, just go to the next. */
        return DWARF_CB_OK;
}

/* Get the build-id out of an ELF object or a dwarf core module. */
static int parse_buildid(Dwfl_Module *mod, Elf *elf, const char *name, struct stack_context *c, JsonVariant **ret_id_json) {
        _cleanup_(json_variant_unrefp) JsonVariant *id_json = NULL;
        const unsigned char *id;
        GElf_Addr id_vaddr;
        ssize_t id_len;
        int r;

        assert(mod || elf);
        assert(c);

        if (mod)
                id_len = sym_dwfl_module_build_id(mod, &id, &id_vaddr);
        else
                id_len = sym_dwelf_elf_gnu_build_id(elf, (const void **)&id);
        if (id_len <= 0) {
                /* If we don't find a build-id, note it in the journal message, and try
                 * anyway to find the package metadata. It's unlikely to have the latter
                 * without the former, but there's no hard rule. */
                if (c->f)
                        fprintf(c->f, "Found module %s without build-id.\n", name);
        } else {
                /* We will later parse package metadata json and pass it to our caller. Prepare the
                * build-id in json format too, so that it can be appended and parsed cleanly. It
                * will then be added as metadata to the journal message with the stack trace. */
                r = json_build(&id_json, JSON_BUILD_OBJECT(JSON_BUILD_PAIR("buildId", JSON_BUILD_HEX(id, id_len))));
                if (r < 0)
                        return log_error_errno(r, "json_build on build-id failed: %m");

                if (c->f) {
                        JsonVariant *build_id = json_variant_by_key(id_json, "buildId");
                        assert_se(build_id);
                        fprintf(c->f, "Found module %s with build-id: %s\n", name, json_variant_string(build_id));
                }
        }

        if (ret_id_json)
                *ret_id_json = TAKE_PTR(id_json);

        return 0;
}

static int module_callback(Dwfl_Module *mod, void **userdata, const char *name, Dwarf_Addr start, void *arg) {
        _cleanup_(json_variant_unrefp) JsonVariant *id_json = NULL;
        struct stack_context *c = arg;
        size_t n_program_headers;
        GElf_Addr bias;
        int r;
        Elf *elf;

        assert(mod);
        assert(c);

        if (!name)
                name = "(unnamed)"; /* For logging purposes */

        /* We are iterating on each "module", which is what dwfl calls ELF objects contained in the
         * core file, and extracting the build-id first and then the package metadata.
         * We proceed in a best-effort fashion - not all ELF objects might contain both or either.
         * The build-id is easy, as libdwfl parses it during the sym_dwfl_core_file_report() call and
         * stores it separately in an internal library struct. */
        r = parse_buildid(mod, NULL, name, c, &id_json);
        if (r < 0)
                return DWARF_CB_ABORT;

        /* The .note.package metadata is more difficult. From the module, we need to get a reference
         * to the ELF object first. We might be lucky and just get it from elfutils. */
        elf = sym_dwfl_module_getelf(mod, &bias);
        if (elf)
                return parse_package_metadata(name, id_json, elf, c);

        /* We did not get the ELF object. That is likely because we didn't get direct
         * access to the executable, and the version of elfutils does not yet support
         * parsing it out of the core file directly.
         * So fallback to manual extraction - get the PT_LOAD section from the core,
         * and if it's the right one we can interpret it as an Elf object, and parse
         * its notes manually. */

        r = sym_elf_getphdrnum(c->elf, &n_program_headers);
        if (r < 0) {
                log_warning("Could not parse number of program headers from core file: %s",
                            sym_elf_errmsg(-1)); /* -1 retrieves the most recent error */
                return DWARF_CB_OK;
        }

        for (size_t i = 0; i < n_program_headers; ++i) {
                GElf_Phdr mem, *program_header;
                Elf_Data *data;

                /* The core file stores the ELF files in the PT_LOAD segment. */
                program_header = sym_gelf_getphdr(c->elf, i, &mem);
                if (!program_header || program_header->p_type != PT_LOAD)
                        continue;

                /* Now get a usable Elf reference, and parse the notes from it. */
                data = sym_elf_getdata_rawchunk(c->elf,
                                            program_header->p_offset,
                                            program_header->p_filesz,
                                            ELF_T_NHDR);
                if (!data)
                        continue;

                Elf *memelf = sym_elf_memory(data->d_buf, data->d_size);
                if (!memelf)
                        continue;
                r = parse_package_metadata(name, id_json, memelf, c);
                if (r != DWARF_CB_OK)
                        return r;
        }

        return DWARF_CB_OK;
}

static int parse_core(int fd, const char *executable, char **ret, JsonVariant **ret_package_metadata) {

        const Dwfl_Callbacks callbacks = {
                .find_elf = sym_dwfl_build_id_find_elf,
                .section_address = sym_dwfl_offline_section_address,
                .find_debuginfo = sym_dwfl_standard_find_debuginfo,
        };

        _cleanup_(json_variant_unrefp) JsonVariant *package_metadata = NULL;
        _cleanup_(set_freep) Set *modules = NULL;
        struct stack_context c = {
                .package_metadata = &package_metadata,
                .modules = &modules,
        };
        char *buf = NULL;
        size_t sz = 0;
        int r;

        assert(fd >= 0);

        if (lseek(fd, 0, SEEK_SET) == (off_t) -1)
                return -errno;

        if (ret) {
                c.f = open_memstream_unlocked(&buf, &sz);
                if (!c.f)
                        return -ENOMEM;
        }

        sym_elf_version(EV_CURRENT);

        c.elf = sym_elf_begin(fd, ELF_C_READ_MMAP, NULL);
        if (!c.elf) {
                r = -EINVAL;
                goto finish;
        }

        c.dwfl = sym_dwfl_begin(&callbacks);
        if (!c.dwfl) {
                r = -EINVAL;
                goto finish;
        }

        if (sym_dwfl_core_file_report(c.dwfl, c.elf, executable) < 0) {
                r = -EINVAL;
                goto finish;
        }

        if (sym_dwfl_report_end(c.dwfl, NULL, NULL) != 0) {
                r = -EINVAL;
                goto finish;
        }

        if (sym_dwfl_getmodules(c.dwfl, &module_callback, &c, 0) < 0) {
                r = -EINVAL;
                goto finish;
        }

        if (sym_dwfl_core_file_attach(c.dwfl, c.elf) < 0) {
                r = -EINVAL;
                goto finish;
        }

        if (sym_dwfl_getthreads(c.dwfl, thread_callback, &c) < 0) {
                r = -EINVAL;
                goto finish;
        }

        c.f = safe_fclose(c.f);

        if (ret)
                *ret = TAKE_PTR(buf);
        if (ret_package_metadata)
                *ret_package_metadata = TAKE_PTR(package_metadata);

        r = 0;

finish:
        if (c.dwfl)
                sym_dwfl_end(c.dwfl);

        if (c.elf)
                sym_elf_end(c.elf);

        safe_fclose(c.f);

        free(buf);

        if (r == -EINVAL)
                log_warning("Failed to generate stack trace: %s", sym_dwfl_errmsg(sym_dwfl_errno()));
        else if (r < 0)
                log_warning_errno(r, "Failed to generate stack trace: %m");

        return r;
}

static int parse_elf(int fd, const char *executable, char **ret, JsonVariant **ret_package_metadata) {
        _cleanup_(json_variant_unrefp) JsonVariant *package_metadata = NULL, *id_json = NULL;
        _cleanup_(set_freep) Set *modules = NULL;
        struct stack_context c = {
                .package_metadata = &package_metadata,
                .modules = &modules,
        };
        char *buf = NULL;
        size_t sz = 0;
        int r;

        assert(fd >= 0);

        if (lseek(fd, 0, SEEK_SET) == (off_t) -1) {
                r = -errno;
                goto finish;
        }

        if (ret) {
                c.f = open_memstream_unlocked(&buf, &sz);
                if (!c.f) {
                        r = -ENOMEM;
                        goto finish;
                }
        }

        sym_elf_version(EV_CURRENT);

        c.elf = sym_elf_begin(fd, ELF_C_READ_MMAP, NULL);
        if (!c.elf) {
                r = -EINVAL;
                goto finish;
        }

        r = parse_buildid(NULL, c.elf, executable, &c, &id_json);
        if (r < 0)
                goto finish;

        r = parse_package_metadata(executable, id_json, c.elf, &c);
        if (r < 0)
                goto finish;

        c.f = safe_fclose(c.f);

        /* If we found a build-id and nothing else, return at least that. */
        if (!package_metadata && id_json) {
                r = json_build(&package_metadata,
                               JSON_BUILD_OBJECT(JSON_BUILD_PAIR(executable, JSON_BUILD_VARIANT(id_json))));
                if (r < 0)
                        goto finish;
        }

        if (ret)
                *ret = TAKE_PTR(buf);
        if (ret_package_metadata)
                *ret_package_metadata = TAKE_PTR(package_metadata);

        r = 0;

finish:
        if (c.dwfl)
                sym_dwfl_end(c.dwfl);

        if (c.elf)
                sym_elf_end(c.elf);

        safe_fclose(c.f);

        free(buf);

        if (r == -EINVAL)
                log_warning("Failed to inspect ELF: %s", sym_dwfl_errmsg(sym_dwfl_errno()));
        else if (r < 0)
                log_warning_errno(r, "Failed to inspect ELF: %m");

        return r;
}

int parse_elf_object(int fd, const char *executable, char **ret, JsonVariant **ret_package_metadata) {
        _cleanup_close_pair_ int error_pipe[2] = { -1, -1 }, return_pipe[2] = { -1, -1 }, json_pipe[2] = { -1, -1 };
        int r;

        r = dlopen_dw();
        if (r < 0)
                return r;

        r = dlopen_elf();
        if (r < 0)
                return r;

        r = RET_NERRNO(pipe2(error_pipe, O_CLOEXEC));
        if (r < 0)
                return r;

        if (ret) {
                r = RET_NERRNO(pipe2(return_pipe, O_CLOEXEC));
                if (r < 0)
                        return r;
        }

        if (ret_package_metadata) {
                r = RET_NERRNO(pipe2(json_pipe, O_CLOEXEC));
                if (r < 0)
                        return r;
        }

        /* Parsing possibly malformed data is crash-happy, so fork and avoid looping. */
        r = safe_fork_full("(sd-parse-elf)",
                           (int[]){ fd, error_pipe[1], return_pipe[1], json_pipe[1] },
                           4,
                           FORK_RESET_SIGNALS|FORK_CLOSE_ALL_FDS|FORK_NEW_MOUNTNS|FORK_MOUNTNS_SLAVE|FORK_NEW_USERNS|FORK_WAIT,
                           NULL);
        if (r < 0) {
                if (r == -EPROTO) { /* We should have the errno from the child, but don't clobber original error */
                        int e, k;

                        k = RET_NERRNO(read(error_pipe[0], &e, sizeof(e)));
                        if (k < 0)
                                return k;
                        if (k == sizeof(e))
                                return e; /* propagate error sent to us from child */
                        if (k != 0)
                                return -EIO;
                }

                return r;
        }
        if (r == 0) {
                _cleanup_(json_variant_unrefp) JsonVariant *package_metadata = NULL;
                _cleanup_free_ char *buf = NULL;

                (void) setrlimit(RLIMIT_CORE, &RLIMIT_MAKE_CONST(0));

                r = parse_core(fd, executable, ret ? &buf : NULL, ret_package_metadata ? &package_metadata : NULL);
                if (r < 0 || (!package_metadata && !buf)) { /* Maybe not a core? Try as exec/lib */
                        int k;

                        k = parse_elf(fd, executable, ret ? &buf : NULL, ret_package_metadata ? &package_metadata : NULL);
                        if (k < 0) { /* Don't clobber the original error */
                                if (r == 0)
                                        r = k;
                                goto child_fail;
                        }
                }

                if (buf) {
                        _cleanup_fclose_ FILE *out = NULL;

                        out = fdopen(TAKE_FD(return_pipe[1]), "w");
                        if (!out) {
                                r = -errno;
                                goto child_fail;
                        }

                        fputs(buf, out);
                        fflush(out);
                }

                if (package_metadata) {
                        _cleanup_fclose_ FILE *json_out = NULL;

                        json_out = fdopen(TAKE_FD(json_pipe[1]), "w");
                        if (!json_out) {
                                r = -errno;
                                goto child_fail;
                        }

                        json_variant_dump(package_metadata, JSON_FORMAT_FLUSH, json_out, NULL);
                }

                _exit(EXIT_SUCCESS);

        child_fail:
                (void) write(error_pipe[1], &r, sizeof(r));
                _exit(EXIT_FAILURE);
        }

        error_pipe[1] = safe_close(error_pipe[1]);
        return_pipe[1] = safe_close(return_pipe[1]);
        json_pipe[1] = safe_close(json_pipe[1]);

        if (ret) {
                _cleanup_fclose_ FILE *in = NULL;

                in = fdopen(TAKE_FD(return_pipe[0]), "r");
                if (!in)
                        return -errno;

                r = read_full_stream(in, ret, NULL);
                if (r < 0)
                        return r;
        }

        if (ret_package_metadata) {
                _cleanup_fclose_ FILE *json_in = NULL;

                json_in = fdopen(TAKE_FD(json_pipe[0]), "r");
                if (!json_in)
                        return -errno;

                r = json_parse_file(json_in, NULL, 0, ret_package_metadata, NULL, NULL);
                if (r < 0 && r != -EINVAL)
                        return r;
        }

        return 0;
}

#endif
