/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "log.h"
#include "proto/rng.h"
#include "proto/simple-text-io.h"
#include "util.h"

static unsigned log_count = 0;

#define DEBUGCON_PREFIX u"systemd: "
#define DEBUGCON_ACK 0xE9
#define DEBUGCON_PORT 0x402

void freeze(void) {
        for (;;)
                BS->Stall(60 * 1000 * 1000);
}

#if defined(__i386__) || defined(__x86_64__)
static bool log_has_debugcon(void) {
        static bool init;
        static bool present;

        if (!init) {
                present = (inb(DEBUGCON_PORT) == DEBUGCON_ACK);
                init = true;
        }
        return present;
}

static void log_debugcon(const char16_t *msg) {
        size_t i;

        if (!log_has_debugcon())
                return;

        for (i = 0; msg[i]; i++)
                outb(DEBUGCON_PORT, msg[i]);
}
#else /* ! __i386__ && ! __x86_64__ */
static void log_debugcon(const char16_t *msg) {
}
#endif /* ! __i386__ && ! __x86_64__ */

_noreturn_ static void panic(const char16_t *message) {
        if (ST->ConOut->Mode->CursorColumn > 0)
                ST->ConOut->OutputString(ST->ConOut, (char16_t *) u"\r\n");
        ST->ConOut->SetAttribute(ST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTRED, EFI_BLACK));
        ST->ConOut->OutputString(ST->ConOut, (char16_t *) message);
        log_debugcon(DEBUGCON_PREFIX);
        log_debugcon(message);
        freeze();
}

void efi_assert(const char *expr, const char *file, unsigned line, const char *function) {
        static bool asserting = false;

        /* Let's be paranoid. */
        if (asserting)
                panic(u"systemd-boot: Nested assertion failure, halting.");

        asserting = true;
        log_error("systemd-boot: Assertion '%s' failed at %s:%u@%s, halting.", expr, file, line, function);
        freeze();
}

EFI_STATUS log_internal(EFI_STATUS status, const char *format, ...) {
        assert(format);
        char16_t *msg;

        int32_t attr = ST->ConOut->Mode->Attribute;

        if (ST->ConOut->Mode->CursorColumn > 0)
                ST->ConOut->OutputString(ST->ConOut, (char16_t *) u"\r\n");
        ST->ConOut->SetAttribute(ST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTRED, EFI_BLACK));

        va_list ap;
        va_start(ap, format);
        msg = xvasprintf_status(status, format, ap);
        va_end(ap);

        ST->ConOut->OutputString(ST->ConOut, msg);
        ST->ConOut->OutputString(ST->ConOut, (char16_t *) u"\r\n");
        ST->ConOut->SetAttribute(ST->ConOut, attr);

        log_debugcon(DEBUGCON_PREFIX);
        log_debugcon(msg);
        log_debugcon(u"\r\n");

        msg = mfree(msg);

        log_count++;
        return status;
}

#ifdef EFI_DEBUG
void log_hexdump(const char16_t *prefix, const void *data, size_t size) {
        /* Debugging helper — please keep this around, even if not used */

        _cleanup_free_ char16_t *hex = hexdump(data, size);
        log_internal(EFI_SUCCESS, "%ls[%zu]: %ls", prefix, size, hex);
}
#endif

void log_wait(void) {
        if (log_count == 0)
                return;

        BS->Stall(MIN(4u, log_count) * 2500 * 1000);
        log_count = 0;
}

_used_ intptr_t __stack_chk_guard = (intptr_t) 0x70f6967de78acae3;

/* We can only set a random stack canary if this function attribute is available,
 * otherwise this may create a stack check fail. */
#if STACK_PROTECTOR_RANDOM
void __stack_chk_guard_init(void) {
        EFI_RNG_PROTOCOL *rng;
        if (BS->LocateProtocol(MAKE_GUID_PTR(EFI_RNG_PROTOCOL), NULL, (void **) &rng) == EFI_SUCCESS)
                (void) rng->GetRNG(rng, NULL, sizeof(__stack_chk_guard), (void *) &__stack_chk_guard);
}
#endif

_used_ _noreturn_ void __stack_chk_fail(void);
_used_ _noreturn_ void __stack_chk_fail_local(void);
void __stack_chk_fail(void) {
        panic(u"systemd-boot: Stack check failed, halting.");
}
void __stack_chk_fail_local(void) {
        __stack_chk_fail();
}

/* Called by libgcc for some fatal errors like integer overflow with -ftrapv. */
_used_ _noreturn_ void abort(void);
void abort(void) {
        panic(u"systemd-boot: Unknown error, halting.");
}

#if defined(__ARM_EABI__)
/* These override the (weak) div0 handlers from libgcc as they would otherwise call raise() instead. */
_used_ _noreturn_ int __aeabi_idiv0(int return_value);
_used_ _noreturn_ long long __aeabi_ldiv0(long long return_value);

int __aeabi_idiv0(int return_value) {
        panic(u"systemd-boot: Division by zero, halting.");
}

long long __aeabi_ldiv0(long long return_value) {
        panic(u"systemd-boot: Division by zero, halting.");
}
#endif
