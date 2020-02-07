/* SPDX-License-Identifier: LGPL-2.1+ */

#include <sys/auxv.h>

#include "dirent-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "group-record-nss.h"
#include "missing_syscall.h"
#include "parse-util.h"
#include "set.h"
#include "socket-util.h"
#include "strv.h"
#include "user-record-nss.h"
#include "user-util.h"
#include "userdb.h"
#include "varlink.h"

DEFINE_PRIVATE_HASH_OPS_WITH_VALUE_DESTRUCTOR(link_hash_ops, void, trivial_hash_func, trivial_compare_func, Varlink, varlink_unref);

typedef enum LookupWhat {
        LOOKUP_USER,
        LOOKUP_GROUP,
        LOOKUP_MEMBERSHIP,
        _LOOKUP_WHAT_MAX,
} LookupWhat;

struct UserDBIterator {
        LookupWhat what;
        Set *links;
        bool nss_covered:1;
        bool nss_iterating:1;
        bool synthesize_root:1;
        bool synthesize_nobody:1;
        int error;
        int nss_lock;
        unsigned n_found;
        sd_event *event;
        UserRecord *found_user;                   /* when .what == LOOKUP_USER */
        GroupRecord *found_group;                 /* when .what == LOOKUP_GROUP */

        char *found_user_name, *found_group_name; /* when .what == LOOKUP_MEMBERSHIP */
        char **members_of_group;
        size_t index_members_of_group;
        char *filter_user_name;
};

UserDBIterator* userdb_iterator_free(UserDBIterator *iterator) {
        if (!iterator)
                return NULL;

        set_free(iterator->links);

        switch (iterator->what) {

        case LOOKUP_USER:
                user_record_unref(iterator->found_user);

                if (iterator->nss_iterating)
                        endpwent();

                break;

        case LOOKUP_GROUP:
                group_record_unref(iterator->found_group);

                if (iterator->nss_iterating)
                        endgrent();

                break;

        case LOOKUP_MEMBERSHIP:
                free(iterator->found_user_name);
                free(iterator->found_group_name);
                strv_free(iterator->members_of_group);
                free(iterator->filter_user_name);

                if (iterator->nss_iterating)
                        endgrent();

                break;

        default:
                assert_not_reached("Unexpected state?");
        }

        sd_event_unref(iterator->event);
        safe_close(iterator->nss_lock);

        return mfree(iterator);
}

static UserDBIterator* userdb_iterator_new(LookupWhat what) {
        UserDBIterator *i;

        assert(what >= 0);
        assert(what < _LOOKUP_WHAT_MAX);

        i = new(UserDBIterator, 1);
        if (!i)
                return NULL;

        *i = (UserDBIterator) {
                .what = what,
                .nss_lock = -1,
        };

        return i;
}

struct user_group_data {
        JsonVariant *record;
        bool incomplete;
};

static void user_group_data_release(struct user_group_data *d) {
        json_variant_unref(d->record);
}

