/* SPDX-License-Identifier: LGPL-2.1+ */

#include "sd-path.h"

#include "alloc-util.h"
#include "architecture.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "missing_stdlib.h"
#include "path-util.h"
#include "string-util.h"
#include "strv.h"
#include "user-util.h"
#include "util.h"

static int from_environment(const char *envname, const char *fallback, const char **ret) {
        assert(ret);

        if (envname) {
                const char *e;

                e = secure_getenv(envname);
                if (e && path_is_absolute(e)) {
                        *ret = e;
                        return 0;
                }
        }

        if (fallback) {
                *ret = fallback;
                return 0;
        }

        return -ENXIO;
}

static int from_home_dir(const char *envname, const char *suffix, char **buffer, const char **ret) {
        _cleanup_free_ char *h = NULL;
        char *cc = NULL;
        int r;

        assert(suffix);
        assert(buffer);
        assert(ret);

        if (envname) {
                const char *e = NULL;

                e = secure_getenv(envname);
                if (e && path_is_absolute(e)) {
                        *ret = e;
                        return 0;
                }
        }

        r = get_home_dir(&h);
        if (r < 0)
                return r;

        if (endswith(h, "/"))
                cc = strappend(h, suffix);
        else
                cc = strjoin(h, "/", suffix);
        if (!cc)
                return -ENOMEM;

        *buffer = cc;
        *ret = cc;
        return 0;
}

static int from_user_dir(const char *field, char **buffer, const char **ret) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *b = NULL;
        _cleanup_free_ const char *fn = NULL;
        const char *c = NULL;
        size_t n;
        int r;

        assert(field);
        assert(buffer);
        assert(ret);

        r = from_home_dir("XDG_CONFIG_HOME", ".config", &b, &c);
        if (r < 0)
                return r;

        fn = strappend(c, "/user-dirs.dirs");
        if (!fn)
                return -ENOMEM;

        f = fopen(fn, "re");
        if (!f) {
                if (errno == ENOENT)
                        goto fallback;

                return -errno;
        }

        /* This is an awful parse, but it follows closely what
         * xdg-user-dirs does upstream */

        n = strlen(field);
        for (;;) {
                _cleanup_free_ char *line = NULL;
                char *l, *p, *e;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                l = strstrip(line);

                if (!strneq(l, field, n))
                        continue;

                p = l + n;
                p += strspn(p, WHITESPACE);

                if (*p != '=')
                        continue;
                p++;

                p += strspn(p, WHITESPACE);

                if (*p != '"')
                        continue;
                p++;

                e = strrchr(p, '"');
                if (!e)
                        continue;
                *e = 0;

                /* Three syntaxes permitted: relative to $HOME, $HOME itself, and absolute path */
                if (startswith(p, "$HOME/")) {
                        _cleanup_free_ char *h = NULL;
                        char *cc;

                        r = get_home_dir(&h);
                        if (r < 0)
                                return r;

                        cc = strappend(h, p+5);
                        if (!cc)
                                return -ENOMEM;

                        *buffer = cc;
                        *ret = cc;
                        return 0;
                } else if (streq(p, "$HOME")) {

                        r = get_home_dir(buffer);
                        if (r < 0)
                                return r;

                        *ret = *buffer;
                        return 0;
                } else if (path_is_absolute(p)) {
                        char *copy;

                        copy = strdup(p);
                        if (!copy)
                                return -ENOMEM;

                        *buffer = copy;
                        *ret = copy;
                        return 0;
                }
        }

fallback:
        /* The desktop directory defaults to $HOME/Desktop, the others to $HOME */
        if (streq(field, "XDG_DESKTOP_DIR")) {
                _cleanup_free_ char *h = NULL;
                char *cc;

                r = get_home_dir(&h);
                if (r < 0)
                        return r;

                cc = strappend(h, "/Desktop");
                if (!cc)
                        return -ENOMEM;

                *buffer = cc;
                *ret = cc;
        } else {

                r = get_home_dir(buffer);
                if (r < 0)
                        return r;

                *ret = *buffer;
        }

        return 0;
}

