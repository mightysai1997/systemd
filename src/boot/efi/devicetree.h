/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

struct devicetree_state {
        EFI_PHYSICAL_ADDRESS addr;
        UINTN pages;
        VOID *orig;
};

EFI_STATUS devicetree_install(struct devicetree_state *state, EFI_FILE_HANDLE root_dir, CHAR16 *name);
EFI_STATUS devicetree_install_from_memory(
                struct devicetree_state *state, VOID *dtb_buffer, UINTN dtb_length);
void devicetree_cleanup(struct devicetree_state *state);
