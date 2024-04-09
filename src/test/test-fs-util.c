/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/file.h>
#include <unistd.h>

#include "alloc-util.h"
#include "copy.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "macro.h"
#include "mkdir.h"
#include "path-util.h"
#include "process-util.h"
#include "random-util.h"
#include "rm-rf.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "sync-util.h"
#include "tests.h"
#include "tmpfile-util.h"
#include "umask-util.h"
#include "user-util.h"
#include "virt.h"

static const char *arg_test_dir = NULL;

TEST(readlink_and_make_absolute) {
        const char *tempdir, *name, *name2, *name_alias;
        _cleanup_free_ char *r1 = NULL, *r2 = NULL, *pwd = NULL;

        tempdir = strjoina(arg_test_dir ?: "/tmp", "/test-readlink_and_make_absolute");
        name = strjoina(tempdir, "/original");
        name2 = "test-readlink_and_make_absolute/original";
        name_alias = strjoina(arg_test_dir ?: "/tmp", "/test-readlink_and_make_absolute-alias");

        ASSERT_OK(mkdir_safe(tempdir, 0755, getuid(), getgid(), MKDIR_WARN_MODE));
        ASSERT_OK(touch(name));

        if (symlink(name, name_alias) < 0) {
                ASSERT_TRUE(IN_SET(errno, EINVAL, ENOSYS, ENOTTY, EPERM));
                log_tests_skipped_errno(errno, "symlink() not possible");
        } else {
                assert_se(readlink_and_make_absolute(name_alias, &r1) >= 0);
                ASSERT_TRUE(streq(r1, name));
                ASSERT_OK(unlink(name_alias));

                assert_se(safe_getcwd(&pwd) >= 0);

                ASSERT_OK(chdir(tempdir));
                ASSERT_OK(symlink(name2, name_alias));
                assert_se(readlink_and_make_absolute(name_alias, &r2) >= 0);
                ASSERT_TRUE(streq(r2, name));
                ASSERT_OK(unlink(name_alias));

                ASSERT_OK(chdir(pwd));
        }

        assert_se(rm_rf(tempdir, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);
}

TEST(get_files_in_directory) {
        _cleanup_strv_free_ char **l = NULL, **t = NULL;

        assert_se(get_files_in_directory(arg_test_dir ?: "/tmp", &l) >= 0);
        assert_se(get_files_in_directory(".", &t) >= 0);
        ASSERT_OK(get_files_in_directory(".", NULL));
}

TEST(var_tmp) {
        _cleanup_free_ char *tmpdir_backup = NULL, *temp_backup = NULL, *tmp_backup = NULL;
        const char *tmp_dir = NULL, *t;

        t = getenv("TMPDIR");
        if (t) {
                tmpdir_backup = strdup(t);
                ASSERT_TRUE(tmpdir_backup);
        }

        t = getenv("TEMP");
        if (t) {
                temp_backup = strdup(t);
                ASSERT_TRUE(temp_backup);
        }

        t = getenv("TMP");
        if (t) {
                tmp_backup = strdup(t);
                ASSERT_TRUE(tmp_backup);
        }

        ASSERT_OK(unsetenv("TMPDIR"));
        ASSERT_OK(unsetenv("TEMP"));
        ASSERT_OK(unsetenv("TMP"));

        assert_se(var_tmp_dir(&tmp_dir) >= 0);
        assert_se(streq(tmp_dir, "/var/tmp"));

        assert_se(setenv("TMPDIR", "/tmp", true) >= 0);
        assert_se(streq(getenv("TMPDIR"), "/tmp"));

        assert_se(var_tmp_dir(&tmp_dir) >= 0);
        assert_se(streq(tmp_dir, "/tmp"));

        assert_se(setenv("TMPDIR", "/88_does_not_exist_88", true) >= 0);
        assert_se(streq(getenv("TMPDIR"), "/88_does_not_exist_88"));

        assert_se(var_tmp_dir(&tmp_dir) >= 0);
        assert_se(streq(tmp_dir, "/var/tmp"));

        if (tmpdir_backup)  {
                ASSERT_OK(setenv("TMPDIR", tmpdir_backup, true));
                ASSERT_TRUE(streq(getenv("TMPDIR"), tmpdir_backup));
        }

        if (temp_backup)  {
                ASSERT_OK(setenv("TEMP", temp_backup, true));
                ASSERT_TRUE(streq(getenv("TEMP"), temp_backup));
        }

        if (tmp_backup)  {
                ASSERT_OK(setenv("TMP", tmp_backup, true));
                ASSERT_TRUE(streq(getenv("TMP"), tmp_backup));
        }
}

TEST(dot_or_dot_dot) {
        ASSERT_FALSE(dot_or_dot_dot(NULL));
        ASSERT_FALSE(dot_or_dot_dot(""));
        ASSERT_FALSE(dot_or_dot_dot("xxx"));
        ASSERT_TRUE(dot_or_dot_dot("."));
        ASSERT_TRUE(dot_or_dot_dot(".."));
        ASSERT_FALSE(dot_or_dot_dot(".foo"));
        ASSERT_FALSE(dot_or_dot_dot("..foo"));
}

TEST(access_fd) {
        _cleanup_(rmdir_and_freep) char *p = NULL;
        _cleanup_close_ int fd = -EBADF;
        const char *a;

        a = strjoina(arg_test_dir ?: "/tmp", "/access-fd.XXXXXX");
        assert_se(mkdtemp_malloc(a, &p) >= 0);

        fd = open(p, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        ASSERT_OK(fd);

        ASSERT_OK(access_fd(fd, R_OK));
        ASSERT_OK(access_fd(fd, F_OK));
        ASSERT_OK(access_fd(fd, W_OK));

        ASSERT_OK(fchmod(fd, 0000));

        ASSERT_OK(access_fd(fd, F_OK));

        if (geteuid() == 0) {
                ASSERT_OK(access_fd(fd, R_OK));
                ASSERT_OK(access_fd(fd, W_OK));
        } else {
                assert_se(access_fd(fd, R_OK) == -EACCES);
                assert_se(access_fd(fd, W_OK) == -EACCES);
        }
}

TEST(touch_file) {
        uid_t test_uid, test_gid;
        _cleanup_(rm_rf_physical_and_freep) char *p = NULL;
        struct stat st;
        const char *a;
        usec_t test_mtime;
        int r;

        test_uid = geteuid() == 0 ? 65534 : getuid();
        test_gid = geteuid() == 0 ? 65534 : getgid();

        test_mtime = usec_sub_unsigned(now(CLOCK_REALTIME), USEC_PER_WEEK);

        a = strjoina(arg_test_dir ?: "/dev/shm", "/touch-file-XXXXXX");
        assert_se(mkdtemp_malloc(a, &p) >= 0);

        a = strjoina(p, "/regular");
        r = touch_file(a, false, test_mtime, test_uid, test_gid, 0640);
        if (r < 0) {
                assert_se(IN_SET(r, -EINVAL, -ENOSYS, -ENOTTY, -EPERM));
                log_tests_skipped_errno(errno, "touch_file() not possible");
                return;
        }

        assert_se(lstat(a, &st) >= 0);
        assert_se(st.st_uid == test_uid);
        assert_se(st.st_gid == test_gid);
        ASSERT_TRUE(S_ISREG(st.st_mode));
        assert_se((st.st_mode & 0777) == 0640);
        assert_se(timespec_load(&st.st_mtim) == test_mtime);

        a = strjoina(p, "/dir");
        ASSERT_OK(mkdir(a, 0775));
        ASSERT_OK(touch_file(a, false, test_mtime, test_uid, test_gid, 0640));
        assert_se(lstat(a, &st) >= 0);
        assert_se(st.st_uid == test_uid);
        assert_se(st.st_gid == test_gid);
        ASSERT_TRUE(S_ISDIR(st.st_mode));
        assert_se((st.st_mode & 0777) == 0640);
        assert_se(timespec_load(&st.st_mtim) == test_mtime);

        a = strjoina(p, "/fifo");
        ASSERT_OK(mkfifo(a, 0775));
        ASSERT_OK(touch_file(a, false, test_mtime, test_uid, test_gid, 0640));
        assert_se(lstat(a, &st) >= 0);
        assert_se(st.st_uid == test_uid);
        assert_se(st.st_gid == test_gid);
        ASSERT_TRUE(S_ISFIFO(st.st_mode));
        assert_se((st.st_mode & 0777) == 0640);
        assert_se(timespec_load(&st.st_mtim) == test_mtime);

        a = strjoina(p, "/sock");
        assert_se(mknod(a, 0775 | S_IFSOCK, 0) >= 0);
        ASSERT_OK(touch_file(a, false, test_mtime, test_uid, test_gid, 0640));
        assert_se(lstat(a, &st) >= 0);
        assert_se(st.st_uid == test_uid);
        assert_se(st.st_gid == test_gid);
        ASSERT_TRUE(S_ISSOCK(st.st_mode));
        assert_se((st.st_mode & 0777) == 0640);
        assert_se(timespec_load(&st.st_mtim) == test_mtime);

        if (geteuid() == 0) {
                a = strjoina(p, "/bdev");
                r = mknod(a, 0775 | S_IFBLK, makedev(0, 0));
                if (r < 0 && errno == EPERM && detect_container() > 0) {
                        log_notice("Running in unprivileged container? Skipping remaining tests in %s", __func__);
                        return;
                }
                ASSERT_OK(r);
                ASSERT_OK(touch_file(a, false, test_mtime, test_uid, test_gid, 0640));
                assert_se(lstat(a, &st) >= 0);
                assert_se(st.st_uid == test_uid);
                assert_se(st.st_gid == test_gid);
                ASSERT_TRUE(S_ISBLK(st.st_mode));
                assert_se((st.st_mode & 0777) == 0640);
                assert_se(timespec_load(&st.st_mtim) == test_mtime);

                a = strjoina(p, "/cdev");
                assert_se(mknod(a, 0775 | S_IFCHR, makedev(0, 0)) >= 0);
                ASSERT_OK(touch_file(a, false, test_mtime, test_uid, test_gid, 0640));
                assert_se(lstat(a, &st) >= 0);
                assert_se(st.st_uid == test_uid);
                assert_se(st.st_gid == test_gid);
                ASSERT_TRUE(S_ISCHR(st.st_mode));
                assert_se((st.st_mode & 0777) == 0640);
                assert_se(timespec_load(&st.st_mtim) == test_mtime);
        }

        a = strjoina(p, "/lnk");
        ASSERT_OK(symlink("target", a));
        ASSERT_OK(touch_file(a, false, test_mtime, test_uid, test_gid, 0640));
        assert_se(lstat(a, &st) >= 0);
        assert_se(st.st_uid == test_uid);
        assert_se(st.st_gid == test_gid);
        ASSERT_TRUE(S_ISLNK(st.st_mode));
        assert_se(timespec_load(&st.st_mtim) == test_mtime);
}

TEST(unlinkat_deallocate) {
        _cleanup_free_ char *p = NULL;
        _cleanup_close_ int fd = -EBADF;
        struct stat st;

        assert_se(tempfn_random_child(arg_test_dir, "unlink-deallocation", &p) >= 0);

        fd = open(p, O_WRONLY|O_CLOEXEC|O_CREAT|O_EXCL, 0600);
        ASSERT_OK(fd);

        assert_se(write(fd, "hallo\n", 6) == 6);

        assert_se(fstat(fd, &st) >= 0);
        ASSERT_EQ(st.st_size, 6);
        ASSERT_GT(st.st_blocks, 0);
        ASSERT_EQ(st.st_nlink, 1u);

        ASSERT_OK(unlinkat_deallocate(AT_FDCWD, p, UNLINK_ERASE));

        assert_se(fstat(fd, &st) >= 0);
        ASSERT_TRUE(IN_SET(st.st_size, 0, 6)); /* depending on whether hole punching worked the size will be 6
                                                (it worked) or 0 (we had to resort to truncation) */
        ASSERT_EQ(st.st_blocks, 0u);
        ASSERT_EQ(st.st_nlink, 0u);
}

TEST(fsync_directory_of_file) {
        _cleanup_close_ int fd = -EBADF;

        fd = open_tmpfile_unlinkable(arg_test_dir, O_RDWR);
        ASSERT_OK(fd);

        ASSERT_OK(fsync_directory_of_file(fd));
}

TEST(rename_noreplace) {
        static const char* const table[] = {
                "/reg",
                "/dir",
                "/fifo",
                "/socket",
                "/symlink",
                NULL
        };

        _cleanup_(rm_rf_physical_and_freep) char *z = NULL;
        const char *j = NULL;

        if (arg_test_dir)
                j = strjoina(arg_test_dir, "/testXXXXXX");
        assert_se(mkdtemp_malloc(j, &z) >= 0);

        j = strjoina(z, table[0]);
        ASSERT_OK(touch(j));

        j = strjoina(z, table[1]);
        ASSERT_OK(mkdir(j, 0777));

        j = strjoina(z, table[2]);
        (void) mkfifo(j, 0777);

        j = strjoina(z, table[3]);
        (void) mknod(j, S_IFSOCK | 0777, 0);

        j = strjoina(z, table[4]);
        (void) symlink("foobar", j);

        STRV_FOREACH(a, table) {
                _cleanup_free_ char *x = NULL, *y = NULL;

                x = strjoin(z, *a);
                ASSERT_TRUE(x);

                if (access(x, F_OK) < 0) {
                        assert_se(errno == ENOENT);
                        continue;
                }

                STRV_FOREACH(b, table) {
                        _cleanup_free_ char *w = NULL;

                        w = strjoin(z, *b);
                        ASSERT_TRUE(w);

                        if (access(w, F_OK) < 0) {
                                assert_se(errno == ENOENT);
                                continue;
                        }

                        assert_se(rename_noreplace(AT_FDCWD, x, AT_FDCWD, w) == -EEXIST);
                }

                y = strjoin(z, "/somethingelse");
                ASSERT_TRUE(y);

                ASSERT_OK(rename_noreplace(AT_FDCWD, x, AT_FDCWD, y));
                ASSERT_OK(rename_noreplace(AT_FDCWD, y, AT_FDCWD, x));
        }
}

TEST(chmod_and_chown) {
        _cleanup_(rm_rf_physical_and_freep) char *d = NULL;
        struct stat st;
        const char *p;

        if (geteuid() != 0)
                return;

        BLOCK_WITH_UMASK(0000);

        assert_se(mkdtemp_malloc(NULL, &d) >= 0);

        p = strjoina(d, "/reg");
        assert_se(mknod(p, S_IFREG | 0123, 0) >= 0);

        assert_se(chmod_and_chown(p, S_IFREG | 0321, 1, 2) >= 0);
        assert_se(chmod_and_chown(p, S_IFDIR | 0555, 3, 4) == -EINVAL);

        assert_se(lstat(p, &st) >= 0);
        ASSERT_TRUE(S_ISREG(st.st_mode));
        assert_se((st.st_mode & 07777) == 0321);

        p = strjoina(d, "/dir");
        ASSERT_OK(mkdir(p, 0123));

        assert_se(chmod_and_chown(p, S_IFDIR | 0321, 1, 2) >= 0);
        assert_se(chmod_and_chown(p, S_IFREG | 0555, 3, 4) == -EINVAL);

        assert_se(lstat(p, &st) >= 0);
        ASSERT_TRUE(S_ISDIR(st.st_mode));
        assert_se((st.st_mode & 07777) == 0321);

        p = strjoina(d, "/lnk");
        ASSERT_OK(symlink("idontexist", p));

        assert_se(chmod_and_chown(p, S_IFLNK | 0321, 1, 2) >= 0);
        assert_se(chmod_and_chown(p, S_IFREG | 0555, 3, 4) == -EINVAL);
        assert_se(chmod_and_chown(p, S_IFDIR | 0555, 3, 4) == -EINVAL);

        assert_se(lstat(p, &st) >= 0);
        ASSERT_TRUE(S_ISLNK(st.st_mode));
}

static void create_binary_file(const char *p, const void *data, size_t l) {
        _cleanup_close_ int fd = -EBADF;

        fd = open(p, O_CREAT|O_WRONLY|O_EXCL|O_CLOEXEC, 0600);
        ASSERT_OK(fd);
        assert_se(write(fd, data, l) == (ssize_t) l);
}

TEST(conservative_rename) {
        _cleanup_(unlink_and_freep) char *p = NULL;
        _cleanup_free_ char *q = NULL;
        size_t l = 16*1024 + random_u64() % (32 * 1024); /* some randomly sized buffer 16k…48k */
        uint8_t buffer[l+1];

        random_bytes(buffer, l);

        assert_se(tempfn_random_child(NULL, NULL, &p) >= 0);
        create_binary_file(p, buffer, l);

        assert_se(tempfn_random_child(NULL, NULL, &q) >= 0);

        /* Check that the hardlinked "copy" is detected */
        ASSERT_OK(link(p, q));
        ASSERT_EQ(conservative_renameat(AT_FDCWD, q, AT_FDCWD, p), 0);
        assert_se(access(q, F_OK) < 0 && errno == ENOENT);

        /* Check that a manual copy is detected */
        ASSERT_OK(copy_file(p, q, 0, MODE_INVALID, COPY_REFLINK));
        ASSERT_EQ(conservative_renameat(AT_FDCWD, q, AT_FDCWD, p), 0);
        assert_se(access(q, F_OK) < 0 && errno == ENOENT);

        /* Check that a manual new writeout is also detected */
        create_binary_file(q, buffer, l);
        ASSERT_EQ(conservative_renameat(AT_FDCWD, q, AT_FDCWD, p), 0);
        assert_se(access(q, F_OK) < 0 && errno == ENOENT);

        /* Check that a minimally changed version is detected */
        buffer[47] = ~buffer[47];
        create_binary_file(q, buffer, l);
        ASSERT_GT(conservative_renameat(AT_FDCWD, q, AT_FDCWD, p), 0);
        assert_se(access(q, F_OK) < 0 && errno == ENOENT);

        /* Check that this really is new updated version */
        create_binary_file(q, buffer, l);
        ASSERT_EQ(conservative_renameat(AT_FDCWD, q, AT_FDCWD, p), 0);
        assert_se(access(q, F_OK) < 0 && errno == ENOENT);

        /* Make sure we detect extended files */
        buffer[l++] = 47;
        create_binary_file(q, buffer, l);
        ASSERT_GT(conservative_renameat(AT_FDCWD, q, AT_FDCWD, p), 0);
        assert_se(access(q, F_OK) < 0 && errno == ENOENT);

        /* Make sure we detect truncated files */
        l--;
        create_binary_file(q, buffer, l);
        ASSERT_GT(conservative_renameat(AT_FDCWD, q, AT_FDCWD, p), 0);
        assert_se(access(q, F_OK) < 0 && errno == ENOENT);
}

static void test_rmdir_parents_one(
                const char *prefix,
                const char *path,
                const char *stop,
                int expected,
                const char *test_exist,
                const char *test_nonexist_subdir) {

        const char *p, *s;

        log_debug("/* %s(%s, %s) */", __func__, path, stop);

        p = strjoina(prefix, path);
        s = strjoina(prefix, stop);

        if (expected >= 0)
                ASSERT_OK(mkdir_parents(p, 0700));

        assert_se(rmdir_parents(p, s) == expected);

        if (expected >= 0) {
                const char *e, *f;

                e = strjoina(prefix, test_exist);
                f = strjoina(e, test_nonexist_subdir);

                ASSERT_OK(access(e, F_OK));
                ASSERT_LT(access(f, F_OK), 0);
        }
}

TEST(rmdir_parents) {
        char *temp;

        temp = strjoina(arg_test_dir ?: "/tmp", "/test-rmdir.XXXXXX");
        ASSERT_TRUE(mkdtemp(temp));

        test_rmdir_parents_one(temp, "/aaa/../hoge/foo", "/hoge/foo", -EINVAL, NULL, NULL);
        test_rmdir_parents_one(temp, "/aaa/bbb/ccc", "/hoge/../aaa", -EINVAL, NULL, NULL);

        test_rmdir_parents_one(temp, "/aaa/bbb/ccc/ddd/eee", "/aaa/bbb/ccc/ddd", 0, "/aaa/bbb/ccc/ddd", "/eee");
        test_rmdir_parents_one(temp, "/aaa/bbb/ccc/ddd/eee", "/aaa/bbb/ccc", 0, "/aaa/bbb/ccc", "/ddd");
        test_rmdir_parents_one(temp, "/aaa/bbb/ccc/ddd/eee", "/aaa/bbb", 0, "/aaa/bbb", "/ccc");
        test_rmdir_parents_one(temp, "/aaa/bbb/ccc/ddd/eee", "/aaa", 0, "/aaa", "/bbb");
        test_rmdir_parents_one(temp, "/aaa/bbb/ccc/ddd/eee", "/", 0, "/", "/aaa");

        test_rmdir_parents_one(temp, "/aaa/bbb/ccc/ddd/eee", "/aaa/hoge/foo", 0, "/aaa", "/bbb");
        test_rmdir_parents_one(temp, "/aaa////bbb/.//ccc//ddd/eee///./.", "///././aaa/.", 0, "/aaa", "/bbb");

        assert_se(rm_rf(temp, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);
}

static void test_parse_cifs_service_one(const char *f, const char *h, const char *s, const char *d, int ret) {
        _cleanup_free_ char *a = NULL, *b = NULL, *c = NULL;

        assert_se(parse_cifs_service(f, &a, &b, &c) == ret);
        ASSERT_TRUE(streq_ptr(a, h));
        ASSERT_TRUE(streq_ptr(b, s));
        ASSERT_TRUE(streq_ptr(c, d));
}

TEST(parse_cifs_service) {
        test_parse_cifs_service_one("//foo/bar/baz", "foo", "bar", "baz", 0);
        test_parse_cifs_service_one("\\\\foo\\bar\\baz", "foo", "bar", "baz", 0);
        test_parse_cifs_service_one("//foo/bar", "foo", "bar", NULL, 0);
        test_parse_cifs_service_one("\\\\foo\\bar", "foo", "bar", NULL, 0);
        test_parse_cifs_service_one("//foo/bar/baz/uuu", "foo", "bar", "baz/uuu", 0);
        test_parse_cifs_service_one("\\\\foo\\bar\\baz\\uuu", "foo", "bar", "baz/uuu", 0);

        test_parse_cifs_service_one(NULL, NULL, NULL, NULL, -EINVAL);
        test_parse_cifs_service_one("", NULL, NULL, NULL, -EINVAL);
        test_parse_cifs_service_one("abc", NULL, NULL, NULL, -EINVAL);
        test_parse_cifs_service_one("abc/cde/efg", NULL, NULL, NULL, -EINVAL);
        test_parse_cifs_service_one("//foo/bar/baz/..", NULL, NULL, NULL, -EINVAL);
        test_parse_cifs_service_one("//foo///", NULL, NULL, NULL, -EINVAL);
        test_parse_cifs_service_one("//foo/.", NULL, NULL, NULL, -EINVAL);
        test_parse_cifs_service_one("//foo/a/.", NULL, NULL, NULL, -EINVAL);
        test_parse_cifs_service_one("//./a", NULL, NULL, NULL, -EINVAL);
}

TEST(open_mkdir_at) {
        _cleanup_close_ int fd = -EBADF, subdir_fd = -EBADF, subsubdir_fd = -EBADF;
        _cleanup_(rm_rf_physical_and_freep) char *t = NULL;
        struct stat sta, stb;

        assert_se(open_mkdir_at(AT_FDCWD, "/", O_EXCL|O_CLOEXEC, 0) == -EEXIST);
        assert_se(open_mkdir_at(AT_FDCWD, ".", O_EXCL|O_CLOEXEC, 0) == -EEXIST);

        fd = open_mkdir_at(AT_FDCWD, "/", O_CLOEXEC, 0);
        ASSERT_OK(fd);
        assert_se(stat("/", &sta) >= 0);
        assert_se(fstat(fd, &stb) >= 0);
        assert_se(stat_inode_same(&sta, &stb));
        fd = safe_close(fd);

        fd = open_mkdir_at(AT_FDCWD, ".", O_CLOEXEC, 0);
        assert_se(stat(".", &sta) >= 0);
        assert_se(fstat(fd, &stb) >= 0);
        assert_se(stat_inode_same(&sta, &stb));
        fd = safe_close(fd);

        assert_se(open_mkdir_at(AT_FDCWD, "/proc", O_EXCL|O_CLOEXEC, 0) == -EEXIST);

        fd = open_mkdir_at(AT_FDCWD, "/proc", O_CLOEXEC, 0);
        ASSERT_OK(fd);
        fd = safe_close(fd);

        assert_se(open_mkdir_at(AT_FDCWD, "/bin/sh", O_EXCL|O_CLOEXEC, 0) == -EEXIST);
        assert_se(open_mkdir_at(AT_FDCWD, "/bin/sh", O_CLOEXEC, 0) == -EEXIST);

        assert_se(mkdtemp_malloc(NULL, &t) >= 0);

        assert_se(open_mkdir_at(AT_FDCWD, t, O_EXCL|O_CLOEXEC, 0) == -EEXIST);
        assert_se(open_mkdir_at(AT_FDCWD, t, O_PATH|O_EXCL|O_CLOEXEC, 0) == -EEXIST);

        fd = open_mkdir_at(AT_FDCWD, t, O_CLOEXEC, 0000);
        ASSERT_OK(fd);
        fd = safe_close(fd);

        fd = open_mkdir_at(AT_FDCWD, t, O_PATH|O_CLOEXEC, 0000);
        ASSERT_OK(fd);

        subdir_fd = open_mkdir_at(fd, "xxx", O_PATH|O_EXCL|O_CLOEXEC, 0700);
        ASSERT_OK(subdir_fd);

        assert_se(open_mkdir_at(fd, "xxx", O_PATH|O_EXCL|O_CLOEXEC, 0) == -EEXIST);

        subsubdir_fd = open_mkdir_at(subdir_fd, "yyy", O_EXCL|O_CLOEXEC, 0700);
        ASSERT_OK(subsubdir_fd);
        subsubdir_fd = safe_close(subsubdir_fd);

        assert_se(open_mkdir_at(subdir_fd, "yyy", O_EXCL|O_CLOEXEC, 0) == -EEXIST);

        assert_se(open_mkdir_at(fd, "xxx/yyy", O_EXCL|O_CLOEXEC, 0) == -EEXIST);

        subsubdir_fd = open_mkdir_at(fd, "xxx/yyy", O_CLOEXEC, 0700);
        ASSERT_OK(subsubdir_fd);
}

TEST(openat_report_new) {
        _cleanup_free_ char *j = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *d = NULL;
        _cleanup_close_ int fd = -EBADF;
        bool b;

        assert_se(mkdtemp_malloc(NULL, &d) >= 0);

        j = path_join(d, "test");
        ASSERT_TRUE(j);

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT, 0666, &b);
        ASSERT_OK(fd);
        fd = safe_close(fd);
        ASSERT_TRUE(b);

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT, 0666, &b);
        ASSERT_OK(fd);
        fd = safe_close(fd);
        ASSERT_FALSE(b);

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT, 0666, &b);
        ASSERT_OK(fd);
        fd = safe_close(fd);
        ASSERT_FALSE(b);

        ASSERT_OK(unlink(j));

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT, 0666, &b);
        ASSERT_OK(fd);
        fd = safe_close(fd);
        ASSERT_TRUE(b);

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT, 0666, &b);
        ASSERT_OK(fd);
        fd = safe_close(fd);
        ASSERT_FALSE(b);

        ASSERT_OK(unlink(j));

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT, 0666, NULL);
        ASSERT_OK(fd);
        fd = safe_close(fd);

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT, 0666, &b);
        ASSERT_OK(fd);
        fd = safe_close(fd);
        ASSERT_FALSE(b);

        fd = openat_report_new(AT_FDCWD, j, O_RDWR, 0666, &b);
        ASSERT_OK(fd);
        fd = safe_close(fd);
        ASSERT_FALSE(b);

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT|O_EXCL, 0666, &b);
        assert_se(fd == -EEXIST);

        ASSERT_OK(unlink(j));

        fd = openat_report_new(AT_FDCWD, j, O_RDWR, 0666, &b);
        assert_se(fd == -ENOENT);

        fd = openat_report_new(AT_FDCWD, j, O_RDWR|O_CREAT|O_EXCL, 0666, &b);
        ASSERT_OK(fd);
        fd = safe_close(fd);
        ASSERT_TRUE(b);
}

