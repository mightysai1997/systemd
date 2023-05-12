#!/bin/bash -eux
# SPDX-License-Identifier: LGPL-2.1-or-later

systemctl --failed --no-legend | tee /failed-services

# Check that secure boot keys were properly enrolled.
if [[ -d /sys/firmware/efi/efivars/ ]]; then
    cmp /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c <(printf '\6\0\0\0\1')
    cmp /sys/firmware/efi/efivars/SetupMode-8be4df61-93ca-11d2-aa0d-00e098032b8c <(printf '\6\0\0\0\0')
    grep -q this_should_be_here /proc/cmdline && exit 1
    grep -q this_should_not_be_here /proc/cmdline && exit 1
fi

# Exit with non-zero EC if the /failed-services file is not empty (we have -e set)
[[ ! -s /failed-services ]]

: >/testok
