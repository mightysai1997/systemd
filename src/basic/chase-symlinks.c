/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/magic.h>

#include "alloc-util.h"
#include "chase-symlinks.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "glyph-util.h"
#include "log.h"
#include "path-util.h"
#include "string-util.h"
#include "user-util.h"

bool unsafe_transition(const struct stat *a, const struct stat *b) {
        /* Returns true if the transition from a to b is safe, i.e. that we never transition from unprivileged to
         * privileged files or directories. Why bother? So that unprivileged code can't symlink to privileged files
         * making us believe we read something safe even though it isn't safe in the specific context we open it in. */

        if (a->st_uid == 0) /* Transitioning from privileged to unprivileged is always fine */
                return false;

        return a->st_uid != b->st_uid; /* Otherwise we need to stay within the same UID */
}

static int log_unsafe_transition(int a, int b, const char *path, ChaseSymlinksFlags flags) {
        _cleanup_free_ char *n1 = NULL, *n2 = NULL, *user_a = NULL, *user_b = NULL;
        struct stat st;

        if (!FLAGS_SET(flags, CHASE_WARN))
                return -ENOLINK;

        (void) fd_get_path(a, &n1);
        (void) fd_get_path(b, &n2);

        if (fstat(a, &st) == 0)
                user_a = uid_to_name(st.st_uid);
        if (fstat(b, &st) == 0)
                user_b = uid_to_name(st.st_uid);

        return log_warning_errno(SYNTHETIC_ERRNO(ENOLINK),
                                 "Detected unsafe path transition %s (owned by %s) %s %s (owned by %s) during canonicalization of %s.",
                                 strna(n1), strna(user_a), special_glyph(SPECIAL_GLYPH_ARROW_RIGHT), strna(n2), strna(user_b), path);
}

static int log_autofs_mount_point(int fd, const char *path, ChaseSymlinksFlags flags) {
        _cleanup_free_ char *n1 = NULL;

        if (!FLAGS_SET(flags, CHASE_WARN))
                return -EREMOTE;

        (void) fd_get_path(fd, &n1);

        return log_warning_errno(SYNTHETIC_ERRNO(EREMOTE),
                                 "Detected autofs mount point %s during canonicalization of %s.",
                                 strna(n1), path);
}