TEST(xopenat_full) {
        _cleanup_(rm_rf_physical_and_freep) char *t = NULL;
        _cleanup_close_ int tfd = -EBADF, fd = -EBADF, fd2 = -EBADF;

        assert_se((tfd = mkdtemp_open(NULL, 0, &t)) >= 0);

        /* Test that xopenat_full() creates directories if O_DIRECTORY is specified. */

        assert_se((fd = xopenat_full(tfd, "abc", O_DIRECTORY|O_CREAT|O_EXCL|O_CLOEXEC, 0, 0755)) >= 0);
        assert_se((fd_verify_directory(fd) >= 0));
        fd = safe_close(fd);

        assert_se(xopenat_full(tfd, "abc", O_DIRECTORY|O_CREAT|O_EXCL|O_CLOEXEC, 0, 0755) == -EEXIST);

        assert_se((fd = xopenat_full(tfd, "abc", O_DIRECTORY|O_CREAT|O_CLOEXEC, 0, 0755)) >= 0);
        assert_se((fd_verify_directory(fd) >= 0));
        fd = safe_close(fd);

        /* Test that xopenat_full() creates regular files if O_DIRECTORY is not specified. */

        assert_se((fd = xopenat_full(tfd, "def", O_CREAT|O_EXCL|O_CLOEXEC, 0, 0644)) >= 0);
        ASSERT_OK(fd_verify_regular(fd));
        fd = safe_close(fd);

        /* Test that we can reopen an existing fd with xopenat_full() by specifying an empty path. */

        assert_se((fd = xopenat_full(tfd, "def", O_PATH|O_CLOEXEC, 0, 0)) >= 0);
        assert_se((fd2 = xopenat_full(fd, "", O_RDWR|O_CLOEXEC, 0, 0644)) >= 0);
}

