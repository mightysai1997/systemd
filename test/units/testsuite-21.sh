#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

# Save the end.service state before we start fuzzing, as it might get changed
# on the fly by one of the fuzzers
systemctl list-jobs | grep -F 'end.service' && SHUTDOWN_AT_EXIT=1 || SHUTDOWN_AT_EXIT=0

# shellcheck disable=SC2317
at_exit() {
    set +e
    # We have to call the end.service/poweroff explicitly even if it's specified on
    # the kernel cmdline via systemd.wants=end.service, since dfuzzer calls
    # org.freedesktop.systemd1.Manager.ClearJobs() which drops the service
    # from the queue
    if [[ $SHUTDOWN_AT_EXIT -ne 0 ]] && ! systemctl poweroff; then
        # PID1 is down let's try to save the journal
        journalctl --sync      # journal can be down as well so let's ignore exit codes here
        systemctl -ff poweroff # sync() and reboot(RB_POWER_OFF)
    fi
}

trap at_exit EXIT

systemctl log-level info

# FIXME: systemd-run doesn't play well with daemon-reexec
# See: https://github.com/systemd/systemd/issues/27204
sed -i '/\[org.freedesktop.systemd1\]/aorg.freedesktop.systemd1.Manager:Reexecute FIXME' /etc/dfuzzer.conf

sed -i '/\[org.freedesktop.systemd1\]/aorg.freedesktop.systemd1.Manager:SoftReboot destructive' /etc/dfuzzer.conf
sed -i '/\[org.freedesktop.login1\]/aSleep destructive' /etc/dfuzzer.conf

# TODO
#   * check for possibly newly introduced buses?
BUS_LIST=(
    org.freedesktop.home1
    org.freedesktop.hostname1
    org.freedesktop.import1
    org.freedesktop.locale1
    org.freedesktop.login1
    org.freedesktop.machine1
    org.freedesktop.portable1
    org.freedesktop.resolve1
    org.freedesktop.systemd1
    org.freedesktop.timedate1
)

# systemd-oomd requires PSI
if tail -n +1 /proc/pressure/{cpu,io,memory}; then
    BUS_LIST+=(
        org.freedesktop.oom1
    )
fi

# Some services require specific conditions:
#   - systemd-timesyncd can't run in a container
#   - systemd-networkd can run in a container if it has CAP_NET_ADMIN capability
if ! systemd-detect-virt --container; then
    BUS_LIST+=(
        org.freedesktop.network1
        org.freedesktop.timesync1
    )
elif busctl introspect org.freedesktop.network1 / &>/dev/null; then
    BUS_LIST+=(
        org.freedesktop.network1
    )
fi

SESSION_BUS_LIST=(
    org.freedesktop.systemd1
)

# Maximum payload size generated by dfuzzer (in bytes) - default: 50K
PAYLOAD_MAX=50000
# Tweak the maximum payload size if we're running under sanitizers, since
# with larger payloads we start hitting reply timeouts
if [[ -v ASAN_OPTIONS || -v UBSAN_OPTIONS ]]; then
    PAYLOAD_MAX=10000 # 10K
fi

# Overmount /var/lib/machines with a size-limited tmpfs, as fuzzing
# the org.freedesktop.machine1 stuff makes quite a mess
mount -t tmpfs -o size=50M tmpfs /var/lib/machines

# Fuzz both the system and the session buses (where applicable)
for bus in "${BUS_LIST[@]}"; do
    echo "Bus: $bus (system)"
    systemd-run --pipe --wait \
                -- dfuzzer -b "$PAYLOAD_MAX" -n "$bus"

    # Let's reload the systemd daemon to test (de)serialization as well
    systemctl daemon-reload
    # FIXME: explicitly trigger reexecute until systemd/systemd#27204 is resolved
    systemctl daemon-reexec
done

umount /var/lib/machines

for bus in "${SESSION_BUS_LIST[@]}"; do
    echo "Bus: $bus (session)"
    systemd-run --machine 'testuser@.host' --user --pipe --wait \
                -- dfuzzer -b "$PAYLOAD_MAX" -n "$bus"

    # Let's reload the systemd user daemon to test (de)serialization as well
    systemctl --machine 'testuser@.host' --user daemon-reload
    # FIXME: explicitly trigger reexecute until systemd/systemd#27204 is resolved
    systemctl --machine 'testuser@.host' --user daemon-reexec
done

touch /testok