static int userdb_on_query_reply(
                Varlink *link,
                JsonVariant *parameters,
                const char *error_id,
                VarlinkReplyFlags flags,
                void *userdata) {

        UserDBIterator *iterator = userdata;
        int r;

        assert(iterator);

        if (error_id) {
                log_debug("Got lookup error: %s", error_id);

                if (STR_IN_SET(error_id,
                               "io.systemd.UserDatabase.NoRecordFound",
                               "io.systemd.UserDatabase.ConflictingRecordFound"))
                        r = -ESRCH;
                else if (streq(error_id, "io.systemd.UserDatabase.ServiceNotAvailable"))
                        r = -EHOSTDOWN;
                else if (streq(error_id, VARLINK_ERROR_TIMEOUT))
                        r = -ETIMEDOUT;
                else
                        r = -EIO;

                goto finish;
        }

        switch (iterator->what) {

        case LOOKUP_USER: {
                _cleanup_(user_group_data_release) struct user_group_data user_data = {};

                static const JsonDispatch dispatch_table[] = {
                        { "record",     _JSON_VARIANT_TYPE_INVALID, json_dispatch_variant, offsetof(struct user_group_data, record),     0 },
                        { "incomplete", JSON_VARIANT_BOOLEAN,       json_dispatch_boolean, offsetof(struct user_group_data, incomplete), 0 },
                        {}
                };
                _cleanup_(user_record_unrefp) UserRecord *hr = NULL;

                assert_se(!iterator->found_user);

                r = json_dispatch(parameters, dispatch_table, NULL, 0, &user_data);
                if (r < 0)
                        goto finish;

                if (!user_data.record) {
                        r = log_debug_errno(SYNTHETIC_ERRNO(EIO), "Reply is missing record key");
                        goto finish;
                }

                hr = user_record_new();
                if (!hr) {
                        r = -ENOMEM;
                        goto finish;
                }

                r = user_record_load(hr, user_data.record, USER_RECORD_LOAD_REFUSE_SECRET|USER_RECORD_PERMISSIVE);
                if (r < 0)
                        goto finish;

                if (!hr->service) {
                        r = log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "User record does not carry service information, refusing.");
                        goto finish;
                }

                hr->incomplete = user_data.incomplete;

                /* We match the root user by the name since the name is our primary key. We match the nobody
                 * use by UID though, since the name might differ on OSes */
                if (streq_ptr(hr->user_name, "root"))
                        iterator->synthesize_root = false;
                if (hr->uid == UID_NOBODY)
                        iterator->synthesize_nobody = false;

                iterator->found_user = TAKE_PTR(hr);
                iterator->n_found++;

                /* More stuff coming? then let's just exit cleanly here */
                if (FLAGS_SET(flags, VARLINK_REPLY_CONTINUES))
                        return 0;

                /* Otherwise, let's remove this link and exit cleanly then */
                r = 0;
                goto finish;
        }

        case LOOKUP_GROUP: {
                _cleanup_(user_group_data_release) struct user_group_data group_data = {};

                static const JsonDispatch dispatch_table[] = {
                        { "record",     _JSON_VARIANT_TYPE_INVALID, json_dispatch_variant, offsetof(struct user_group_data, record),     0 },
                        { "incomplete", JSON_VARIANT_BOOLEAN,       json_dispatch_boolean, offsetof(struct user_group_data, incomplete), 0 },
                        {}
                };
                _cleanup_(group_record_unrefp) GroupRecord *g = NULL;

                assert_se(!iterator->found_group);

                r = json_dispatch(parameters, dispatch_table, NULL, 0, &group_data);
                if (r < 0)
                        goto finish;

                if (!group_data.record) {
                        r = log_debug_errno(SYNTHETIC_ERRNO(EIO), "Reply is missing record key");
                        goto finish;
                }

                g = group_record_new();
                if (!g) {
                        r = -ENOMEM;
                        goto finish;
                }

                r = group_record_load(g, group_data.record, USER_RECORD_LOAD_REFUSE_SECRET|USER_RECORD_PERMISSIVE);
                if (r < 0)
                        goto finish;

                if (!g->service) {
                        r = log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Group record does not carry service information, refusing.");
                        goto finish;
                }

                g->incomplete = group_data.incomplete;

                if (streq_ptr(g->group_name, "root"))
                        iterator->synthesize_root = false;
                if (g->gid == GID_NOBODY)
                        iterator->synthesize_nobody = false;

                iterator->found_group = TAKE_PTR(g);
                iterator->n_found++;

                if (FLAGS_SET(flags, VARLINK_REPLY_CONTINUES))
                        return 0;

                r = 0;
                goto finish;
        }

        case LOOKUP_MEMBERSHIP: {
                struct membership_data {
                        const char *user_name;
                        const char *group_name;
                } membership_data = {};

                static const JsonDispatch dispatch_table[] = {
                        { "userName",  JSON_VARIANT_STRING, json_dispatch_const_string, offsetof(struct membership_data, user_name),  JSON_SAFE },
                        { "groupName", JSON_VARIANT_STRING, json_dispatch_const_string, offsetof(struct membership_data, group_name), JSON_SAFE },
                        {}
                };

                assert(!iterator->found_user_name);
                assert(!iterator->found_group_name);

                r = json_dispatch(parameters, dispatch_table, NULL, 0, &membership_data);
                if (r < 0)
                        goto finish;

                iterator->found_user_name = mfree(iterator->found_user_name);
                iterator->found_group_name = mfree(iterator->found_group_name);

                iterator->found_user_name = strdup(membership_data.user_name);
                if (!iterator->found_user_name) {
                        r = -ENOMEM;
                        goto finish;
                }

                iterator->found_group_name = strdup(membership_data.group_name);
                if (!iterator->found_group_name) {
                        r = -ENOMEM;
                        goto finish;
                }

                iterator->n_found++;

                if (FLAGS_SET(flags, VARLINK_REPLY_CONTINUES))
                        return 0;

                r = 0;
                goto finish;
        }

        default:
                assert_not_reached("unexpected lookup");
        }

finish:
        /* If we got one ESRCH, let that win. This way when we do a wild dump we won't be tripped up by bad
         * errors if at least one connection ended cleanly */
        if (r == -ESRCH || iterator->error == 0)
                iterator->error = -r;

        assert_se(set_remove(iterator->links, link) == link);
        link = varlink_unref(link);
        return 0;
}