static int get_path(uint64_t type, char **buffer, const char **ret) {
        int r;

        assert(buffer);
        assert(ret);

        switch (type) {

        case SD_PATH_TEMPORARY:
                return tmp_dir(ret);

        case SD_PATH_TEMPORARY_LARGE:
                return var_tmp_dir(ret);

        case SD_PATH_SYSTEM_BINARIES:
                *ret = "/usr/bin";
                return 0;

        case SD_PATH_SYSTEM_INCLUDE:
                *ret = "/usr/include";
                return 0;

        case SD_PATH_SYSTEM_LIBRARY_PRIVATE:
                *ret = "/usr/lib";
                return 0;

        case SD_PATH_SYSTEM_LIBRARY_ARCH:
                *ret = LIBDIR;
                return 0;

        case SD_PATH_SYSTEM_SHARED:
                *ret = "/usr/share";
                return 0;

        case SD_PATH_SYSTEM_CONFIGURATION_FACTORY:
                *ret = "/usr/share/factory/etc";
                return 0;

        case SD_PATH_SYSTEM_STATE_FACTORY:
                *ret = "/usr/share/factory/var";
                return 0;

        case SD_PATH_SYSTEM_CONFIGURATION:
                *ret = "/etc";
                return 0;

        case SD_PATH_SYSTEM_RUNTIME:
                *ret = "/run";
                return 0;

        case SD_PATH_SYSTEM_RUNTIME_LOGS:
                *ret = "/run/log";
                return 0;

        case SD_PATH_SYSTEM_STATE_PRIVATE:
                *ret = "/var/lib";
                return 0;

        case SD_PATH_SYSTEM_STATE_LOGS:
                *ret = "/var/log";
                return 0;

        case SD_PATH_SYSTEM_STATE_CACHE:
                *ret = "/var/cache";
                return 0;

        case SD_PATH_SYSTEM_STATE_SPOOL:
                *ret = "/var/spool";
                return 0;

        case SD_PATH_USER_BINARIES:
                return from_home_dir(NULL, ".local/bin", buffer, ret);

        case SD_PATH_USER_LIBRARY_PRIVATE:
                return from_home_dir(NULL, ".local/lib", buffer, ret);

        case SD_PATH_USER_LIBRARY_ARCH:
                return from_home_dir(NULL, ".local/lib/" LIB_ARCH_TUPLE, buffer, ret);

        case SD_PATH_USER_SHARED:
                return from_home_dir("XDG_DATA_HOME", ".local/share", buffer, ret);

        case SD_PATH_USER_CONFIGURATION:
                return from_home_dir("XDG_CONFIG_HOME", ".config", buffer, ret);

        case SD_PATH_USER_RUNTIME:
                return from_environment("XDG_RUNTIME_DIR", NULL, ret);

        case SD_PATH_USER_STATE_CACHE:
                return from_home_dir("XDG_CACHE_HOME", ".cache", buffer, ret);

        case SD_PATH_USER:
                r = get_home_dir(buffer);
                if (r < 0)
                        return r;

                *ret = *buffer;
                return 0;

        case SD_PATH_USER_DOCUMENTS:
                return from_user_dir("XDG_DOCUMENTS_DIR", buffer, ret);

        case SD_PATH_USER_MUSIC:
                return from_user_dir("XDG_MUSIC_DIR", buffer, ret);

        case SD_PATH_USER_PICTURES:
                return from_user_dir("XDG_PICTURES_DIR", buffer, ret);

        case SD_PATH_USER_VIDEOS:
                return from_user_dir("XDG_VIDEOS_DIR", buffer, ret);

        case SD_PATH_USER_DOWNLOAD:
                return from_user_dir("XDG_DOWNLOAD_DIR", buffer, ret);

        case SD_PATH_USER_PUBLIC:
                return from_user_dir("XDG_PUBLICSHARE_DIR", buffer, ret);

        case SD_PATH_USER_TEMPLATES:
                return from_user_dir("XDG_TEMPLATES_DIR", buffer, ret);

        case SD_PATH_USER_DESKTOP:
                return from_user_dir("XDG_DESKTOP_DIR", buffer, ret);
        }

        return -EOPNOTSUPP;
}

