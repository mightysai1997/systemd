/* SPDX-License-Identifier: LGPL-2.1+ */

#include <nss.h>
#include <pthread.h>

#include "env-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "group-record-nss.h"
#include "macro.h"
#include "nss-util.h"
#include "signal-util.h"
#include "strv.h"
#include "user-util.h"
#include "userdb-glue.h"
#include "userdb.h"

static const struct passwd root_passwd = {
        .pw_name = (char*) "root",
        .pw_passwd = (char*) "x", /* see shadow file */
        .pw_uid = 0,
        .pw_gid = 0,
        .pw_gecos = (char*) "Super User",
        .pw_dir = (char*) "/root",
        .pw_shell = (char*) "/bin/sh",
};

static const struct passwd nobody_passwd = {
        .pw_name = (char*) NOBODY_USER_NAME,
        .pw_passwd = (char*) "*", /* locked */
        .pw_uid = UID_NOBODY,
        .pw_gid = GID_NOBODY,
        .pw_gecos = (char*) "User Nobody",
        .pw_dir = (char*) "/",
        .pw_shell = (char*) NOLOGIN,
};

static const struct group root_group = {
        .gr_name = (char*) "root",
        .gr_gid = 0,
        .gr_passwd = (char*) "x", /* see shadow file */
        .gr_mem = (char*[]) { NULL },
};

static const struct group nobody_group = {
        .gr_name = (char*) NOBODY_GROUP_NAME,
        .gr_gid = GID_NOBODY,
        .gr_passwd = (char*) "*", /* locked */
        .gr_mem = (char*[]) { NULL },
};

typedef struct GetentData {
        /* As explained in NOTES section of getpwent_r(3) as 'getpwent_r() is not really reentrant since it
         * shares the reading position in the stream with all other threads', we need to protect the data in
         * UserDBIterator from multithreaded programs which may call setpwent(), getpwent_r(), or endpwent()
         * simultaneously. So, each function locks the data by using the mutex below. */
        pthread_mutex_t mutex;
        UserDBIterator *iterator;

        /* Applies to group iterations only: true while we iterate over groups defined through NSS, false
         * otherwise. */
        bool by_membership;
} GetentData;

static GetentData getpwent_data = {
        .mutex = PTHREAD_MUTEX_INITIALIZER
};

static GetentData getgrent_data = {
        .mutex = PTHREAD_MUTEX_INITIALIZER
};

NSS_GETPW_PROTOTYPES(systemd);
NSS_GETGR_PROTOTYPES(systemd);
NSS_PWENT_PROTOTYPES(systemd);
NSS_GRENT_PROTOTYPES(systemd);
NSS_INITGROUPS_PROTOTYPE(systemd);

enum nss_status _nss_systemd_getpwnam_r(
                const char *name,
                struct passwd *pwd,
                char *buffer, size_t buflen,
                int *errnop) {

        enum nss_status status;
        int e;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        assert(name);
        assert(pwd);
        assert(errnop);

        /* If the username is not valid, then we don't know it. Ideally libc would filter these for us
         * anyway. We don't generate EINVAL here, because it isn't really out business to complain about
         * invalid user names. */
        if (!valid_user_group_name(name))
                return NSS_STATUS_NOTFOUND;

        /* Synthesize entries for the root and nobody users, in case they are missing in /etc/passwd */
        if (getenv_bool_secure("SYSTEMD_NSS_BYPASS_SYNTHETIC") <= 0) {

                if (streq(name, root_passwd.pw_name)) {
                        *pwd = root_passwd;
                        return NSS_STATUS_SUCCESS;
                }

                if (streq(name, nobody_passwd.pw_name)) {
                        if (!synthesize_nobody())
                                return NSS_STATUS_NOTFOUND;

                        *pwd = nobody_passwd;
                        return NSS_STATUS_SUCCESS;
                }

        } else if (STR_IN_SET(name, root_passwd.pw_name, nobody_passwd.pw_name))
                return NSS_STATUS_NOTFOUND;

        status = userdb_getpwnam(name, pwd, buffer, buflen, &e);
        if (IN_SET(status, NSS_STATUS_UNAVAIL, NSS_STATUS_TRYAGAIN)) {
                UNPROTECT_ERRNO;
                *errnop = e;
                return status;
        }

        return status;
}