static int userdb_connect(
                UserDBIterator *iterator,
                const char *path,
                const char *method,
                bool more,
                JsonVariant *query) {

        _cleanup_(varlink_unrefp) Varlink *vl = NULL;
        int r;

        assert(iterator);
        assert(path);
        assert(method);

        r = varlink_connect_address(&vl, path);
        if (r < 0)
                return log_debug_errno(r, "Unable to connect to %s: %m", path);

        varlink_set_userdata(vl, iterator);

        if (!iterator->event) {
                r = sd_event_new(&iterator->event);
                if (r < 0)
                        return log_debug_errno(r, "Unable to allocate event loop: %m");
        }

        r = varlink_attach_event(vl, iterator->event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return log_debug_errno(r, "Failed to attach varlink connection to event loop: %m");

        (void) varlink_set_description(vl, path);

        r = varlink_bind_reply(vl, userdb_on_query_reply);
        if (r < 0)
                return log_debug_errno(r, "Failed to bind reply callback: %m");

        if (more)
                r = varlink_observe(vl, method, query);
        else
                r = varlink_invoke(vl, method, query);
        if (r < 0)
                return log_debug_errno(r, "Failed to invoke varlink method: %m");

        r = set_ensure_allocated(&iterator->links, &link_hash_ops);
        if (r < 0)
                return log_debug_errno(r, "Failed to allocate set: %m");

        r = set_put(iterator->links, vl);
        if (r < 0)
                return log_debug_errno(r, "Failed to add varlink connection to set: %m");

        TAKE_PTR(vl);
        return r;
}

static int userdb_start_query(
                UserDBIterator *iterator,
                const char *method,
                bool more,
                JsonVariant *query,
                UserDBFlags flags) {

        _cleanup_(strv_freep) char **except = NULL, **only = NULL;
        _cleanup_(closedirp) DIR *d = NULL;
        struct dirent *de;
        const char *e;
        int r, ret = 0;

        assert(iterator);
        assert(method);

        e = getenv("SYSTEMD_BYPASS_USERDB");
        if (e) {
                r = parse_boolean(e);
                if (r > 0)
                        return -ENOLINK;
                if (r < 0) {
                        except = strv_split(e, ":");
                        if (!except)
                                return -ENOMEM;
                }
        }

        e = getenv("SYSTEMD_ONLY_USERDB");
        if (e) {
                only = strv_split(e, ":");
                if (!only)
                        return -ENOMEM;
        }

        /* First, let's talk to the multiplexer, if we can */
        if ((flags & (USERDB_AVOID_MULTIPLEXER|USERDB_AVOID_DYNAMIC_USER|USERDB_AVOID_NSS|USERDB_DONT_SYNTHESIZE)) == 0 &&
            !strv_contains(except, "io.systemd.Multiplexer") &&
            (!only || strv_contains(only, "io.systemd.Multiplexer"))) {
                _cleanup_(json_variant_unrefp) JsonVariant *patched_query = json_variant_ref(query);

                r = json_variant_set_field_string(&patched_query, "service", "io.systemd.Multiplexer");
                if (r < 0)
                        return log_debug_errno(r, "Unable to set service JSON field: %m");

                r = userdb_connect(iterator, "/run/systemd/userdb/io.systemd.Multiplexer", method, more, patched_query);
                if (r >= 0) {
                        iterator->nss_covered = true; /* The multiplexer does NSS */
                        return 0;
                }
        }

        d = opendir("/run/systemd/userdb/");
        if (!d) {
                if (errno == ENOENT)
                        return -ESRCH;

                return -errno;
        }

        FOREACH_DIRENT(de, d, return -errno) {
                _cleanup_(json_variant_unrefp) JsonVariant *patched_query = NULL;
                _cleanup_free_ char *p = NULL;
                bool is_nss;

                if (streq(de->d_name, "io.systemd.Multiplexer")) /* We already tried this above, don't try this again */
                        continue;

                if (FLAGS_SET(flags, USERDB_AVOID_DYNAMIC_USER) &&
                    streq(de->d_name, "io.systemd.DynamicUser"))
                        continue;

                /* Avoid NSS is this is requested. Note that we also skip NSS when we were asked to skip the
                 * multiplexer, since in that case it's safer to do NSS in the client side emulation below
                 * (and when we run as part of systemd-userdbd.service we don't want to talk to ourselves
                 * anyway). */
                is_nss = streq(de->d_name, "io.systemd.NameServiceSwitch");
                if ((flags & (USERDB_AVOID_NSS|USERDB_AVOID_MULTIPLEXER)) && is_nss)
                        continue;

                if (strv_contains(except, de->d_name))
                        continue;

                if (only && !strv_contains(only, de->d_name))
                        continue;

                p = path_join("/run/systemd/userdb/", de->d_name);
                if (!p)
                        return -ENOMEM;

                patched_query = json_variant_ref(query);
                r = json_variant_set_field_string(&patched_query, "service", de->d_name);
                if (r < 0)
                        return log_debug_errno(r, "Unable to set service JSON field: %m");

                r = userdb_connect(iterator, p, method, more, patched_query);
                if (is_nss && r >= 0) /* Turn off fallback NSS if we found the NSS service and could connect
                                       * to it */
                        iterator->nss_covered = true;

                if (ret == 0 && r < 0)
                        ret = r;
        }

        if (set_isempty(iterator->links))
                return ret; /* propagate last error we saw if we couldn't connect to anything. */

        /* We connected to some services, in this case, ignore the ones we failed on */
        return 0;
}

static int userdb_process(
                UserDBIterator *iterator,
                UserRecord **ret_user_record,
                GroupRecord **ret_group_record,
                char **ret_user_name,
                char **ret_group_name) {

        int r;

        assert(iterator);

        for (;;) {
                if (iterator->what == LOOKUP_USER && iterator->found_user) {
                        if (ret_user_record)
                                *ret_user_record = TAKE_PTR(iterator->found_user);
                        else
                                iterator->found_user = user_record_unref(iterator->found_user);

                        if (ret_group_record)
                                *ret_group_record = NULL;
                        if (ret_user_name)
                                *ret_user_name = NULL;
                        if (ret_group_name)
                                *ret_group_name = NULL;

                        return 0;
                }

                if (iterator->what == LOOKUP_GROUP && iterator->found_group) {
                        if (ret_group_record)
                                *ret_group_record = TAKE_PTR(iterator->found_group);
                        else
                                iterator->found_group = group_record_unref(iterator->found_group);

                        if (ret_user_record)
                                *ret_user_record = NULL;
                        if (ret_user_name)
                                *ret_user_name = NULL;
                        if (ret_group_name)
                                *ret_group_name = NULL;

                        return 0;
                }

                if (iterator->what == LOOKUP_MEMBERSHIP && iterator->found_user_name && iterator->found_group_name) {
                        if (ret_user_name)
                                *ret_user_name = TAKE_PTR(iterator->found_user_name);
                        else
                                iterator->found_user_name = mfree(iterator->found_user_name);

                        if (ret_group_name)
                                *ret_group_name = TAKE_PTR(iterator->found_group_name);
                        else
                                iterator->found_group_name = mfree(iterator->found_group_name);

                        if (ret_user_record)
                                *ret_user_record = NULL;
                        if (ret_group_record)
                                *ret_group_record = NULL;

                        return 0;
                }

                if (set_isempty(iterator->links)) {
                        if (iterator->error == 0)
                                return -ESRCH;

                        return -abs(iterator->error);
                }

                if (!iterator->event)
                        return -ESRCH;

                r = sd_event_run(iterator->event, UINT64_MAX);
                if (r < 0)
                        return r;
        }
}

static int synthetic_root_user_build(UserRecord **ret) {
        return user_record_build(
                        ret,
                        JSON_BUILD_OBJECT(JSON_BUILD_PAIR("userName", JSON_BUILD_STRING("root")),
                                          JSON_BUILD_PAIR("uid", JSON_BUILD_UNSIGNED(0)),
                                          JSON_BUILD_PAIR("gid", JSON_BUILD_UNSIGNED(0)),
                                          JSON_BUILD_PAIR("homeDirectory", JSON_BUILD_STRING("/root")),
                                          JSON_BUILD_PAIR("disposition", JSON_BUILD_STRING("intrinsic"))));
}

static int synthetic_nobody_user_build(UserRecord **ret) {
        return user_record_build(
                        ret,
                        JSON_BUILD_OBJECT(JSON_BUILD_PAIR("userName", JSON_BUILD_STRING(NOBODY_USER_NAME)),
                                          JSON_BUILD_PAIR("uid", JSON_BUILD_UNSIGNED(UID_NOBODY)),
                                          JSON_BUILD_PAIR("gid", JSON_BUILD_UNSIGNED(GID_NOBODY)),
                                          JSON_BUILD_PAIR("shell", JSON_BUILD_STRING(NOLOGIN)),
                                          JSON_BUILD_PAIR("locked", JSON_BUILD_BOOLEAN(true)),
                                          JSON_BUILD_PAIR("disposition", JSON_BUILD_STRING("intrinsic"))));
}

int userdb_by_name(const char *name, UserDBFlags flags, UserRecord **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *query = NULL;
        int r;

        if (!valid_user_group_name(name))
                return -EINVAL;

        r = json_build(&query, JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("userName", JSON_BUILD_STRING(name))));
        if (r < 0)
                return r;

        iterator = userdb_iterator_new(LOOKUP_USER);
        if (!iterator)
                return -ENOMEM;

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetUserRecord", false, query, flags);
        if (r >= 0) {
                r = userdb_process(iterator, ret, NULL, NULL, NULL);
                if (r >= 0)
                        return r;
        }

        if (!FLAGS_SET(flags, USERDB_AVOID_NSS) && !(iterator && iterator->nss_covered)) {
                /* Make sure the NSS lookup doesn't recurse back to us. (EBUSY is fine here, it just means we
                 * already took the lock from our thread, which is totally OK.) */
                r = userdb_nss_compat_disable();
                if (r >= 0 || r == -EBUSY) {
                        iterator->nss_lock = r;

                        /* Client-side NSS fallback */
                        r = nss_user_record_by_name(name, ret);
                        if (r >= 0)
                                return r;
                }
        }

        if (!FLAGS_SET(flags, USERDB_DONT_SYNTHESIZE)) {
                if (streq(name, "root"))
                        return synthetic_root_user_build(ret);

                if (streq(name, NOBODY_USER_NAME) && synthesize_nobody())
                        return synthetic_nobody_user_build(ret);
        }

        return r;
}

