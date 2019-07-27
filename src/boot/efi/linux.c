/* SPDX-License-Identifier: LGPL-2.1+ */

#include <efi.h>
#include <efilib.h>

#include "linux.h"
#include "util.h"

#ifdef __x86_64__
typedef VOID(*handover_f)(VOID *image, EFI_SYSTEM_TABLE *table, struct boot_params *params);
static VOID linux_efi_handover(EFI_HANDLE image, struct boot_params *params) {
        handover_f handover;

        asm volatile ("cli");
        handover = (handover_f)((UINTN)params->hdr.code32_start + 512 + params->hdr.handover_offset);
        handover(image, ST, params);
}
#else
typedef VOID(*handover_f)(VOID *image, EFI_SYSTEM_TABLE *table, struct boot_params *params) __attribute__((regparm(0)));
static VOID linux_efi_handover(EFI_HANDLE image, struct boot_params *params) {
        handover_f handover;

        handover = (handover_f)((UINTN)params->hdr.code32_start + params->hdr.handover_offset);
        handover(image, ST, params);
}
#endif

EFI_STATUS linux_exec(EFI_HANDLE *image,
                      CHAR8 *cmdline, UINTN cmdline_len,
                      UINTN linux_addr,
                      UINTN initrd_addr, UINTN initrd_size) {
        struct boot_params *image_params;
        struct boot_params *boot_params;
        UINT8 setup_sectors;
        EFI_PHYSICAL_ADDRESS addr;
        EFI_STATUS err;

        image_params = (struct boot_params *) linux_addr;

        if (image_params->hdr.boot_flag != 0xAA55 ||
            image_params->hdr.header != SETUP_MAGIC ||
            image_params->hdr.version < 0x20b ||
            !image_params->hdr.relocatable_kernel)
                return EFI_LOAD_ERROR;

        boot_params = (struct boot_params *) 0xFFFFFFFF;
        err = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress, EfiLoaderData,
                                EFI_SIZE_TO_PAGES(0x4000), (EFI_PHYSICAL_ADDRESS*) &boot_params);
        if (EFI_ERROR(err))
                return err;

        ZeroMem(boot_params, 0x4000);
        CopyMem(&boot_params->hdr, &image_params->hdr, sizeof(struct setup_header));
        boot_params->hdr.type_of_loader = 0xff;
        setup_sectors = image_params->hdr.setup_sects > 0 ? image_params->hdr.setup_sects : 4;
        boot_params->hdr.code32_start = (UINT32)linux_addr + (setup_sectors + 1) * 512;

        if (cmdline) {
                addr = 0xA0000;
                err = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress, EfiLoaderData,
                                        EFI_SIZE_TO_PAGES(cmdline_len + 1), &addr);
                if (EFI_ERROR(err))
                        return err;
                CopyMem((VOID *)(UINTN)addr, cmdline, cmdline_len);
                ((CHAR8 *)(UINTN)addr)[cmdline_len] = 0;
                boot_params->hdr.cmd_line_ptr = (UINT32)addr;
        }

        boot_params->hdr.ramdisk_image = (UINT32)initrd_addr;
        boot_params->hdr.ramdisk_size = (UINT32)initrd_size;

        linux_efi_handover(image, boot_params);
        return EFI_LOAD_ERROR;
}