int chase_symlinks_at(
                int dir_fd,
                const char *path,
                ChaseSymlinksFlags flags,
                char **ret_path,
                int *ret_fd) {

        _cleanup_free_ char *buffer = NULL, *done = NULL;
        _cleanup_close_ int fd = -1, root_fd = -1;
        unsigned max_follow = CHASE_SYMLINKS_MAX; /* how many symlinks to follow before giving up and returning ELOOP */
        bool exists = true, append_trail_slash = false;
        struct stat previous_stat;
        const char *todo;
        int r;

        assert(path);
        assert(!FLAGS_SET(flags, CHASE_PREFIX_ROOT));
        assert(dir_fd >= 0 || dir_fd == AT_FDCWD);

        /* Either the file may be missing, or we return an fd to the final object, but both make no sense */
        if ((flags & CHASE_NONEXISTENT) && ret_fd)
                return -EINVAL;

        if ((flags & CHASE_STEP) && ret_fd)
                return -EINVAL;

        if (isempty(path))
                path = ".";

        /* This function allows following symlinks relative to the given directory file descriptor, instead
         * of the root of the host.
         *
         * Note that "dir_fd" primarily matters if we encounter an absolute symlink. It is also used when
         * following relative symlinks to ensure they cannot be used to "escape" the root directory. The
         * "path" parameter is always interpreted relative to the given directory file descriptor. If the
         * given directory file descriptor is invalid (< 0), the path is interpreted relative to the root
         * directory of the host.
         *
         * Algorithmically this operates on two path buffers: "done" are the components of the path we
         * already processed and resolved symlinks, "." and ".." of. "todo" are the components of the path we
         * still need to process. On each iteration, we move one component from "todo" to "done", processing
         * it's special meaning each time. We always keep an O_PATH fd to the component we are currently
         * processing, thus keeping lookup races to a minimum.
         *
         * Suggested usage: whenever you want to canonicalize a path, use this function. Pass the absolute
         * path you got as-is: fully qualified and relative to your host's root. Optionally, specify the
         * "dir_fd" parameter to tell this function what to do when encountering a symlink with an absolute
         * path as directory: resolve it relative to the given directory file descriptor.
         *
         * There are five ways to invoke this function:
         *
         * 1. Without CHASE_STEP or ret_fd: in this case the path is resolved and the normalized path is
         *    returned in `ret_path`. The return value is < 0 on error. If CHASE_NONEXISTENT is also set, 0
         *    is returned if the file doesn't exist, > 0 otherwise. If CHASE_NONEXISTENT is not set, >= 0 is
         *    returned if the destination was found, -ENOENT if it wasn't.
         *
         * 2. With ret_fd: in this case the destination is opened after chasing it as O_PATH and this file
         *    descriptor is returned as return value. This is useful to open files relative to some root
         *    directory. Note that the returned O_PATH file descriptors must be converted into a regular one
         *    (using fd_reopen() or such) before it can be used for reading/writing. ret_fd may not be
         *    combined with CHASE_NONEXISTENT.
         *
         * 3. With CHASE_STEP: in this case only a single step of the normalization is executed, i.e. only
         *    the first symlink or ".." component of the path is resolved, and the resulting path is
         *    returned. This is useful if a caller wants to trace the path through the file system verbosely.
         *    Returns < 0 on error, > 0 if the path is fully normalized, and == 0 for each normalization
         *    step. This may be combined with CHASE_NONEXISTENT, in which case 1 is returned when a component
         *    is not found.
         *
         * 4. With CHASE_SAFE: in this case the path must not contain unsafe transitions, i.e. transitions from
         *    unprivileged to privileged files or directories. In such cases the return value is -ENOLINK. If
         *    CHASE_WARN is also set, a warning describing the unsafe transition is emitted. CHASE_WARN cannot
         *    be used in PID 1.
         *
         * 5. With CHASE_NO_AUTOFS: in this case if an autofs mount point is encountered, path normalization
         *    is aborted and -EREMOTE is returned. If CHASE_WARN is also set, a warning showing the path of
         *    the mount point is emitted. CHASE_WARN cannot be used in PID 1.
         */

        if (!(flags & (CHASE_AT_RESOLVE_IN_ROOT|CHASE_NONEXISTENT|CHASE_NO_AUTOFS|CHASE_SAFE|CHASE_STEP)) &&
                !ret_path && ret_fd) {

                /* Shortcut the ret_fd case if the caller isn't interested in the actual path and has no root set
                 * and doesn't care about any of the other special features we provide either. */
                r = openat(dir_fd, path, O_PATH|O_CLOEXEC|((flags & CHASE_NOFOLLOW) ? O_NOFOLLOW : 0));
                if (r < 0)
                        return -errno;

                *ret_fd = r;
                return 0;
        }

        if (FLAGS_SET(flags, CHASE_AT_RESOLVE_IN_ROOT) || dir_fd >= 0) {
                buffer = strdup(path);
                if (!buffer)
                        return -ENOMEM;
        } else {
                r = path_make_absolute_cwd(path, &buffer);
                if (r < 0)
                        return r;
        }

        /* If we get a valid directory fd and an absolute path, we need to return a relative path because
         * openat() like functions generally ignore the directory fd if provided with an absolute path. On
         * the other hand, if we get an invalid directory fd (which includes AT_FDCWD in this case) and an
         * absolute path, we need to return an absolute path because if we don't, if the caller is using
         * AT_FDCWD and passes the returned relative path to another openat() like function, it would be
         * resolved relative to the current working directory instead of to "/" like it's supposed to be. And
         * if the caller is using -1 as the directory fd, passing the returned relative path to another
         * openat() like function would result in an EINVAL error.
         */

        bool need_absolute = !FLAGS_SET(flags, CHASE_AT_RESOLVE_IN_ROOT) && dir_fd == AT_FDCWD &&
                        path_is_absolute(path);
        if (need_absolute) {
                done = strdup("/");
                if (!done)
                        return -ENOMEM;
        }

        if (FLAGS_SET(flags, CHASE_AT_RESOLVE_IN_ROOT))
                root_fd = fd_reopen(dir_fd, O_CLOEXEC|O_DIRECTORY|O_PATH);
        else
                root_fd = RET_NERRNO(open("/", O_CLOEXEC|O_DIRECTORY|O_PATH));

        if (root_fd < 0)
                return root_fd;

        /* This is our iterator file descriptor which we initialize with a copy of the root file descriptor. */
        if (!FLAGS_SET(flags, CHASE_AT_RESOLVE_IN_ROOT) && dir_fd == AT_FDCWD && path_is_absolute(path))
                fd = RET_NERRNO(open("/", O_CLOEXEC|O_DIRECTORY|O_PATH));
        else
                fd = fd_reopen(dir_fd, O_CLOEXEC|O_DIRECTORY|O_PATH);
        if (fd < 0)
                return fd;

        if (flags & CHASE_SAFE)
                if (fstat(fd, &previous_stat) < 0)
                        return -errno;

        if (flags & CHASE_TRAIL_SLASH)
                append_trail_slash = endswith(buffer, "/") || endswith(buffer, "/.");

        for (todo = buffer;;) {
                _cleanup_free_ char *first = NULL;
                _cleanup_close_ int child = -1;
                struct stat st;
                const char *e;

                r = path_find_first_component(&todo, /* accept_dot_dot= */ true, &e);
                if (r < 0)
                        return r;
                if (r == 0) { /* We reached the end. */
                        if (append_trail_slash)
                                if (!strextend(&done, "/"))
                                        return -ENOMEM;
                        break;
                }

                first = strndup(e, r);
                if (!first)
                        return -ENOMEM;

                /* Two dots? Then chop off the last bit of what we already found out. */
                if (path_equal(first, "..")) {
                        _cleanup_free_ char *parent = NULL;
                        _cleanup_close_ int fd_parent = -1;

                        /* If we already are at the top, then going up will not change anything. This is
                         * in-line with how the kernel handles this. */
                        if (empty_or_root(done) &&
                            (FLAGS_SET(flags, CHASE_AT_RESOLVE_IN_ROOT) ||
                             (dir_fd == AT_FDCWD && path_is_absolute(path))))
                                continue;

                        r = path_extract_directory(done, &parent);
                        if (r >= 0 || r == -EDESTADDRREQ)
                                free_and_replace(done, parent);
                        else if (IN_SET(r, -EINVAL, -EADDRNOTAVAIL)) {
                                /* If we're at the top of "dir_fd", start appending ".." to "done". */
                                if (!path_extend(&done, ".."))
                                        return -ENOMEM;
                        } else
                                return r;

                        if (flags & CHASE_STEP)
                                goto chased_one;

                        fd_parent = openat(fd, "..", O_CLOEXEC|O_NOFOLLOW|O_PATH);
                        if (fd_parent < 0)
                                return -errno;

                        if (flags & CHASE_SAFE) {
                                if (fstat(fd_parent, &st) < 0)
                                        return -errno;

                                if (unsafe_transition(&previous_stat, &st))
                                        return log_unsafe_transition(fd, fd_parent, path, flags);

                                previous_stat = st;
                        }

                        close_and_replace(fd, fd_parent);

                        continue;
                }

                /* Otherwise let's see what this is. */
                child = openat(fd, first, O_CLOEXEC|O_NOFOLLOW|O_PATH);
                if (child < 0) {
                        if (errno == ENOENT &&
                            (flags & CHASE_NONEXISTENT) &&
                            (isempty(todo) || path_is_safe(todo))) {
                                /* If CHASE_NONEXISTENT is set, and the path does not exist, then
                                 * that's OK, return what we got so far. But don't allow this if the
                                 * remaining path contains "../" or something else weird. */

                                if (!path_extend(&done, first, todo))
                                        return -ENOMEM;

                                exists = false;
                                break;
                        }

                        return -errno;
                }

                if (fstat(child, &st) < 0)
                        return -errno;
                if ((flags & CHASE_SAFE) &&
                    unsafe_transition(&previous_stat, &st))
                        return log_unsafe_transition(fd, child, path, flags);

                previous_stat = st;

                if ((flags & CHASE_NO_AUTOFS) &&
                    fd_is_fs_type(child, AUTOFS_SUPER_MAGIC) > 0)
                        return log_autofs_mount_point(child, path, flags);

                if (S_ISLNK(st.st_mode) && !((flags & CHASE_NOFOLLOW) && isempty(todo))) {
                        _cleanup_free_ char *destination = NULL;

                        /* This is a symlink, in this case read the destination. But let's make sure we
                         * don't follow symlinks without bounds. */
                        if (--max_follow <= 0)
                                return -ELOOP;

                        r = readlinkat_malloc(fd, first, &destination);
                        if (r < 0)
                                return r;
                        if (isempty(destination))
                                return -EINVAL;

                        if (path_is_absolute(destination)) {

                                /* An absolute destination. Start the loop from the beginning, but use the
                                 * root file descriptor as base. */

                                safe_close(fd);
                                fd = fd_reopen(root_fd, O_CLOEXEC|O_PATH|O_DIRECTORY);
                                if (fd < 0)
                                        return fd;

                                if (flags & CHASE_SAFE) {
                                        if (fstat(fd, &st) < 0)
                                                return -errno;

                                        if (unsafe_transition(&previous_stat, &st))
                                                return log_unsafe_transition(child, fd, path, flags);

                                        previous_stat = st;
                                }

                                r = free_and_strdup(&done, need_absolute ? "/" : NULL);
                                if (r < 0)
                                        return r;
                        }

                        /* Prefix what's left to do with what we just read, and start the loop again, but
                         * remain in the current directory. */
                        if (!path_extend(&destination, todo))
                                return -ENOMEM;

                        free_and_replace(buffer, destination);
                        todo = buffer;

                        if (flags & CHASE_STEP)
                                goto chased_one;

                        continue;
                }

                /* If this is not a symlink, then let's just add the name we read to what we already verified. */
                if (!path_extend(&done, first))
                        return -ENOMEM;

                /* And iterate again, but go one directory further down. */
                close_and_replace(fd, child);
        }

        if (ret_path) {
                if (!FLAGS_SET(flags, CHASE_AT_RESOLVE_IN_ROOT) && dir_fd == AT_FDCWD && !path_is_absolute(path)) {
                        /* Return a relative path if we were provided with one when AT_FDCWD was used. */
                        r = path_make_relative_cwd(done, &done);
                        if (r < 0)
                                return r;
                }

                *ret_path = TAKE_PTR(done);
        }

        if (ret_fd) {
                /* Return the O_PATH fd we currently are looking to the caller. It can translate it to a
                 * proper fd by opening /proc/self/fd/xyz. */

                assert(fd >= 0);
                *ret_fd = TAKE_FD(fd);
        }

        if (flags & CHASE_STEP)
                return 1;

        return exists;

chased_one:
        if (ret_path) {
                const char *e;

                if (!FLAGS_SET(flags, CHASE_AT_RESOLVE_IN_ROOT) && dir_fd == AT_FDCWD && !path_is_absolute(path)) {
                        /* Return a relative path if we were provided with one when AT_FDCWD was used. */
                        r = path_make_relative_cwd(done, &done);
                        if (r < 0)
                                return r;
                }

                /* todo may contain slashes at the beginning. */
                r = path_find_first_component(&todo, /* accept_dot_dot= */ true, &e);
                if (r < 0)
                        return r;
                if (r == 0)
                        *ret_path = TAKE_PTR(done);
                else {
                        char *c;

                        c = path_join(done, e);
                        if (!c)
                                return -ENOMEM;

                        *ret_path = c;
                }
        }

        return 0;
}

