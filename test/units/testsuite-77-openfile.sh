#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

# shellcheck source=test/units/assert.sh
. "$(dirname "$0")"/assert.sh

export SYSTEMD_LOG_LEVEL=debug

assert_eq "$LISTEN_FDS" "1"
assert_eq "$LISTEN_FDNAMES" "socket"
read -u 3 text
assert_eq "$text" "Socket"
