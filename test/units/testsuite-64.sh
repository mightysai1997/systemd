#!/usr/bin/env bash
# vi: ts=4 sw=4 tw=0 et:

set -eux
set -o pipefail

testcase_megasas2_basic() {
    lsblk -S
    [[ "$(lsblk --scsi --noheadings | wc -l)" -ge 128 ]]
}

testcase_nvme_basic() {
    lsblk --noheadings | grep "^nvme"
    [[ "$(lsblk --noheadings | grep -c "^nvme")" -ge 28 ]]
}

testcase_virtio_scsi_identically_named_partitions() {
    lsblk --noheadings -a -o NAME,PARTLABEL
    [[ "$(lsblk --noheadings -a -o NAME,PARTLABEL | grep -c "Hello world")" -eq $((16 * 8)) ]]
}

testcase_multipath_basic() {
    local i wwid

    # Configure multipath
    mpathconf --enable --user_friendly_names n --with_multipathd y --with_module y --find_multipaths y --property_blacklist n
    systemctl status multipathd.service
    multipath -ll
    ls -l /dev/mapper

    for i in {0..63}; do
        wwid="3deaddeadbeef$(printf "%.4d" "$i")"
        lsblk "/dev/mapper/$wwid"
        multipath -C "$(readlink -f "/dev/mapper/$wwid")"
        # We should have 4 paths for each multipath device
        # Note: multipath -C sends all its output to stderr, hence |&
        [[ "$(multipath -C "$(readlink -f "/dev/mapper/$wwid")" |& grep -c "$wwid")" -eq 4 ]]
    done
}

: >/failed

udevadm settle

lsblk -a

# TEST_FUNCTION_NAME is passed on the kernel command line via systemd.setenv=
# in the respective test.sh file
if ! command -v "${TEST_FUNCTION_NAME:?}"; then
    echo >&2 "Missing verification handler for test case '$TEST_FUNCTION_NAME'"
    exit 1
fi

echo "TEST_FUNCTION_NAME=$TEST_FUNCTION_NAME"
"$TEST_FUNCTION_NAME"

systemctl status systemd-udevd

touch /testok
rm /failed