int userdb_by_uid(uid_t uid, UserDBFlags flags, UserRecord **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *query = NULL;
        int r;

        if (!uid_is_valid(uid))
                return -EINVAL;

        r = json_build(&query, JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("uid", JSON_BUILD_UNSIGNED(uid))));
        if (r < 0)
                return r;

        iterator = userdb_iterator_new(LOOKUP_USER);
        if (!iterator)
                return -ENOMEM;

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetUserRecord", false, query, flags);
        if (r >= 0) {
                r = userdb_process(iterator, ret, NULL, NULL, NULL);
                if (r >= 0)
                        return r;
        }

        if (!FLAGS_SET(flags, USERDB_AVOID_NSS) && !(iterator && iterator->nss_covered)) {
                r = userdb_nss_compat_disable();
                if (r >= 0 || r == -EBUSY) {
                        iterator->nss_lock = r;

                        /* Client-side NSS fallback */
                        r = nss_user_record_by_uid(uid, ret);
                        if (r >= 0)
                                return r;
                }
        }

        if (!FLAGS_SET(flags, USERDB_DONT_SYNTHESIZE)) {
                if (uid == 0)
                        return synthetic_root_user_build(ret);

                if (uid == UID_NOBODY && synthesize_nobody())
                        return synthetic_nobody_user_build(ret);
        }

        return r;
}