enum nss_status _nss_systemd_getpwuid_r(
                uid_t uid,
                struct passwd *pwd,
                char *buffer, size_t buflen,
                int *errnop) {

        enum nss_status status;
        int e;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        assert(pwd);
        assert(errnop);

        if (!uid_is_valid(uid))
                return NSS_STATUS_NOTFOUND;

        /* Synthesize data for the root user and for nobody in case they are missing from /etc/passwd */
        if (getenv_bool_secure("SYSTEMD_NSS_BYPASS_SYNTHETIC") <= 0) {

                if (uid == root_passwd.pw_uid) {
                        *pwd = root_passwd;
                        return NSS_STATUS_SUCCESS;
                }

                if (uid == nobody_passwd.pw_uid) {
                        if (!synthesize_nobody())
                                return NSS_STATUS_NOTFOUND;

                        *pwd = nobody_passwd;
                        return NSS_STATUS_SUCCESS;
                }

        } else if (uid == root_passwd.pw_uid || uid == nobody_passwd.pw_uid)
                return NSS_STATUS_NOTFOUND;

        status = userdb_getpwuid(uid, pwd, buffer, buflen, &e);
        if (IN_SET(status, NSS_STATUS_UNAVAIL, NSS_STATUS_TRYAGAIN)) {
                UNPROTECT_ERRNO;
                *errnop = e;
                return status;
        }

        return status;
}

#pragma GCC diagnostic ignored "-Wsizeof-pointer-memaccess"

enum nss_status _nss_systemd_getgrnam_r(
                const char *name,
                struct group *gr,
                char *buffer, size_t buflen,
                int *errnop) {

        enum nss_status status;
        int e;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        assert(name);
        assert(gr);
        assert(errnop);

        if (!valid_user_group_name(name))
                return NSS_STATUS_NOTFOUND;

        /* Synthesize records for root and nobody, in case they are missing form /etc/group */
        if (getenv_bool_secure("SYSTEMD_NSS_BYPASS_SYNTHETIC") <= 0) {

                if (streq(name, root_group.gr_name)) {
                        *gr = root_group;
                        return NSS_STATUS_SUCCESS;
                }

                if (streq(name, nobody_group.gr_name)) {
                        if (!synthesize_nobody())
                                return NSS_STATUS_NOTFOUND;

                        *gr = nobody_group;
                        return NSS_STATUS_SUCCESS;
                }

        } else if (STR_IN_SET(name, root_group.gr_name, nobody_group.gr_name))
                return NSS_STATUS_NOTFOUND;

        status = userdb_getgrnam(name, gr, buffer, buflen, &e);
        if (IN_SET(status, NSS_STATUS_UNAVAIL, NSS_STATUS_TRYAGAIN)) {
                UNPROTECT_ERRNO;
                *errnop = e;
                return status;
        }

        return status;
}

enum nss_status _nss_systemd_getgrgid_r(
                gid_t gid,
                struct group *gr,
                char *buffer, size_t buflen,
                int *errnop) {

        enum nss_status status;
        int e;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        assert(gr);
        assert(errnop);

        if (!gid_is_valid(gid))
                return NSS_STATUS_NOTFOUND;

        /* Synthesize records for root and nobody, in case they are missing from /etc/group */
        if (getenv_bool_secure("SYSTEMD_NSS_BYPASS_SYNTHETIC") <= 0) {

                if (gid == root_group.gr_gid) {
                        *gr = root_group;
                        return NSS_STATUS_SUCCESS;
                }

                if (gid == nobody_group.gr_gid) {
                        if (!synthesize_nobody())
                                return NSS_STATUS_NOTFOUND;

                        *gr = nobody_group;
                        return NSS_STATUS_SUCCESS;
                }

        } else if (gid == root_group.gr_gid || gid == nobody_group.gr_gid)
                return NSS_STATUS_NOTFOUND;

        status = userdb_getgrgid(gid, gr, buffer, buflen, &e);
        if (IN_SET(status, NSS_STATUS_UNAVAIL, NSS_STATUS_TRYAGAIN)) {
                UNPROTECT_ERRNO;
                *errnop = e;
                return status;
        }

        return status;
}