int chase_symlinks(
                const char *path,
                const char *original_root,
                ChaseSymlinksFlags flags,
                char **ret_path,
                int *ret_fd) {

        _cleanup_free_ char *root = NULL, *absolute = NULL, *p = NULL;
        _cleanup_close_ int fd = -1, pfd = -1;
        int r;

        assert(path);

        if (isempty(path))
                return -EINVAL;

        /* A root directory of "/" or "" is identical to none */
        if (empty_or_root(original_root))
                original_root = NULL;

        if (original_root) {
                r = path_make_absolute_cwd(original_root, &root);
                if (r < 0)
                        return r;

                /* Simplify the root directory, so that it has no duplicate slashes and nothing at the
                 * end. While we won't resolve the root path we still simplify it. Note that dropping the
                 * trailing slash should not change behaviour, since when opening it we specify O_DIRECTORY
                 * anyway. Moreover at the end of this function after processing everything we'll always turn
                 * the empty string back to "/". */
                delete_trailing_chars(root, "/");
                path_simplify(root);

                if (flags & CHASE_PREFIX_ROOT) {
                        absolute = path_join(root, path);
                        if (!absolute)
                                return -ENOMEM;
                }
        }

        if (!absolute) {
                r = path_make_absolute_cwd(path, &absolute);
                if (r < 0)
                        return r;
        }

        if (root) {
                path = path_startswith(absolute, root);
                if (!path)
                        return log_full_errno(flags & CHASE_WARN ? LOG_WARNING : LOG_DEBUG,
                                              SYNTHETIC_ERRNO(ECHRNG),
                                              "Specified path '%s' is outside of specified root directory '%s', refusing to resolve.",
                                              absolute, root);
        }

        if (root) {
                fd = open(root, O_CLOEXEC|O_DIRECTORY|O_PATH);
                if (fd < 0)
                        return -errno;

                flags |= CHASE_AT_RESOLVE_IN_ROOT;
        } else
                fd = AT_FDCWD;

        r = chase_symlinks_at(fd, path, flags & ~CHASE_PREFIX_ROOT, ret_path ? &p : NULL, ret_fd ? &pfd : NULL);
        if (r < 0)
                return r;

        if (ret_path) {
                char *q = path_join(root, p);
                if (!q)
                        return -ENOMEM;

                *ret_path = TAKE_PTR(q);
        }

        if (ret_fd)
                *ret_fd = TAKE_FD(pfd);

        return r;
}

