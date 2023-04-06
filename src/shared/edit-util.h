/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#define DROPIN_MARKER_START "### Anything between here and the comment below will become the contents of the drop-in file"
#define DROPIN_MARKER_END "### Edits below this comment will be discarded"

typedef struct EditFile EditFile;
typedef struct EditFileContext EditFileContext;

struct EditFile {
        EditFileContext *context;
        char *path;
        char *original_path;
        char **comment_paths;
        char *temp;
        unsigned line;
};

struct EditFileContext {
        EditFile *files;
        size_t n_files;
        const char *marker_start;
        const char *marker_end;
        bool remove_parent;
        bool overwrite_with_origin; /* whether to always overwrite target with original file */
};

void edit_file_context_done(EditFileContext *context);

bool edit_files_contains(const EditFileContext *context, const char *path);

int edit_files_add(
                EditFileContext *context,
                const char *path,
                const char *original_path,
                char * const *comment_paths);

int do_edit_files_and_install(EditFileContext *context);