int userdb_all(UserDBFlags flags, UserDBIterator **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        int r;

        assert(ret);

        iterator = userdb_iterator_new(LOOKUP_USER);
        if (!iterator)
                return -ENOMEM;

        iterator->synthesize_root = iterator->synthesize_nobody = !FLAGS_SET(flags, USERDB_DONT_SYNTHESIZE);

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetUserRecord", true, NULL, flags);

        if (!FLAGS_SET(flags, USERDB_AVOID_NSS) && (r < 0 || !iterator->nss_covered)) {
                iterator->nss_lock = userdb_nss_compat_disable();
                if (iterator->nss_lock < 0 && iterator->nss_lock != -EBUSY)
                        return iterator->nss_lock;

                setpwent();
                iterator->nss_iterating = true;
                goto finish;
        }

        if (!FLAGS_SET(flags, USERDB_DONT_SYNTHESIZE))
                goto finish;

        return r;

finish:
        *ret = TAKE_PTR(iterator);
        return 0;
}

int userdb_iterator_get(UserDBIterator *iterator, UserRecord **ret) {
        int r;

        assert(iterator);
        assert(iterator->what == LOOKUP_USER);

        if (iterator->nss_iterating) {
                struct passwd *pw;

                /* If NSS isn't covered elsewhere, let's iterate through it first, since it probably contains
                 * the more traditional sources, which are probably good to show first. */

                pw = getpwent();
                if (pw) {
                        _cleanup_free_ char *buffer = NULL;
                        bool incomplete = false;
                        struct spwd spwd;

                        if (streq_ptr(pw->pw_name, "root"))
                                iterator->synthesize_root = false;
                        if (pw->pw_uid == UID_NOBODY)
                                iterator->synthesize_nobody = false;

                        r = nss_spwd_for_passwd(pw, &spwd, &buffer);
                        if (r < 0) {
                                log_debug_errno(r, "Failed to acquire shadow entry for user %s, ignoring: %m", pw->pw_name);
                                incomplete = ERRNO_IS_PRIVILEGE(r);
                        }

                        r = nss_passwd_to_user_record(pw, r >= 0 ? &spwd : NULL, ret);
                        if (r < 0)
                                return r;

                        if (ret)
                                (*ret)->incomplete = incomplete;
                        return r;
                }

                if (errno != 0)
                        log_debug_errno(errno, "Failure to iterate NSS user database, ignoring: %m");

                iterator->nss_iterating = false;
                endpwent();
        }

        r = userdb_process(iterator, ret, NULL, NULL, NULL);

        if (r < 0) {
                if (iterator->synthesize_root) {
                        iterator->synthesize_root = false;
                        iterator->n_found++;
                        return synthetic_root_user_build(ret);
                }

                if (iterator->synthesize_nobody) {
                        iterator->synthesize_nobody = false;
                        iterator->n_found++;
                        return synthetic_nobody_user_build(ret);
                }
        }

        /* if we found at least one entry, then ignore errors and indicate that we reached the end */
        if (r < 0 && iterator->n_found > 0)
                return -ESRCH;

        return r;
}

static int synthetic_root_group_build(GroupRecord **ret) {
        return group_record_build(
                        ret,
                        JSON_BUILD_OBJECT(JSON_BUILD_PAIR("groupName", JSON_BUILD_STRING("root")),
                                          JSON_BUILD_PAIR("gid", JSON_BUILD_UNSIGNED(0)),
                                          JSON_BUILD_PAIR("disposition", JSON_BUILD_STRING("intrinsic"))));
}

static int synthetic_nobody_group_build(GroupRecord **ret) {
        return group_record_build(
                        ret,
                        JSON_BUILD_OBJECT(JSON_BUILD_PAIR("groupName", JSON_BUILD_STRING(NOBODY_GROUP_NAME)),
                                          JSON_BUILD_PAIR("gid", JSON_BUILD_UNSIGNED(GID_NOBODY)),
                                          JSON_BUILD_PAIR("disposition", JSON_BUILD_STRING("intrinsic"))));
}

int groupdb_by_name(const char *name, UserDBFlags flags, GroupRecord **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *query = NULL;
        int r;

        if (!valid_user_group_name(name))
                return -EINVAL;

        r = json_build(&query, JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("groupName", JSON_BUILD_STRING(name))));
        if (r < 0)
                return r;

        iterator = userdb_iterator_new(LOOKUP_GROUP);
        if (!iterator)
                return -ENOMEM;

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetGroupRecord", false, query, flags);
        if (r >= 0) {
                r = userdb_process(iterator, NULL, ret, NULL, NULL);
                if (r >= 0)
                        return r;
        }

        if (!FLAGS_SET(flags, USERDB_AVOID_NSS) && !(iterator && iterator->nss_covered)) {
                r = userdb_nss_compat_disable();
                if (r >= 0 || r == -EBUSY) {
                        iterator->nss_lock = r;

                        r = nss_group_record_by_name(name, ret);
                        if (r >= 0)
                                return r;
                }
        }

        if (!FLAGS_SET(flags, USERDB_DONT_SYNTHESIZE)) {
                if (streq(name, "root"))
                        return synthetic_root_group_build(ret);

                if (streq(name, NOBODY_GROUP_NAME) && synthesize_nobody())
                        return synthetic_nobody_group_build(ret);
        }

        return r;
}