static enum nss_status nss_systemd_endent(GetentData *p) {
        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        assert(p);

        assert_se(pthread_mutex_lock(&p->mutex) == 0);
        p->iterator = userdb_iterator_free(p->iterator);
        p->by_membership = false;
        assert_se(pthread_mutex_unlock(&p->mutex) == 0);

        return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_systemd_endpwent(void) {
        return nss_systemd_endent(&getpwent_data);
}

enum nss_status _nss_systemd_endgrent(void) {
        return nss_systemd_endent(&getgrent_data);
}

enum nss_status _nss_systemd_setpwent(int stayopen) {
        enum nss_status ret;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        if (userdb_nss_compat_is_enabled() <= 0)
                return NSS_STATUS_NOTFOUND;

        assert_se(pthread_mutex_lock(&getpwent_data.mutex) == 0);

        getpwent_data.iterator = userdb_iterator_free(getpwent_data.iterator);
        getpwent_data.by_membership = false;

        ret = userdb_all(nss_glue_userdb_flags(), &getpwent_data.iterator) < 0 ?
                NSS_STATUS_UNAVAIL : NSS_STATUS_SUCCESS;

        assert_se(pthread_mutex_unlock(&getpwent_data.mutex) == 0);
        return ret;
}

enum nss_status _nss_systemd_setgrent(int stayopen) {
        enum nss_status ret;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        if (userdb_nss_compat_is_enabled() <= 0)
                return NSS_STATUS_NOTFOUND;

        assert_se(pthread_mutex_lock(&getgrent_data.mutex) == 0);

        getgrent_data.iterator = userdb_iterator_free(getgrent_data.iterator);
        getpwent_data.by_membership = false;

        ret = groupdb_all(nss_glue_userdb_flags(), &getgrent_data.iterator) < 0 ?
                NSS_STATUS_UNAVAIL : NSS_STATUS_SUCCESS;

