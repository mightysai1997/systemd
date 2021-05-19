/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "cryptenroll-list.h"
#include "cryptenroll.h"
#include "format-table.h"
#include "parse-util.h"

int list_enrolled(struct crypt_device *cd) {

        struct keyslot_metadata {
                int slot;
                const char *type;
        } *keyslot_metadata = NULL;
        _cleanup_(table_unrefp) Table *t = NULL;
        size_t n_keyslot_metadata = 0;
        int slot_max, r;
        TableCell *cell;

        assert(cd);

        /* First step, find out all currently used slots */
        assert_se((slot_max = crypt_keyslot_max(CRYPT_LUKS2)) > 0);
        for (int slot = 0; slot < slot_max; slot++) {
                crypt_keyslot_info status;

                status = crypt_keyslot_status(cd, slot);
                if (!IN_SET(status, CRYPT_SLOT_ACTIVE, CRYPT_SLOT_ACTIVE_LAST))
                        continue;

                if (!GREEDY_REALLOC(keyslot_metadata, n_keyslot_metadata+1))
                        return log_oom();

                keyslot_metadata[n_keyslot_metadata++] = (struct keyslot_metadata) {
                        .slot = slot,
                };
        }

        /* Second step, enumerate through all tokens, and update the slot table, indicating what kind of
         * token they are assigned to */
        for (int token = 0; token < sym_crypt_token_max(CRYPT_LUKS2); token++) {
                _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
                const char *type;
                JsonVariant *w, *z;
                EnrollType et;

                r = cryptsetup_get_token_as_json(cd, token, NULL, &v);
                if (IN_SET(r, -ENOENT, -EINVAL))
                        continue;
                if (r < 0) {
                        log_warning_errno(r, "Failed to read JSON token data off disk, ignoring: %m");
                        continue;
                }

                w = json_variant_by_key(v, "type");
                if (!w || !json_variant_is_string(w)) {
                        log_warning("Token JSON data lacks type field, ignoring.");
                        continue;
                }

                et = luks2_token_type_from_string(json_variant_string(w));
                if (et < 0)
                        type = "other";
                else
                        type = enroll_type_to_string(et);

                w = json_variant_by_key(v, "keyslots");
                if (!w || !json_variant_is_array(w)) {
                        log_warning("Token JSON data lacks keyslots field, ignoring.");
                        continue;
                }

                JSON_VARIANT_ARRAY_FOREACH(z, w) {
                        unsigned u;

                        if (!json_variant_is_string(z)) {
                                log_warning("Token JSON data's keyslot field is not an array of strings, ignoring.");
                                continue;
                        }

                        r = safe_atou(json_variant_string(z), &u);
                        if (r < 0) {
                                log_warning_errno(r, "Token JSON data's keyslot filed is not an integer formatted as string, ignoring.");
                                continue;
                        }

                        for (size_t i = 0; i < n_keyslot_metadata; i++) {
                                if ((unsigned) keyslot_metadata[i].slot != u)
                                        continue;

                                if (keyslot_metadata[i].type) /* Slot claimed multiple times? */
                                        keyslot_metadata[i].type = POINTER_MAX;
                                else
                                        keyslot_metadata[i].type = type;
                        }
                }
        }

        /* Finally, create a table out of it all */
        t = table_new("slot", "type");
        if (!t)
                return log_oom();

        assert_se(cell = table_get_cell(t, 0, 0));
        (void) table_set_align_percent(t, cell, 100);

        for (size_t i = 0; i < n_keyslot_metadata; i++) {
                r = table_add_many(
                                t,
                                TABLE_INT, keyslot_metadata[i].slot,
                                TABLE_STRING, keyslot_metadata[i].type == POINTER_MAX ? "conflict" :
                                              keyslot_metadata[i].type ?: "password");
                if (r < 0)
                        return table_log_add_error(r);
        }

        if (table_get_rows(t) <= 1) {
                log_info("No slots found.");
                return 0;
        }

        r = table_print(t, stdout);
        if (r < 0)
                return log_error_errno(r, "Failed to show slot table: %m");

        return 0;
}
