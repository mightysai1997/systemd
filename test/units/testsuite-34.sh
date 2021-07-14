#!/usr/bin/env bash
set -eux
set -o pipefail

systemd-analyze log-level debug
systemd-analyze log-target console

# Set everything up without DynamicUser=1

systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz touch /var/lib/zzz/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz test -f /var/lib/zzz/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz -p TemporaryFileSystem=/var/lib test -f /var/lib/zzz/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:yyy test -f /var/lib/yyy/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:xxx -p TemporaryFileSystem=/var/lib test -f /var/lib/xxx/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:xxx -p TemporaryFileSystem=/var/lib:ro test -f /var/lib/xxx/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz test -f /var/lib/zzz/test-missing \
    && { echo 'unexpected success'; exit 1; }

test -d /var/lib/zzz
test ! -L /var/lib/zzz
test ! -e /var/lib/private/zzz
test -f /var/lib/zzz/test
test ! -f /var/lib/zzz/test-missing

# Convert to DynamicUser=1

systemd-run --wait -p DynamicUser=1 -p StateDirectory=zzz test -f /var/lib/zzz/test
systemd-run --wait -p DynamicUser=1 -p StateDirectory=zzz -p TemporaryFileSystem=/var/lib test -f /var/lib/zzz/test
systemd-run --wait -p DynamicUser=1 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:yyy test -f /var/lib/yyy/test
systemd-run --wait -p DynamicUser=1 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:xxx -p TemporaryFileSystem=/var/lib test -f /var/lib/xxx/test
systemd-run --wait -p DynamicUser=1 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:xxx -p TemporaryFileSystem=/var/lib:ro test -f /var/lib/xxx/test
systemd-run --wait -p DynamicUser=1 -p StateDirectory=zzz test -f /var/lib/zzz/test-missing \
    && { echo 'unexpected success'; exit 1; }

test -L /var/lib/zzz
test -L /var/lib/yyy
test -d /var/lib/private/zzz
test ! -L /var/lib/private/xxx
test ! -L /var/lib/xxx

test -f /var/lib/zzz/test
test ! -f /var/lib/zzz/test-missing

# Convert back

systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz test -f /var/lib/zzz/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz -p TemporaryFileSystem=/var/lib test -f /var/lib/zzz/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:yyy test -f /var/lib/yyy/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:xxx -p TemporaryFileSystem=/var/lib test -f /var/lib/xxx/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz -p StateDirectorySymlink=zzz:xxx -p TemporaryFileSystem=/var/lib:ro test -f /var/lib/xxx/test
systemd-run --wait -p DynamicUser=0 -p StateDirectory=zzz test -f /var/lib/zzz/test-missing \
    && { echo 'unexpected success'; exit 1; }

test -d /var/lib/zzz
test ! -L /var/lib/xxx
test ! -L /var/lib/private/xxx
test -L /var/lib/yyy
test ! -L /var/lib/zzz
test ! -e /var/lib/private/zzz
test -f /var/lib/zzz/test
test ! -f /var/lib/zzz/test-missing

systemd-analyze log-level info

echo OK >/testok

exit 0