_public_ int sd_path_home(uint64_t type, const char *suffix, char **path) {
        char *buffer = NULL, *cc;
        const char *ret;
        int r;

        assert_return(path, -EINVAL);

        if (IN_SET(type,
                   SD_PATH_SEARCH_BINARIES,
                   SD_PATH_SEARCH_BINARIES_DEFAULT,
                   SD_PATH_SEARCH_LIBRARY_PRIVATE,
                   SD_PATH_SEARCH_LIBRARY_ARCH,
                   SD_PATH_SEARCH_SHARED,
                   SD_PATH_SEARCH_CONFIGURATION_FACTORY,
                   SD_PATH_SEARCH_STATE_FACTORY,
                   SD_PATH_SEARCH_CONFIGURATION)) {

                _cleanup_strv_free_ char **l = NULL;

                r = sd_path_search(type, suffix, &l);
                if (r < 0)
                        return r;

                buffer = strv_join(l, ":");
                if (!buffer)
                        return -ENOMEM;

                *path = buffer;
                return 0;
        }

        r = get_path(type, &buffer, &ret);
        if (r < 0)
                return r;

        if (!suffix) {
                if (!buffer) {
                        buffer = strdup(ret);
                        if (!buffer)
                                return -ENOMEM;
                }

                *path = buffer;
                return 0;
        }

        suffix += strspn(suffix, "/");

        if (endswith(ret, "/"))
                cc = strappend(ret, suffix);
        else
                cc = strjoin(ret, "/", suffix);

        free(buffer);

        if (!cc)
                return -ENOMEM;

        *path = cc;
        return 0;
}

static int search_from_environment(
                char ***list,
                const char *env_home,
                const char *home_suffix,
                const char *env_search,
                bool env_search_sufficient,
                const char *first, ...) {

        const char *e;
        char *h = NULL;
        char **l = NULL;
        int r;

        assert(list);

        if (env_search) {
                e = secure_getenv(env_search);
                if (e) {
                        l = strv_split(e, ":");
                        if (!l)
                                return -ENOMEM;

                        if (env_search_sufficient) {
                                *list = l;
                                return 0;
                        }
                }
        }

        if (!l && first) {
                va_list ap;

                va_start(ap, first);
                l = strv_new_ap(first, ap);
                va_end(ap);

                if (!l)
                        return -ENOMEM;
        }

        if (env_home) {
                e = secure_getenv(env_home);
                if (e && path_is_absolute(e)) {
                        h = strdup(e);
                        if (!h) {
                                strv_free(l);
                                return -ENOMEM;
                        }
                }
        }

        if (!h && home_suffix) {
                e = secure_getenv("HOME");
                if (e && path_is_absolute(e)) {
                        if (endswith(e, "/"))
                                h = strappend(e, home_suffix);
                        else
                                h = strjoin(e, "/", home_suffix);

                        if (!h) {
                                strv_free(l);
                                return -ENOMEM;
                        }
                }
        }

        if (h) {
                r = strv_consume_prepend(&l, h);
                if (r < 0) {
                        strv_free(l);
                        return -ENOMEM;
                }
        }

        *list = l;
        return 0;
}

#if HAVE_SPLIT_BIN
#  define ARRAY_SBIN_BIN(x) x "sbin", x "bin"
#else
#  define ARRAY_SBIN_BIN(x) x "bin"
#endif