TEST(xopenat_lock_full) {
        _cleanup_(rm_rf_physical_and_freep) char *t = NULL;
        _cleanup_close_ int tfd = -EBADF, fd = -EBADF;
        siginfo_t si;

        assert_se((tfd = mkdtemp_open(NULL, 0, &t)) >= 0);

        /* Test that we can acquire an exclusive lock on a directory in one process, remove the directory,
         * and close the file descriptor and still properly create the directory and acquire the lock in
         * another process.  */

        fd = xopenat_lock_full(tfd, "abc", O_CREAT|O_DIRECTORY|O_CLOEXEC, 0, 0755, LOCK_BSD, LOCK_EX);
        ASSERT_OK(fd);
        ASSERT_OK(faccessat(tfd, "abc", F_OK, 0));
        ASSERT_OK(fd_verify_directory(fd));
        assert_se(xopenat_lock_full(tfd, "abc", O_DIRECTORY|O_CLOEXEC, 0, 0755, LOCK_BSD, LOCK_EX|LOCK_NB) == -EAGAIN);

        pid_t pid = fork();
        ASSERT_OK(pid);

        if (pid == 0) {
                safe_close(fd);

                fd = xopenat_lock_full(tfd, "abc", O_CREAT|O_DIRECTORY|O_CLOEXEC, 0, 0755, LOCK_BSD, LOCK_EX);
                ASSERT_OK(fd);
                ASSERT_OK(faccessat(tfd, "abc", F_OK, 0));
                ASSERT_OK(fd_verify_directory(fd));
                assert_se(xopenat_lock_full(tfd, "abc", O_DIRECTORY|O_CLOEXEC, 0, 0755, LOCK_BSD, LOCK_EX|LOCK_NB) == -EAGAIN);

                _exit(EXIT_SUCCESS);
        }

        /* We need to give the child process some time to get past the xopenat() call in xopenat_lock_full()
         * and block in the call to lock_generic() waiting for the lock to become free. We can't modify
         * xopenat_lock_full() to signal an eventfd to let us know when that has happened, so we just sleep
         * for a little and assume that's enough time for the child process to get along far enough. It
         * doesn't matter if it doesn't get far enough, in that case we just won't trigger the fallback logic
         * in xopenat_lock_full(), but the test will still succeed. */
        assert_se(usleep_safe(20 * USEC_PER_MSEC) >= 0);

        ASSERT_OK(unlinkat(tfd, "abc", AT_REMOVEDIR));
        fd = safe_close(fd);

        assert_se(wait_for_terminate(pid, &si) >= 0);
        assert_se(si.si_code == CLD_EXITED);

        assert_se(xopenat_lock_full(tfd, "abc", 0, 0, 0755, LOCK_POSIX, LOCK_EX) == -EBADF);
        assert_se(xopenat_lock_full(tfd, "def", O_DIRECTORY, 0, 0755, LOCK_POSIX, LOCK_EX) == -EBADF);
}

