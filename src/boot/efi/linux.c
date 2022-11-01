/* SPDX-License-Identifier: LGPL-2.1-or-later */

/*
 * Generic Linux boot protocol using the EFI/PE entry point of the kernel. Passes
 * initrd with the LINUX_INITRD_MEDIA_GUID DevicePath and cmdline with
 * EFI LoadedImageProtocol.
 *
 * This method works for Linux 5.8 and newer on ARM/Aarch64, x86/x68_64 and RISC-V.
 */

#include <efi.h>
#include <efilib.h>

#include "initrd.h"
#include "linux.h"
#include "pe.h"
#include "secure-boot.h"
#include "util.h"

#define STUB_PAYLOAD_GUID \
        { 0x55c5d1f8, 0x04cd, 0x46b5, { 0x8a, 0x20, 0xe5, 0x6c, 0xbb, 0x30, 0x52, 0xd0 } }

static EFIAPI EFI_STATUS security_hook(
                const SecurityOverride *this, uint32_t authentication_status, const EFI_DEVICE_PATH *file) {

        assert(this);
        assert(this->hook == security_hook);

        if (file == this->payload_device_path)
                return EFI_SUCCESS;

        return this->original_security->FileAuthenticationState(
                        this->original_security, authentication_status, file);
}

static EFIAPI EFI_STATUS security2_hook(
                const SecurityOverride *this,
                const EFI_DEVICE_PATH *device_path,
                void *file_buffer,
                size_t file_size,
                BOOLEAN boot_policy) {

        assert(this);
        assert(this->hook == security2_hook);

        if (file_buffer == this->payload && file_size == this->payload_len &&
            device_path == this->payload_device_path)
                return EFI_SUCCESS;

        return this->original_security2->FileAuthentication(
                        this->original_security2, device_path, file_buffer, file_size, boot_policy);
}

static EFI_STATUS load_image(EFI_HANDLE parent, const void *source, size_t len, EFI_HANDLE *ret_image) {
        assert(parent);
        assert(source);
        assert(ret_image);

        /* We could pass a NULL device path, but it's nicer to provide something and it allows us to identify
         * the loaded image from within the security hooks. */
        struct {
                VENDOR_DEVICE_PATH payload;
                EFI_DEVICE_PATH end;
        } _packed_ payload_device_path = {
                .payload = {
                        .Header = {
                                .Type = MEDIA_DEVICE_PATH,
                                .SubType = MEDIA_VENDOR_DP,
                                .Length = { sizeof(payload_device_path.payload), 0 },
                        },
                        .Guid = STUB_PAYLOAD_GUID,
                },
                .end = {
                        .Type = END_DEVICE_PATH_TYPE,
                        .SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE,
                        .Length = { sizeof(payload_device_path.end), 0 },
                },
        };

        /* We want to support unsigned kernel images as payload, which is safe to do under secure boot
         * because it is embedded in this stub loader (and since it is already running it must be trusted). */
        SecurityOverride security_override = {
                .hook = security_hook,
                .payload = source,
                .payload_len = len,
                .payload_device_path = &payload_device_path.payload.Header,
        }, security2_override = {
                .hook = security2_hook,
                .payload = source,
                .payload_len = len,
                .payload_device_path = &payload_device_path.payload.Header,
        };

        install_security_override(&security_override, &security2_override);

        EFI_STATUS ret = BS->LoadImage(
                        /*BootPolicy=*/false,
                        parent,
                        &payload_device_path.payload.Header,
                        (void *) source,
                        len,
                        ret_image);

        uninstall_security_override(&security_override, &security2_override);

        return ret;
}

EFI_STATUS linux_exec(
                EFI_HANDLE parent,
                const void *load_options, size_t load_options_size,
                const void *linux_buffer, size_t linux_length,
                const void *initrd_buffer, size_t initrd_length) {

        uint32_t compat_address;
        EFI_STATUS err;

        assert(parent);
        assert(load_options || load_options_size == 0);
        assert(linux_buffer && linux_length > 0);
        assert(initrd_buffer || initrd_length == 0);

        err = pe_kernel_info(linux_buffer, &compat_address);
#if defined(__i386__) || defined(__x86_64__)
        if (err == EFI_UNSUPPORTED)
                /* Kernel is too old to support LINUX_INITRD_MEDIA_GUID, try the deprecated EFI handover
                 * protocol. */
                return linux_exec_efi_handover(
                                parent,
                                load_options,
                                load_options_size,
                                linux_buffer,
                                linux_length,
                                initrd_buffer,
                                initrd_length);
#endif
        if (err != EFI_SUCCESS)
                return log_error_status_stall(err, u"Bad kernel image: %r", err);

        _cleanup_(unload_imagep) EFI_HANDLE kernel_image = NULL;
        err = load_image(parent, linux_buffer, linux_length, &kernel_image);
        if (err != EFI_SUCCESS)
                return log_error_status_stall(err, u"Error loading kernel image: %r", err);

        EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
        err = BS->HandleProtocol(kernel_image, &LoadedImageProtocol, (void **) &loaded_image);
        if (err != EFI_SUCCESS)
                return log_error_status_stall(err, u"Error getting kernel loaded image protocol: %r", err);

        if (load_options) {
                loaded_image->LoadOptions = (void *) load_options;
                loaded_image->LoadOptionsSize = load_options_size;
        }

        _cleanup_(cleanup_initrd) EFI_HANDLE initrd_handle = NULL;
        err = initrd_register(initrd_buffer, initrd_length, &initrd_handle);
        if (err != EFI_SUCCESS)
                return log_error_status_stall(err, u"Error registering initrd: %r", err);

        err = BS->StartImage(kernel_image, NULL, NULL);

        /* Try calling the kernel compat entry point if one exists. */
        if (err == EFI_UNSUPPORTED && compat_address > 0) {
                EFI_IMAGE_ENTRY_POINT compat_entry =
                                (EFI_IMAGE_ENTRY_POINT) ((uint8_t *) loaded_image->ImageBase + compat_address);
                err = compat_entry(kernel_image, ST);
        }

        return log_error_status_stall(err, u"Error starting kernel image: %r", err);
}
