/* SPDX-License-Identifier: LGPL-2.1+ */

#include "dns-domain.h"
#include "errno-util.h"
#include "home-util.h"
#include "libcrypt-util.h"
#include "memory-util.h"
#include "path-util.h"
#include "string-util.h"
#include "strv.h"
#include "user-util.h"

bool suitable_user_name(const char *name) {

        /* Checks whether the specified name is suitable for management via homed. Note that client-side we
         * usually validate with the simple valid_user_group_name(), while server-side we are a bit more
         * restrictive, so that we can change the rules server-side without having to update things
         * client-side too. */

        if (!valid_user_group_name(name))
                return false;

        /* We generally rely on NSS to tell us which users not to care for, but let's filter out some
         * particularly well-known users. */
        if (STR_IN_SET(name,
                       "root",
                       "nobody",
                       NOBODY_USER_NAME, NOBODY_GROUP_NAME))
                return false;

        /* Let's also defend our own namespace, as well as Debian's (unwritten?) logic of prefixing system
         * users with underscores. */
        if (STARTSWITH_SET(name, "systemd-", "_"))
                return false;

        return true;
}

int suitable_realm(const char *realm) {
        _cleanup_free_ char *normalized = NULL;
        int r;

        /* Similar to the above: let's validate the realm a bit stricter server-side than client side */

        r = dns_name_normalize(realm, 0, &normalized); /* this also checks general validity */
        if (r == -EINVAL)
                return 0;
        if (r < 0)
                return r;

        if (!streq(realm, normalized)) /* is this normalized? */
                return false;

        if (dns_name_is_root(realm)) /* Don't allow top level domain */
                return false;

        return true;
}

int suitable_image_path(const char *path) {

        return !empty_or_root(path) &&
                path_is_valid(path) &&
                path_is_absolute(path);
}

int split_user_name_realm(const char *t, char **ret_user_name, char **ret_realm) {
        _cleanup_free_ char *user_name = NULL, *realm = NULL;
        const char *c;
        int r;

        assert(t);
        assert(ret_user_name);
        assert(ret_realm);

        c = strchr(t, '@');
        if (!c) {
                user_name = strdup(t);
                if (!user_name)
                        return -ENOMEM;
        } else {
                user_name = strndup(t, c - t);
                if (!user_name)
                        return -ENOMEM;

                realm = strdup(c + 1);
                if (!realm)
                        return -ENOMEM;
        }

        if (!suitable_user_name(user_name))
                return -EINVAL;

        if (realm) {
                r = suitable_realm(realm);
                if (r < 0)
                        return r;
                if (r == 0)
                        return -EINVAL;
        }

        *ret_user_name = TAKE_PTR(user_name);
        *ret_realm = TAKE_PTR(realm);

        return 0;
}

int bus_message_append_secret(sd_bus_message *m, UserRecord *secret) {
        _cleanup_(erase_and_freep) char *formatted = NULL;
        JsonVariant *v;
        int r;

        assert(m);
        assert(secret);

        if (!FLAGS_SET(secret->mask, USER_RECORD_SECRET))
                return sd_bus_message_append(m, "s", "{}");

        v = json_variant_by_key(secret->json, "secret");
        if (!v)
                return -EINVAL;

        r = json_variant_format(v, 0, &formatted);
        if (r < 0)
                return r;

        return sd_bus_message_append(m, "s", formatted);
}

int test_password_one(const char *hashed_password, const char *password) {
        struct crypt_data cc = {};
        const char *k;
        bool b;

        errno = 0;
        k = crypt_r(password, hashed_password, &cc);
        if (!k) {
                explicit_bzero_safe(&cc, sizeof(cc));
                return errno_or_else(EINVAL);
        }

        b = streq(k, hashed_password);
        explicit_bzero_safe(&cc, sizeof(cc));
        return b;
}

int test_password_many(char **hashed_password, const char *password) {
        char **hpw;
        int r;

        STRV_FOREACH(hpw, hashed_password) {
                r = test_password_one(*hpw, password);
                if (r < 0)
                        return r;
                if (r > 0)
                        return true;
        }

        return false;
}