TEST(linkat_replace) {
        _cleanup_(rm_rf_physical_and_freep) char *t = NULL;
        _cleanup_close_ int tfd = -EBADF;

        assert_se((tfd = mkdtemp_open(NULL, 0, &t)) >= 0);

        _cleanup_close_ int fd1 = openat(tfd, "foo", O_CREAT|O_RDWR|O_CLOEXEC, 0600);
        ASSERT_OK(fd1);

        ASSERT_OK(linkat_replace(tfd, "foo", tfd, "bar"));
        ASSERT_OK(linkat_replace(tfd, "foo", tfd, "bar"));

        _cleanup_close_ int fd1_check = openat(tfd, "bar", O_RDWR|O_CLOEXEC);
        ASSERT_OK(fd1_check);

        ASSERT_GT(inode_same_at(fd1, NULL, fd1_check, NULL, AT_EMPTY_PATH), 0);

        _cleanup_close_ int fd2 = openat(tfd, "baz", O_CREAT|O_RDWR|O_CLOEXEC, 0600);
        ASSERT_OK(fd2);

        ASSERT_EQ(inode_same_at(fd1, NULL, fd2, NULL, AT_EMPTY_PATH), 0);

        ASSERT_OK(linkat_replace(tfd, "foo", tfd, "baz"));

        _cleanup_close_ int fd2_check = openat(tfd, "baz", O_RDWR|O_CLOEXEC);

        ASSERT_EQ(inode_same_at(fd2, NULL, fd2_check, NULL, AT_EMPTY_PATH), 0);
        ASSERT_GT(inode_same_at(fd1, NULL, fd2_check, NULL, AT_EMPTY_PATH), 0);
}

