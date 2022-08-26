/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

typedef enum UnitFilePresetMode UnitFilePresetMode;
typedef enum InstallChangeType InstallChangeType;
typedef enum UnitFileFlags UnitFileFlags;
typedef enum UnitFileType UnitFileType;
typedef struct InstallChange InstallChange;
typedef struct UnitFileList UnitFileList;
typedef struct UnitFileInstallInfo UnitFileInstallInfo;

#include "hashmap.h"
#include "macro.h"
#include "path-lookup.h"
#include "strv.h"
#include "unit-file.h"
#include "unit-name.h"

enum UnitFilePresetMode {
        UNIT_FILE_PRESET_FULL,
        UNIT_FILE_PRESET_ENABLE_ONLY,
        UNIT_FILE_PRESET_DISABLE_ONLY,
        _UNIT_FILE_PRESET_MAX,
        _UNIT_FILE_PRESET_INVALID = -EINVAL,
};

/* This enum type is anonymous, since we usually store it in an 'int', as we overload it with negative errno
 * values. */
enum {
        UNIT_FILE_SYMLINK,
        UNIT_FILE_UNLINK,
        UNIT_FILE_IS_MASKED,
        UNIT_FILE_IS_DANGLING,
        UNIT_FILE_DESTINATION_NOT_PRESENT,
        UNIT_FILE_AUXILIARY_FAILED,
        _UNIT_FILE_CHANGE_TYPE_MAX,
        _UNIT_FILE_CHANGE_TYPE_INVALID = -EINVAL,
};

enum UnitFileFlags {
        UNIT_FILE_RUNTIME                  = 1 << 0, /* Public API via DBUS, do not change */
        UNIT_FILE_FORCE                    = 1 << 1, /* Public API via DBUS, do not change */
        UNIT_FILE_PORTABLE                 = 1 << 2, /* Public API via DBUS, do not change */
        UNIT_FILE_DRY_RUN                  = 1 << 3,
        UNIT_FILE_IGNORE_AUXILIARY_FAILURE = 1 << 4,
        _UNIT_FILE_FLAGS_MASK_PUBLIC = UNIT_FILE_RUNTIME|UNIT_FILE_PORTABLE|UNIT_FILE_FORCE,
};

/* type can either one of the UNIT_FILE_SYMLINK, UNIT_FILE_UNLINK, … listed above, or a negative errno value.
 * If source is specified, it should be the contents of the path symlink. In case of an error, source should
 * be the existing symlink contents or NULL. */
struct InstallChange {
        int type_or_errno; /* UNIT_FILE_SYMLINK, … if positive, errno if negative */
        char *path;
        char *source;
};

static inline bool install_changes_have_modification(const InstallChange* changes, size_t n_changes) {
        for (size_t i = 0; i < n_changes; i++)
                if (IN_SET(changes[i].type_or_errno, UNIT_FILE_SYMLINK, UNIT_FILE_UNLINK))
                        return true;
        return false;
}

struct UnitFileList {
        char *path;
        UnitFileState state;
};

enum UnitFileType {
        UNIT_FILE_TYPE_REGULAR,
        UNIT_FILE_TYPE_LINKED,
        UNIT_FILE_TYPE_ALIAS,
        UNIT_FILE_TYPE_MASKED,
        _UNIT_FILE_TYPE_MAX,
        _UNIT_FILE_TYPE_INVALID = -EINVAL,
};

struct UnitFileInstallInfo {
        char *name;
        char *path;
        char *root;

        char **aliases;
        char **wanted_by;
        char **required_by;
        char **also;

        char *default_instance;
        char *symlink_target;

        UnitFileType type;
        bool auxiliary;
};

int install_unit_enable(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                char **names_or_paths,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_disable(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                char **names,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_reenable(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                char **names_or_paths,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_preset(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                char **names,
                UnitFilePresetMode mode,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_preset_all(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                UnitFilePresetMode mode,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_mask(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                char **names,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_unmask(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                char **names,
                InstallChange **changes,
                size_t *n_changes);
int unit_file_link(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                char **files,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_revert(
                LookupScope scope,
                const char *root_dir,
                char **names,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_set_default(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                const char *name,
                InstallChange **changes,
                size_t *n_changes);
int install_unit_get_default(
                LookupScope scope,
                const char *root_dir,
                char **name);
int install_unit_add_dependency(
                LookupScope scope,
                UnitFileFlags flags,
                const char *root_dir,
                char **names,
                const char *target,
                UnitDependency dep,
                InstallChange **changes,
                size_t *n_changes);

int unit_file_lookup_state(
                LookupScope scope,
                const LookupPaths *paths,
                const char *name,
                UnitFileState *ret);

int unit_file_get_state(LookupScope scope, const char *root_dir, const char *filename, UnitFileState *ret);
int unit_file_exists(LookupScope scope, const LookupPaths *paths, const char *name);

int unit_file_get_list(LookupScope scope, const char *root_dir, Hashmap *h, char **states, char **patterns);
Hashmap* unit_file_list_free(Hashmap *h);

int install_changes_add(InstallChange **changes, size_t *n_changes, int type, const char *path, const char *source);
void install_changes_free(InstallChange *changes, size_t n_changes);
void install_changes_dump(int r, const char *verb, const InstallChange *changes, size_t n_changes, bool quiet);

int install_unit_verify_alias(
                const UnitFileInstallInfo *info,
                const char *dst,
                char **ret_dst,
                InstallChange **changes,
                size_t *n_changes);

typedef struct UnitFilePresetRule UnitFilePresetRule;

typedef struct {
        UnitFilePresetRule *rules;
        size_t n_rules;
        bool initialized;
} UnitFilePresets;

void install_unit_presets_freep(UnitFilePresets *p);
int install_unit_query_preset(LookupScope scope, const char *root_dir, const char *name, UnitFilePresets *cached);

const char *unit_file_state_to_string(UnitFileState s) _const_;
UnitFileState unit_file_state_from_string(const char *s) _pure_;
/* from_string conversion is unreliable because of the overlap between -EPERM and -1 for error. */

const char *unit_file_change_type_to_string(int s) _const_;
int unit_file_change_type_from_string(const char *s) _pure_;

const char *unit_file_preset_mode_to_string(UnitFilePresetMode m) _const_;
UnitFilePresetMode unit_file_preset_mode_from_string(const char *s) _pure_;
