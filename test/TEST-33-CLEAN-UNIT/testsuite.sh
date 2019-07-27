#!/bin/bash
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
set -ex
set -o pipefail

cat > /etc/systemd/system/testservice.service <<EOF
[Service]
ConfigurationDirectory=testservice
RuntimeDirectory=testservice
StateDirectory=testservice
CacheDirectory=testservice
LogsDirectory=testservice
RuntimeDirectoryPreserve=yes
ExecStart=/bin/sleep infinity
Type=exec
EOF

systemctl daemon-reload

! test -e /etc/testservice
! test -e /run/testservice
! test -e /var/lib/testservice
! test -e /var/cache/testservice
! test -e /var/log/testservice

systemctl start testservice

test -d /etc/testservice
test -d /run/testservice
test -d /var/lib/testservice
test -d /var/cache/testservice
test -d /var/log/testservice

! systemctl clean testservice

systemctl stop testservice

test -d /etc/testservice
test -d /run/testservice
test -d /var/lib/testservice
test -d /var/cache/testservice
test -d /var/log/testservice

systemctl clean testservice --what=configuration

! test -e /etc/testservice
test -d /run/testservice
test -d /var/lib/testservice
test -d /var/cache/testservice
test -d /var/log/testservice

systemctl clean testservice

! test -e /etc/testservice
! test -e /run/testservice
test -d /var/lib/testservice
! test -e /var/cache/testservice
test -d /var/log/testservice

systemctl clean testservice --what=logs

! test -e /etc/testservice
! test -e /run/testservice
test -d /var/lib/testservice
! test -e /var/cache/testservice
! test -e /var/log/testservice

systemctl clean testservice --what=all

! test -e /etc/testservice
! test -e /run/testservice
! test -e /var/lib/testservice
! test -e /var/cache/testservice
! test -e /var/log/testservice

echo OK > /testok

exit 0