int chase_symlinks_and_open(
                const char *path,
                const char *root,
                ChaseSymlinksFlags chase_flags,
                int open_flags,
                char **ret_path) {

        _cleanup_close_ int path_fd = -1;
        _cleanup_free_ char *p = NULL;
        int r;

        if (chase_flags & (CHASE_NONEXISTENT|CHASE_STEP))
                return -EINVAL;

        if (empty_or_root(root) && !ret_path && (chase_flags & (CHASE_NO_AUTOFS|CHASE_SAFE)) == 0) {
                /* Shortcut this call if none of the special features of this call are requested */
                r = open(path, open_flags | (FLAGS_SET(chase_flags, CHASE_NOFOLLOW) ? O_NOFOLLOW : 0));
                if (r < 0)
                        return -errno;

                return r;
        }

        r = chase_symlinks(path, root, chase_flags, ret_path ? &p : NULL, &path_fd);
        if (r < 0)
                return r;
        assert(path_fd >= 0);

        r = fd_reopen(path_fd, open_flags);
        if (r < 0)
                return r;

        if (ret_path)
                *ret_path = TAKE_PTR(p);

        return r;
}

int chase_symlinks_and_opendir(
                const char *path,
                const char *root,
                ChaseSymlinksFlags chase_flags,
                char **ret_path,
                DIR **ret_dir) {

        _cleanup_close_ int path_fd = -1;
        _cleanup_free_ char *p = NULL;
        DIR *d;
        int r;

        if (!ret_dir)
                return -EINVAL;
        if (chase_flags & (CHASE_NONEXISTENT|CHASE_STEP))
                return -EINVAL;

        if (empty_or_root(root) && !ret_path && (chase_flags & (CHASE_NO_AUTOFS|CHASE_SAFE)) == 0) {
                /* Shortcut this call if none of the special features of this call are requested */
                d = opendir(path);
                if (!d)
                        return -errno;

                *ret_dir = d;
                return 0;
        }

        r = chase_symlinks(path, root, chase_flags, ret_path ? &p : NULL, &path_fd);
        if (r < 0)
                return r;
        assert(path_fd >= 0);

        d = opendir(FORMAT_PROC_FD_PATH(path_fd));
        if (!d)
                return -errno;

        if (ret_path)
                *ret_path = TAKE_PTR(p);

        *ret_dir = d;
        return 0;
}