int groupdb_by_gid(gid_t gid, UserDBFlags flags, GroupRecord **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *query = NULL;
        int r;

        if (!gid_is_valid(gid))
                return -EINVAL;

        r = json_build(&query, JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("gid", JSON_BUILD_UNSIGNED(gid))));
        if (r < 0)
                return r;

        iterator = userdb_iterator_new(LOOKUP_GROUP);
        if (!iterator)
                return -ENOMEM;

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetGroupRecord", false, query, flags);
        if (r >= 0) {
                r = userdb_process(iterator, NULL, ret, NULL, NULL);
                if (r >= 0)
                        return r;
        }

        if (!FLAGS_SET(flags, USERDB_AVOID_NSS) && !(iterator && iterator->nss_covered)) {
                r = userdb_nss_compat_disable();
                if (r >= 0 || r == -EBUSY) {
                        iterator->nss_lock = r;

                        r = nss_group_record_by_gid(gid, ret);
                        if (r >= 0)
                                return r;
                }
        }

        if (!FLAGS_SET(flags, USERDB_DONT_SYNTHESIZE)) {
                if (gid == 0)
                        return synthetic_root_group_build(ret);

                if (gid == GID_NOBODY && synthesize_nobody())
                        return synthetic_nobody_group_build(ret);
        }

        return r;
}

int groupdb_all(UserDBFlags flags, UserDBIterator **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        int r;

        assert(ret);

        iterator = userdb_iterator_new(LOOKUP_GROUP);
        if (!iterator)
                return -ENOMEM;

        iterator->synthesize_root = iterator->synthesize_nobody = !FLAGS_SET(flags, USERDB_DONT_SYNTHESIZE);

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetGroupRecord", true, NULL, flags);

        if (!FLAGS_SET(flags, USERDB_AVOID_NSS) && (r < 0 || !iterator->nss_covered)) {
                iterator->nss_lock = userdb_nss_compat_disable();
                if (iterator->nss_lock < 0 && iterator->nss_lock != -EBUSY)
                        return iterator->nss_lock;

                setgrent();
                iterator->nss_iterating = true;
                goto finish;
        }

        if (!FLAGS_SET(flags, USERDB_DONT_SYNTHESIZE))
                goto finish;

        return r;

finish:
        *ret = TAKE_PTR(iterator);
        return 0;
}

int groupdb_iterator_get(UserDBIterator *iterator, GroupRecord **ret) {
        int r;

        assert(iterator);
        assert(iterator->what == LOOKUP_GROUP);

        if (iterator->nss_iterating) {
                struct group *gr;

                errno = 0;
                gr = getgrent();
                if (gr) {
                        _cleanup_free_ char *buffer = NULL;
                        bool incomplete = false;
                        struct sgrp sgrp;

                        if (streq_ptr(gr->gr_name, "root"))
                                iterator->synthesize_root = false;
                        if (gr->gr_gid == GID_NOBODY)
                                iterator->synthesize_nobody = false;

                        r = nss_sgrp_for_group(gr, &sgrp, &buffer);
                        if (r < 0) {
                                log_debug_errno(r, "Failed to acquire shadow entry for group %s, ignoring: %m", gr->gr_name);
                                incomplete = ERRNO_IS_PRIVILEGE(r);
                        }

                        r = nss_group_to_group_record(gr, r >= 0 ? &sgrp : NULL, ret);
                        if (r < 0)
                                return r;

                        if (ret)
                                (*ret)->incomplete = incomplete;
                        return r;
                }

                if (errno != 0)
                        log_debug_errno(errno, "Failure to iterate NSS group database, ignoring: %m");

                iterator->nss_iterating = false;
                endgrent();
        }

        r = userdb_process(iterator, NULL, ret, NULL, NULL);
        if (r < 0) {
                if (iterator->synthesize_root) {
                        iterator->synthesize_root = false;
                        iterator->n_found++;
                        return synthetic_root_group_build(ret);
                }

                if (iterator->synthesize_nobody) {
                        iterator->synthesize_nobody = false;
                        iterator->n_found++;
                        return synthetic_nobody_group_build(ret);
                }
        }

        /* if we found at least one entry, then ignore errors and indicate that we reached the end */
        if (r < 0 && iterator->n_found > 0)
                return -ESRCH;

        return r;
}

