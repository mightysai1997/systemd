/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

int dm_deferred_remove_cancel(const char *name);
int dm_get_devnode(const char *name, char **ret_devnode);
