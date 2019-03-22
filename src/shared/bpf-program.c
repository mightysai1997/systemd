/* SPDX-License-Identifier: LGPL-2.1+ */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc-util.h"
#include "bpf-program.h"
#include "bpf.h"
#include "fd-util.h"
#include "log.h"
#include "memory-util.h"
#include "missing.h"
#include "path-util.h"

int bpf_program_new(uint32_t prog_type, BPFProgram **ret) {
        _cleanup_(bpf_program_unrefp) BPFProgram *p = NULL;

        p = new0(BPFProgram, 1);
        if (!p)
                return log_oom();

        p->n_ref = 1;
        p->prog_type = prog_type;
        p->kernel_fd = -1;

        *ret = TAKE_PTR(p);

        return 0;
}

static BPFProgram *bpf_program_free(BPFProgram *p) {
        assert(p);

        /* Unfortunately, the kernel currently doesn't implicitly detach BPF programs from their cgroups when the last
         * fd to the BPF program is closed. This has nasty side-effects since this means that abnormally terminated
         * programs that attached one of their BPF programs to a cgroup will leave this programs pinned for good with
         * zero chance of recovery, until the cgroup is removed. This is particularly problematic if the cgroup in
         * question is the root cgroup (or any other cgroup belonging to a service that cannot be restarted during
         * operation, such as dbus), as the memory for the BPF program can only be reclaimed through a reboot. To
         * counter this, we track closely to which cgroup a program was attached to and will detach it on our own
         * whenever we close the BPF fd. */
        (void) bpf_program_cgroup_detach(p);

        safe_close(p->kernel_fd);
        free(p->instructions);
        free(p->attached_path);

        return mfree(p);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(BPFProgram, bpf_program, bpf_program_free);

int bpf_program_add_instructions(BPFProgram *p, const struct bpf_insn *instructions, size_t count) {

        assert(p);

        if (p->kernel_fd >= 0) /* don't allow modification after we uploaded things to the kernel */
                return -EBUSY;

        if (!GREEDY_REALLOC(p->instructions, p->allocated, p->n_instructions + count))
                return -ENOMEM;

        memcpy(p->instructions + p->n_instructions, instructions, sizeof(struct bpf_insn) * count);
        p->n_instructions += count;

        return 0;
}

int bpf_program_load_kernel(BPFProgram *p, char *log_buf, size_t log_size) {

        assert(p);

        if (p->kernel_fd >= 0) { /* make this idempotent */
                memzero(log_buf, log_size);
                return 0;
        }

        p->kernel_fd = bpf_load_program(p->prog_type, p->instructions, p->n_instructions, "GPL", 0, log_buf, log_size);
        if (p->kernel_fd < 0) {
                return -errno;
        }
        return 0;
}

int bpf_program_cgroup_attach(BPFProgram *p, int type, const char *path, uint32_t flags) {
        _cleanup_free_ char *copy = NULL;
        _cleanup_close_ int fd = -1;
        int r;

        assert(p);
        assert(type >= 0);
        assert(path);

        if (!IN_SET(flags, 0, BPF_F_ALLOW_OVERRIDE, BPF_F_ALLOW_MULTI))
                return -EINVAL;

        /* We need to track which cgroup the program is attached to, and we can only track one attachment, hence let's
        * refuse this early. */
        if (p->attached_path) {
                if (!path_equal(p->attached_path, path))
                        return -EBUSY;
                if (p->attached_type != type)
                        return -EBUSY;
                if (p->attached_flags != flags)
                        return -EBUSY;

                /* Here's a shortcut: if we previously attached this program already, then we don't have to do so
                 * again. Well, with one exception: if we are in BPF_F_ALLOW_OVERRIDE mode then someone else might have
                 * replaced our program since the last time, hence let's reattach it again, just to be safe. In flags
                 * == 0 mode this is not an issue since nobody else can replace our program in that case, and in flags
                 * == BPF_F_ALLOW_MULTI mode any other's program would be installed in addition to ours hence ours
                 * would remain in effect. */
                if (flags != BPF_F_ALLOW_OVERRIDE)
                        return 0;
        }

        /* Ensure we have a kernel object for this. */
        r = bpf_program_load_kernel(p, NULL, 0);
        if (r < 0)
                return r;

        copy = strdup(path);
        if (!copy)
                return -ENOMEM;

        fd = open(path, O_DIRECTORY|O_RDONLY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        if (bpf_prog_attach(p->kernel_fd, fd, type, flags) < 0) {
                return -errno;
        }

        free_and_replace(p->attached_path, copy);
        p->attached_type = type;
        p->attached_flags = flags;

        return 0;
}

int bpf_program_cgroup_detach(BPFProgram *p) {
        _cleanup_close_ int fd = -1;

        assert(p);

        if (!p->attached_path)
                return -EUNATCH;

        fd = open(p->attached_path, O_DIRECTORY|O_RDONLY|O_CLOEXEC);
        if (fd < 0) {
                if (errno != ENOENT)
                        return -errno;

                /* If the cgroup does not exist anymore, then we don't have to explicitly detach, it got detached
                 * implicitly by the removal, hence don't complain */

        } else {
                if (bpf_prog_detach2(p->kernel_fd, fd, p->attached_type) < 0) {
                        return -errno;
                }
        }

        p->attached_path = mfree(p->attached_path);

        return 0;
}