int membershipdb_by_user(const char *name, UserDBFlags flags, UserDBIterator **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *query = NULL;
        int r;

        assert(ret);

        if (!valid_user_group_name(name))
                return -EINVAL;

        r = json_build(&query, JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("userName", JSON_BUILD_STRING(name))));
        if (r < 0)
                return r;

        iterator = userdb_iterator_new(LOOKUP_MEMBERSHIP);
        if (!iterator)
                return -ENOMEM;

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetMemberships", true, query, flags);
        if ((r >= 0 && iterator->nss_covered) || FLAGS_SET(flags, USERDB_AVOID_NSS))
                goto finish;

        iterator->nss_lock = userdb_nss_compat_disable();
        if (iterator->nss_lock < 0 && iterator->nss_lock != -EBUSY)
                return iterator->nss_lock;

        iterator->filter_user_name = strdup(name);
        if (!iterator->filter_user_name)
                return -ENOMEM;

        setgrent();
        iterator->nss_iterating = true;

        r = 0;

finish:
        if (r >= 0)
                *ret = TAKE_PTR(iterator);
        return r;
}

int membershipdb_by_group(const char *name, UserDBFlags flags, UserDBIterator **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *query = NULL;
        _cleanup_(group_record_unrefp) GroupRecord *gr = NULL;
        int r;

        assert(ret);

        if (!valid_user_group_name(name))
                return -EINVAL;

        r = json_build(&query, JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("groupName", JSON_BUILD_STRING(name))));
        if (r < 0)
                return r;

        iterator = userdb_iterator_new(LOOKUP_MEMBERSHIP);
        if (!iterator)
                return -ENOMEM;

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetMemberships", true, query, flags);
        if ((r >= 0 && iterator->nss_covered) || FLAGS_SET(flags, USERDB_AVOID_NSS))
                goto finish;

        iterator->nss_lock = userdb_nss_compat_disable();
        if (iterator->nss_lock < 0 && iterator->nss_lock != -EBUSY)
                return iterator->nss_lock;

        /* We ignore all errors here, since the group might be defined by a userdb native service, and we queried them already above. */
        (void) nss_group_record_by_name(name, &gr);
        if (gr) {
                iterator->members_of_group = strv_copy(gr->members);
                if (!iterator->members_of_group)
                        return -ENOMEM;

                iterator->index_members_of_group = 0;

                iterator->found_group_name = strdup(name);
                if (!iterator->found_group_name)
                        return -ENOMEM;
        }

        r = 0;

finish:
        if (r >= 0)
                *ret = TAKE_PTR(iterator);

        return r;
}

int membershipdb_all(UserDBFlags flags, UserDBIterator **ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        int r;

        assert(ret);

        iterator = userdb_iterator_new(LOOKUP_MEMBERSHIP);
        if (!iterator)
                return -ENOMEM;

        r = userdb_start_query(iterator, "io.systemd.UserDatabase.GetMemberships", true, NULL, flags);
        if ((r >= 0 && iterator->nss_covered) || FLAGS_SET(flags, USERDB_AVOID_NSS))
                goto finish;

        iterator->nss_lock = userdb_nss_compat_disable();
        if (iterator->nss_lock < 0 && iterator->nss_lock != -EBUSY)
                return iterator->nss_lock;

        setgrent();
        iterator->nss_iterating = true;

        r = 0;

finish:
        if (r >= 0)
                *ret = TAKE_PTR(iterator);

        return r;
}

int membershipdb_iterator_get(
                UserDBIterator *iterator,
                char **ret_user,
                char **ret_group) {

        int r;

        assert(iterator);

        for (;;) {
                /* If we are iteratring through NSS acquire a new group entry if we haven't acquired one yet. */
                if (!iterator->members_of_group) {
                        struct group *g;

                        if (!iterator->nss_iterating)
                                break;

                        assert(!iterator->found_user_name);
                        do {
                                errno = 0;
                                g = getgrent();
                                if (!g) {
                                        if (errno != 0)
                                                log_debug_errno(errno, "Failure during NSS group iteration, ignoring: %m");
                                        break;
                                }

                        } while (iterator->filter_user_name ? !strv_contains(g->gr_mem, iterator->filter_user_name) :
                                                              strv_isempty(g->gr_mem));

                        if (g) {
                                r = free_and_strdup(&iterator->found_group_name, g->gr_name);
                                if (r < 0)
                                        return r;

                                if (iterator->filter_user_name)
                                        iterator->members_of_group = strv_new(iterator->filter_user_name);
                                else
                                        iterator->members_of_group = strv_copy(g->gr_mem);
                                if (!iterator->members_of_group)
                                        return -ENOMEM;

                                iterator->index_members_of_group = 0;
                        } else {
                                iterator->nss_iterating = false;
                                endgrent();
                                break;
                        }
                }

                assert(iterator->found_group_name);
                assert(iterator->members_of_group);
                assert(!iterator->found_user_name);

                if (iterator->members_of_group[iterator->index_members_of_group]) {
                        _cleanup_free_ char *cu = NULL, *cg = NULL;

                        if (ret_user) {
                                cu = strdup(iterator->members_of_group[iterator->index_members_of_group]);
                                if (!cu)
                                        return -ENOMEM;
                        }

                        if (ret_group) {
                                cg = strdup(iterator->found_group_name);
                                if (!cg)
                                        return -ENOMEM;
                        }

                        if (ret_user)
                                *ret_user = TAKE_PTR(cu);

                        if (ret_group)
                                *ret_group = TAKE_PTR(cg);

                        iterator->index_members_of_group++;
                        return 0;
                }

                iterator->members_of_group = strv_free(iterator->members_of_group);
                iterator->found_group_name = mfree(iterator->found_group_name);
        }

        r = userdb_process(iterator, NULL, NULL, ret_user, ret_group);
        if (r < 0 && iterator->n_found > 0)
                return -ESRCH;

        return r;
}