static int intro(void) {
        arg_test_dir = saved_argv[1];
        return EXIT_SUCCESS;
}

TEST(readlinkat_malloc) {
        _cleanup_(rm_rf_physical_and_freep) char *t = NULL;
        _cleanup_close_ int tfd = -EBADF, fd = -EBADF;
        _cleanup_free_ char *p = NULL, *q = NULL;
        const char *expect = "hgoehogefoobar";

        tfd = mkdtemp_open(NULL, O_PATH, &t);
        ASSERT_OK(tfd);

        ASSERT_OK(symlinkat(expect, tfd, "linkname"));

        assert_se(readlinkat_malloc(tfd, "linkname", &p) >= 0);
        ASSERT_TRUE(streq(p, expect));
        p = mfree(p);

        fd = openat(tfd, "linkname", O_PATH | O_NOFOLLOW | O_CLOEXEC);
        ASSERT_OK(fd);
        assert_se(readlinkat_malloc(fd, NULL, &p) >= 0);
        ASSERT_TRUE(streq(p, expect));
        p = mfree(p);
        assert_se(readlinkat_malloc(fd, "", &p) >= 0);
        ASSERT_TRUE(streq(p, expect));
        p = mfree(p);
        fd = safe_close(fd);

        assert_se(q = path_join(t, "linkname"));
        assert_se(readlinkat_malloc(AT_FDCWD, q, &p) >= 0);
        ASSERT_TRUE(streq(p, expect));
        p = mfree(p);
        assert_se(readlinkat_malloc(INT_MAX, q, &p) >= 0);
        ASSERT_TRUE(streq(p, expect));
        p = mfree(p);
        q = mfree(q);
}

DEFINE_TEST_MAIN_WITH_INTRO(LOG_INFO, intro);