static int get_search(uint64_t type, char ***list) {

        assert(list);

        switch(type) {

        case SD_PATH_SEARCH_BINARIES:
                return search_from_environment(list,
                                               NULL,
                                               ".local/bin",
                                               "PATH",
                                               true,
                                               ARRAY_SBIN_BIN("/usr/local/"),
                                               ARRAY_SBIN_BIN("/usr/"),
#if HAVE_SPLIT_USR
                                               ARRAY_SBIN_BIN("/"),
#endif
                                               NULL);

        case SD_PATH_SEARCH_LIBRARY_PRIVATE:
                return search_from_environment(list,
                                               NULL,
                                               ".local/lib",
                                               NULL,
                                               false,
                                               "/usr/local/lib",
                                               "/usr/lib",
#if HAVE_SPLIT_USR
                                               "/lib",
#endif
                                               NULL);

        case SD_PATH_SEARCH_LIBRARY_ARCH:
                return search_from_environment(list,
                                               NULL,
                                               ".local/lib/" LIB_ARCH_TUPLE,
                                               "LD_LIBRARY_PATH",
                                               true,
                                               LIBDIR,
#if HAVE_SPLIT_USR
                                               ROOTLIBDIR,
#endif
                                               NULL);

        case SD_PATH_SEARCH_SHARED:
                return search_from_environment(list,
                                               "XDG_DATA_HOME",
                                               ".local/share",
                                               "XDG_DATA_DIRS",
                                               false,
                                               "/usr/local/share",
                                               "/usr/share",
                                               NULL);

        case SD_PATH_SEARCH_CONFIGURATION_FACTORY:
                return search_from_environment(list,
                                               NULL,
                                               NULL,
                                               NULL,
                                               false,
                                               "/usr/local/share/factory/etc",
                                               "/usr/share/factory/etc",
                                               NULL);

        case SD_PATH_SEARCH_STATE_FACTORY:
                return search_from_environment(list,
                                               NULL,
                                               NULL,
                                               NULL,
                                               false,
                                               "/usr/local/share/factory/var",
                                               "/usr/share/factory/var",
                                               NULL);

        case SD_PATH_SEARCH_CONFIGURATION:
                return search_from_environment(list,
                                               "XDG_CONFIG_HOME",
                                               ".config",
                                               "XDG_CONFIG_DIRS",
                                               false,
                                               "/etc",
                                               NULL);

        case SD_PATH_SEARCH_BINARIES_DEFAULT: {
                char **t;

                t = strv_split_nulstr(DEFAULT_PATH_NULSTR);
                if (!t)
                        return -ENOMEM;

                *list = t;
                return 0;
        }}

        return -EOPNOTSUPP;
}

_public_ int sd_path_search(uint64_t type, const char *suffix, char ***paths) {
        char **i, **j;
        _cleanup_strv_free_ char **l = NULL, **n = NULL;
        int r;

        assert_return(paths, -EINVAL);

        if (!IN_SET(type,
                    SD_PATH_SEARCH_BINARIES,
                    SD_PATH_SEARCH_BINARIES_DEFAULT,
                    SD_PATH_SEARCH_LIBRARY_PRIVATE,
                    SD_PATH_SEARCH_LIBRARY_ARCH,
                    SD_PATH_SEARCH_SHARED,
                    SD_PATH_SEARCH_CONFIGURATION_FACTORY,
                    SD_PATH_SEARCH_STATE_FACTORY,
                    SD_PATH_SEARCH_CONFIGURATION)) {

                char *p;

                r = sd_path_home(type, suffix, &p);
                if (r < 0)
                        return r;

                l = new(char*, 2);
                if (!l) {
                        free(p);
                        return -ENOMEM;
                }

                l[0] = p;
                l[1] = NULL;

                *paths = TAKE_PTR(l);
                return 0;
        }

        r = get_search(type, &l);
        if (r < 0)
                return r;

        if (!suffix) {
                *paths = TAKE_PTR(l);
                return 0;
        }

        n = new(char*, strv_length(l)+1);
        if (!n)
                return -ENOMEM;

        j = n;
        STRV_FOREACH(i, l) {

                if (endswith(*i, "/"))
                        *j = strappend(*i, suffix);
                else
                        *j = strjoin(*i, "/", suffix);

                if (!*j)
                        return -ENOMEM;

                j++;
        }

        *j = NULL;
        *paths = TAKE_PTR(n);
        return 0;
}