        assert_se(pthread_mutex_unlock(&getgrent_data.mutex) == 0);
        return ret;
}

enum nss_status _nss_systemd_getpwent_r(
                struct passwd *result,
                char *buffer, size_t buflen,
                int *errnop) {

        _cleanup_(user_record_unrefp) UserRecord *ur = NULL;
        enum nss_status ret;
        int r;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        assert(result);
        assert(errnop);

        r = userdb_nss_compat_is_enabled();
        if (r < 0) {
                UNPROTECT_ERRNO;
                *errnop = -r;
                return NSS_STATUS_UNAVAIL;
        }
        if (!r)
                return NSS_STATUS_NOTFOUND;

        assert_se(pthread_mutex_lock(&getpwent_data.mutex) == 0);

        if (!getpwent_data.iterator) {
                UNPROTECT_ERRNO;
                *errnop = EHOSTDOWN;
                ret = NSS_STATUS_UNAVAIL;
                goto finish;
        }

        r = userdb_iterator_get(getpwent_data.iterator, &ur);
        if (r == -ESRCH) {
                ret = NSS_STATUS_NOTFOUND;
                goto finish;
        }
        if (r < 0) {
                UNPROTECT_ERRNO;
                *errnop = -r;
                ret = NSS_STATUS_UNAVAIL;
                goto finish;
        }

        r = nss_pack_user_record(ur, result, buffer, buflen);
        if (r < 0) {
                UNPROTECT_ERRNO;
                *errnop = -r;
                ret = NSS_STATUS_TRYAGAIN;
                goto finish;
        }

        ret = NSS_STATUS_SUCCESS;

finish:
        assert_se(pthread_mutex_unlock(&getpwent_data.mutex) == 0);
        return ret;
}

enum nss_status _nss_systemd_getgrent_r(
                struct group *result,
                char *buffer, size_t buflen,
                int *errnop) {

        _cleanup_(group_record_unrefp) GroupRecord *gr = NULL;
        _cleanup_free_ char **members = NULL;
        enum nss_status ret;
        int r;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        assert(result);
        assert(errnop);

        r = userdb_nss_compat_is_enabled();
        if (r < 0) {
                UNPROTECT_ERRNO;
                *errnop = -r;
                return NSS_STATUS_UNAVAIL;
        }
        if (!r)
                return NSS_STATUS_UNAVAIL;

        assert_se(pthread_mutex_lock(&getgrent_data.mutex) == 0);

        if (!getgrent_data.iterator) {
                UNPROTECT_ERRNO;
                *errnop = EHOSTDOWN;
                ret = NSS_STATUS_UNAVAIL;
                goto finish;
        }

        if (!getgrent_data.by_membership) {
                r = groupdb_iterator_get(getgrent_data.iterator, &gr);
                if (r == -ESRCH) {
                        /* So we finished iterating native groups now. let's now continue with iterating
                         * native memberships, and generate additional group entries for any groups
                         * referenced there that are defined in NSS only. This means for those groups there
                         * will be two or more entries generated during iteration, but this is apparently how
                         * this is supposed to work, and what other implementations do too. Clients are
                         * supposed to merge the group records found during iteration automatically. */
                        getgrent_data.iterator = userdb_iterator_free(getgrent_data.iterator);

                        r = membershipdb_all(nss_glue_userdb_flags(), &getgrent_data.iterator);
                        if (r < 0) {
                                UNPROTECT_ERRNO;
                                *errnop = -r;
                                ret = NSS_STATUS_UNAVAIL;
                                goto finish;
                        }

                        getgrent_data.by_membership = true;
                } else if (r < 0) {
                        UNPROTECT_ERRNO;
                        *errnop = -r;
                        ret = NSS_STATUS_UNAVAIL;
                        goto finish;
                } else if (!STR_IN_SET(gr->group_name, root_group.gr_name, nobody_group.gr_name)) {
                        r = membershipdb_by_group_strv(gr->group_name, nss_glue_userdb_flags(), &members);
                        if (r < 0) {
                                UNPROTECT_ERRNO;
                                *errnop = -r;
                                ret = NSS_STATUS_UNAVAIL;
                                goto finish;
                        }
                }
        }

        if (getgrent_data.by_membership) {
                _cleanup_close_ int lock_fd = -1;

                for (;;) {
                        _cleanup_free_ char *user_name = NULL, *group_name = NULL;

                        r = membershipdb_iterator_get(getgrent_data.iterator, &user_name, &group_name);
                        if (r == -ESRCH) {
                                ret = NSS_STATUS_NOTFOUND;
                                goto finish;
                        }
                        if (r < 0) {
                                UNPROTECT_ERRNO;
                                *errnop = -r;
                                ret = NSS_STATUS_UNAVAIL;
                                goto finish;
                        }

                        if (STR_IN_SET(user_name, root_passwd.pw_name, nobody_passwd.pw_name))
                                continue;
                        if (STR_IN_SET(group_name, root_group.gr_name, nobody_group.gr_name))
                                continue;

                        /* We are about to recursively call into NSS, let's make sure we disable recursion into our own code. */
                        if (lock_fd < 0) {
                                lock_fd = userdb_nss_compat_disable();
                                if (lock_fd < 0 && lock_fd != -EBUSY) {
                                        UNPROTECT_ERRNO;
                                        *errnop = -lock_fd;
                                        ret = NSS_STATUS_UNAVAIL;
                                        goto finish;
                                }
                        }

                        r = nss_group_record_by_name(group_name, &gr);
                        if (r == -ESRCH)
                                continue;
                        if (r < 0) {
                                log_debug_errno(r, "Failed to do NSS check for group '%s', ignoring: %m", group_name);
                                continue;
                        }

                        members = strv_new(user_name);
                        if (!members) {
                                UNPROTECT_ERRNO;
                                *errnop = ENOMEM;
                                return NSS_STATUS_TRYAGAIN;
                        }

                        /* Note that we currently generate one group entry per user that is part of a
                         * group. It's a bit ugly, but equivalent to generating a single entry with a set of
                         * members in them. */
                        break;
                }
        }

        r = nss_pack_group_record(gr, members, result, buffer, buflen);
        if (r < 0) {
                UNPROTECT_ERRNO;
                *errnop = -r;
                ret = NSS_STATUS_TRYAGAIN;
                goto finish;
        }

        ret = NSS_STATUS_SUCCESS;

finish:
        assert_se(pthread_mutex_unlock(&getgrent_data.mutex) == 0);
        return ret;
}

enum nss_status _nss_systemd_initgroups_dyn(
                const char *user_name,
                gid_t gid,
                long *start,
                long *size,
                gid_t **groupsp,
                long int limit,
                int *errnop) {

        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        bool any = false;
        int r;

        PROTECT_ERRNO;
        BLOCK_SIGNALS(NSS_SIGNALS_BLOCK);

        assert(user_name);
        assert(start);
        assert(size);
        assert(groupsp);
        assert(errnop);

        if (!valid_user_group_name(user_name))
                return NSS_STATUS_NOTFOUND;

        /* Don't allow extending these two special users, the same as we won't resolve them via getpwnam() */
        if (STR_IN_SET(user_name, root_passwd.pw_name, nobody_passwd.pw_name))
                return NSS_STATUS_NOTFOUND;

        r = userdb_nss_compat_is_enabled();
        if (r < 0) {
                UNPROTECT_ERRNO;
                *errnop = -r;
                return NSS_STATUS_UNAVAIL;
        }
        if (!r)
                return NSS_STATUS_NOTFOUND;

        r = membershipdb_by_user(user_name, nss_glue_userdb_flags(), &iterator);
        if (r < 0) {
                UNPROTECT_ERRNO;
                *errnop = -r;
                return NSS_STATUS_UNAVAIL;
        }

        for (;;) {
                _cleanup_(group_record_unrefp) GroupRecord *g = NULL;
                _cleanup_free_ char *group_name = NULL;

                r = membershipdb_iterator_get(iterator, NULL, &group_name);
                if (r == -ESRCH)
                        break;
                if (r < 0) {
                        UNPROTECT_ERRNO;
                        *errnop = -r;
                        return NSS_STATUS_UNAVAIL;
                }

                /* The group might be defined via traditional NSS only, hence let's do a full look-up without
                 * disabling NSS. This means we are operating recursively here. */

                r = groupdb_by_name(group_name, nss_glue_userdb_flags() & ~USERDB_AVOID_NSS, &g);
                if (r == -ESRCH)
                        continue;
                if (r < 0) {
                        log_debug_errno(r, "Failed to resolve group '%s', ignoring: %m", group_name);
                        continue;
                }

                if (g->gid == gid)
                        continue;

                if (*start >= *size) {
                        gid_t *new_groups;
                        long new_size;

                        if (limit > 0 && *size >= limit) /* Reached the limit.? */
                                break;

                        if (*size > LONG_MAX/2) { /* Check for overflow */
                                UNPROTECT_ERRNO;
                                *errnop = ENOMEM;
                                return NSS_STATUS_TRYAGAIN;
                        }

                        new_size = *start * 2;
                        if (limit > 0 && new_size > limit)
                                new_size = limit;

                        /* Enlarge buffer */
                        new_groups = realloc(*groupsp, new_size * sizeof(**groupsp));
                        if (!new_groups) {
                                UNPROTECT_ERRNO;
                                *errnop = ENOMEM;
                                return NSS_STATUS_TRYAGAIN;
                        }

                        *groupsp = new_groups;
                        *size = new_size;
                }

                (*groupsp)[(*start)++] = g->gid;
                any = true;
        }

        return any ? NSS_STATUS_SUCCESS : NSS_STATUS_NOTFOUND;
}
