/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <dlfcn.h>
#include <stdlib.h>

#include "bpf-dlopen.h"
#include "cryptsetup-util.h"
#include "elf-util.h"
#include "idn-util.h"
#include "libfido2-util.h"
#include "macro.h"
#include "main-func.h"
#include "pcre2-dlopen.h"
#include "pwquality-util.h"
#include "qrcode-util.h"
#include "tests.h"
#include "tpm2-util.h"

static int run(int argc, char **argv) {
        test_setup_logging(LOG_DEBUG);

        /* Try to load each of our weak library dependencies once. This is supposed to help finding cases
         * where .so versions change and distributions update, but systemd doesn't have the new so names
         * around yet. */

#if HAVE_LIBIDN2 || HAVE_LIBIDN
        assert_se(dlopen_idn() >= 0);
#endif

#if HAVE_LIBCRYPTSETUP
        assert_se(dlopen_cryptsetup() >= 0);
#endif

#if HAVE_PWQUALITY
        assert_se(dlopen_pwquality() >= 0);
#endif

#if HAVE_QRENCODE
        assert_se(dlopen_qrencode() >= 0);
#endif

#if HAVE_TPM2
        assert_se(dlopen_tpm2() >= 0);
#endif

#if HAVE_LIBFIDO2
        assert_se(dlopen_libfido2() >= 0);
#endif

#if HAVE_LIBBPF
        assert_se(dlopen_bpf() >= 0);
#endif

#if HAVE_ELFUTILS
        assert_se(dlopen_dw() >= 0);
        assert_se(dlopen_elf() >= 0);
#endif

#if HAVE_PCRE2
        assert_se(dlopen_pcre2() >= 0);
#endif

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