int chase_symlinks_and_stat(
                const char *path,
                const char *root,
                ChaseSymlinksFlags chase_flags,
                char **ret_path,
                struct stat *ret_stat,
                int *ret_fd) {

        _cleanup_close_ int path_fd = -1;
        _cleanup_free_ char *p = NULL;
        int r;

        assert(path);
        assert(ret_stat);

        if (chase_flags & (CHASE_NONEXISTENT|CHASE_STEP))
                return -EINVAL;

        if (empty_or_root(root) && !ret_path && (chase_flags & (CHASE_NO_AUTOFS|CHASE_SAFE)) == 0 && !ret_fd) {
                /* Shortcut this call if none of the special features of this call are requested */

                if (fstatat(AT_FDCWD, path, ret_stat, FLAGS_SET(chase_flags, CHASE_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0) < 0)
                        return -errno;

                return 1;
        }

        r = chase_symlinks(path, root, chase_flags, ret_path ? &p : NULL, &path_fd);
        if (r < 0)
                return r;
        assert(path_fd >= 0);

        if (fstat(path_fd, ret_stat) < 0)
                return -errno;

        if (ret_path)
                *ret_path = TAKE_PTR(p);
        if (ret_fd)
                *ret_fd = TAKE_FD(path_fd);

        return 1;
}

int chase_symlinks_and_access(
                const char *path,
                const char *root,
                ChaseSymlinksFlags chase_flags,
                int access_mode,
                char **ret_path,
                int *ret_fd) {

        _cleanup_close_ int path_fd = -1;
        _cleanup_free_ char *p = NULL;
        int r;

        assert(path);

        if (chase_flags & (CHASE_NONEXISTENT|CHASE_STEP))
                return -EINVAL;

        if (empty_or_root(root) && !ret_path && (chase_flags & (CHASE_NO_AUTOFS|CHASE_SAFE)) == 0 && !ret_fd) {
                /* Shortcut this call if none of the special features of this call are requested */

                if (faccessat(AT_FDCWD, path, access_mode, FLAGS_SET(chase_flags, CHASE_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0) < 0)
                        return -errno;

                return 1;
        }

        r = chase_symlinks(path, root, chase_flags, ret_path ? &p : NULL, &path_fd);
        if (r < 0)
                return r;
        assert(path_fd >= 0);

        r = access_fd(path_fd, access_mode);
        if (r < 0)
                return r;

        if (ret_path)
                *ret_path = TAKE_PTR(p);
        if (ret_fd)
                *ret_fd = TAKE_FD(path_fd);

        return 1;
}

int chase_symlinks_and_fopen_unlocked(
                const char *path,
                const char *root,
                ChaseSymlinksFlags chase_flags,
                const char *open_flags,
                char **ret_path,
                FILE **ret_file) {

        _cleanup_free_ char *final_path = NULL;
        _cleanup_close_ int fd = -1;
        int mode_flags, r;

        assert(path);
        assert(open_flags);
        assert(ret_file);

        mode_flags = fopen_mode_to_flags(open_flags);
        if (mode_flags < 0)
                return mode_flags;

        fd = chase_symlinks_and_open(path, root, chase_flags, mode_flags, ret_path ? &final_path : NULL);
        if (fd < 0)
                return fd;

        r = take_fdopen_unlocked(&fd, open_flags, ret_file);
        if (r < 0)
                return r;

        if (ret_path)
                *ret_path = TAKE_PTR(final_path);

        return 0;
}
