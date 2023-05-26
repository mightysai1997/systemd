#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

# shellcheck source=test/units/util.sh
. "$(dirname "$0")"/util.sh

export SYSTEMD_LOG_LEVEL=debug

# Create service with KillMode=none inside a slice
cat <<EOF >/run/systemd/system/test19cleanup.service
[Unit]
Description=Test 19 cleanup Service
[Service]
Slice=test19cleanup.slice
Type=simple
ExecStart=sleep infinity
KillMode=none
EOF
cat <<EOF >/run/systemd/system/test19cleanup.slice
[Unit]
Description=Test 19 cleanup Slice
EOF

# Start service
systemctl start test19cleanup.service
assert_rc 0 systemd-cgls /test19cleanup.slice

# Stop slice
# The sleep process will not be killed because of KillMode=none
# Since there is still a process running under it, the /test19cleanup.slice cgroup won't be removed
systemctl stop test19cleanup.slice

# Kill sleep process manually
while pkill sleep
do
    sleep 0.1
done

assert_rc 1 systemd-cgls /test19cleanup.slice/test19cleanup.service

# Check that empty cgroup /test19cleanup.slice has been removed
timeout 30 bash -c 'while systemd-cgls /test19cleanup.slice >& /dev/null; do sleep .5; done'
assert_rc 1 systemd-cgls /test19cleanup.slice