int membershipdb_by_group_strv(const char *name, UserDBFlags flags, char ***ret) {
        _cleanup_(userdb_iterator_freep) UserDBIterator *iterator = NULL;
        _cleanup_strv_free_ char **members = NULL;
        int r;

        assert(name);
        assert(ret);

        r = membershipdb_by_group(name, flags, &iterator);
        if (r < 0)
                return r;

        for (;;) {
                _cleanup_free_ char *user_name = NULL;

                r = membershipdb_iterator_get(iterator, &user_name, NULL);
                if (r == -ESRCH)
                        break;
                if (r < 0)
                        return r;

                r = strv_consume(&members, TAKE_PTR(user_name));
                if (r < 0)
                        return r;
        }

        strv_sort(members);
        strv_uniq(members);

        *ret = TAKE_PTR(members);
        return 0;
}

static int userdb_thread_sockaddr(struct sockaddr_un *ret_sa, socklen_t *ret_salen) {
        static const uint8_t
                k1[16] = { 0x35, 0xc1, 0x1f, 0x41, 0x59, 0xc6, 0xa0, 0xf9, 0x33, 0x4b, 0x17, 0x3d, 0xb9, 0xf6, 0x14, 0xd9 },
                k2[16] = { 0x6a, 0x11, 0x4c, 0x37, 0xe5, 0xa3, 0x8c, 0xa6, 0x93, 0x55, 0x64, 0x8c, 0x93, 0xee, 0xa1, 0x7b };

        struct siphash sh;
        uint64_t x, y;
        pid_t tid;
        void *p;

        assert(ret_sa);
        assert(ret_salen);

        /* This calculates an AF_UNIX socket address in the abstract namespace whose existence works as an
         * indicator whether to emulate NSS records for complex user records that are also available via the
         * varlink protocol. The name of the socket is picked in a way so that:
         *
         *     → it is per-thread (by hashing from the TID)
         *
         *     → is not guessable for foreign processes (by hashing from the — hopefully secret — AT_RANDOM
         *       value every process gets passed from the kernel
         *
         * By using a socket the NSS emulation can be nicely turned off for limited amounts of time only,
         * simply controlled by the lifetime of the fd itself. By using an AF_UNIX socket in the abstract
         * namespace the lock is automatically cleaned up when the process dies abnormally.
         *
         */

        p = ULONG_TO_PTR(getauxval(AT_RANDOM));
        if (!p)
                return -EIO;

        tid = gettid();

        siphash24_init(&sh, k1);
        siphash24_compress(p, 16, &sh);
        siphash24_compress(&tid, sizeof(tid), &sh);
        x = siphash24_finalize(&sh);

        siphash24_init(&sh, k2);
        siphash24_compress(p, 16, &sh);
        siphash24_compress(&tid, sizeof(tid), &sh);
        y = siphash24_finalize(&sh);

        *ret_sa = (struct sockaddr_un) {
                .sun_family = AF_UNIX,
        };

        sprintf(ret_sa->sun_path + 1, "userdb-%016" PRIx64 "%016" PRIx64, x, y);
        *ret_salen = offsetof(struct sockaddr_un, sun_path) + 1 + 7 + 32;

        return 0;
}

int userdb_nss_compat_is_enabled(void) {
        _cleanup_close_ int fd = -1;
        union sockaddr_union sa;
        socklen_t salen;
        int r;

        /* Tests whether the NSS compatibility logic is currently turned on for the invoking thread. Returns
         * true if NSS compatibility is turned on, i.e. whether NSS records shall be synthesized from complex
         * user records. */

        r = userdb_thread_sockaddr(&sa.un, &salen);
        if (r < 0)
                return r;

        fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0);
        if (fd < 0)
                return -errno;

        /* Try to connect(). This doesn't do anything really, except that it checks whether the socket
         * address is bound at all. */
        if (connect(fd, &sa.sa, salen) < 0) {
                if (errno == ECONNREFUSED) /* the socket is not bound, hence NSS emulation shall be done */
                        return true;

                return -errno;
        }

        return false;
}

int userdb_nss_compat_disable(void) {
        _cleanup_close_ int fd = -1;
        union sockaddr_union sa;
        socklen_t salen;
        int r;

        /* Turn off the NSS compatibility logic for the invoking thread. By default NSS records are
         * synthesized for all complex user records looked up via NSS. If this call is invoked this is
         * disabled for the invoking thread, but only for it. A caller that natively supports the varlink
         * user record protocol may use that to turn off the compatibility for NSS lookups. */

        r = userdb_thread_sockaddr(&sa.un, &salen);
        if (r < 0)
                return r;

        fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0)
                return -errno;

        if (bind(fd, &sa.sa, salen) < 0) {
                if (errno == EADDRINUSE) /* lock already taken, convert this into a recognizable error */
                        return -EBUSY;

                return -errno;
        }

        return TAKE_FD(fd);
}
