#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

journalctl --rotate --vacuum-files=1
# Nuke all archived journals, so we start with a clean slate
rm -f /var/log/journal/$(</etc/machine-id)/system@*.journal
rm -f /var/log/journal/$(</etc/machine-id)/user-*@*.journal
journalctl --header | grep path

# Make sure the user instance is active when we rotate journals
systemd-run --unit user-sleep.service --user -M testuser@ sleep infinity

for i in {0..10}; do
    journalctl --rotate
    journalctl --sync
    SYSTEMD_LOG_LEVEL=debug journalctl -n1 -q
    (! journalctl -n0 -q |& grep corrupted)
done

systemctl stop --user -M testuser@ user-sleep.service
