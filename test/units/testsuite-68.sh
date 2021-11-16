#!/usr/bin/env bash
set -eux
set -o pipefail

if ! command -v systemd-sysupdate &>/dev/null; then
    echo "no systemd-sysupdate" >/skipped
    exit 0
fi

export SYSTEMD_PAGER=cat
export SYSTEMD_LOG_LEVEL=debug

rm -f /var/tmp/68-joined.raw
truncate -s 10M /var/tmp/68-joined.raw

sfdisk /var/tmp/68-joined.raw <<EOF
label: gpt
unit: sectors
sector-size: 512

size=2048, type=4f68bce3-e8cd-4db1-96e7-fbcaf984b709, name=_empty
size=2048, type=4f68bce3-e8cd-4db1-96e7-fbcaf984b709, name=_empty
size=2048, type=2c7357ed-ebd2-46d9-aec1-23d437ec2bf5, name=_empty
size=2048, type=2c7357ed-ebd2-46d9-aec1-23d437ec2bf5, name=_empty
EOF

rm -rf /var/tmp/68-dirs

rm -rf /var/tmp/68-defs
mkdir -p /var/tmp/68-defs

cat > /var/tmp/68-defs/01-first.conf <<EOF
[Source]
Type=regular-file
Path=/var/tmp/68-source
MatchPattern=part1-\$v.raw

[Target]
Type=partition
Path=/var/tmp/68-joined.raw
MatchPattern=part1-\$v
MatchPartitionType=root-x86-64
EOF

cat > /var/tmp/68-defs/02-second.conf <<EOF
[Source]
Type=regular-file
Path=/var/tmp/68-source
MatchPattern=part2-\$v.raw.gz

[Target]
Type=partition
Path=/var/tmp/68-joined.raw
MatchPattern=part2-\$v
MatchPartitionType=root-x86-64-verity
EOF

cat > /var/tmp/68-defs/03-third.conf <<EOF
[Source]
Type=directory
Path=/var/tmp/68-source
MatchPattern=dir-\$v

[Target]
Type=directory
Path=/var/tmp/68-dirs
CurrentSymlink=/var/tmp/68-dirs/current
MatchPattern=dir-\$v
InstancesMax=3
EOF

rm -rf /var/tmp/68-source
mkdir -p /var/tmp/68-source

new_version() {
    # Create a pair of random partition payloads, and compress one
    dd if=/dev/urandom of=/var/tmp/68-source/part1-$1.raw bs=1024 count=1024
    dd if=/dev/urandom of=/var/tmp/68-source/part2-$1.raw bs=1024 count=1024
    gzip -k -f /var/tmp/68-source/part2-$1.raw

    mkdir -p /var/tmp/68-source/dir-$1
    echo $RANDOM > /var/tmp/68-source/dir-$1/foo.txt
    echo $RANDOM > /var/tmp/68-source/dir-$1/bar.txt

    tar --numeric-owner -C /var/tmp/68-source/dir-$1/ -czf /var/tmp/68-source/dir-$1.tar.gz .

    ( cd  /var/tmp/68-source/ && sha256sum part* dir-*.tar.gz > SHA256SUMS )
}

update_now() {
    # Update to newest version. First there should be an update ready, then we
    # do the update, and then there should not be any ready anymore

    systemd-sysupdate --definitions=/var/tmp/68-defs --verify=no test
    systemd-sysupdate --definitions=/var/tmp/68-defs --verify=no now
    ! systemd-sysupdate --definitions=/var/tmp/68-defs --verify=no test
}

verify_version() {
    # Expects: version ID + sector offset of both partitions to compare
    dd if=/var/tmp/68-joined.raw bs=1024 skip=$2 count=1024 | cmp /var/tmp/68-source/part1-$1.raw
    dd if=/var/tmp/68-joined.raw bs=1024 skip=$3 count=1024 | cmp /var/tmp/68-source/part2-$1.raw
    cmp /var/tmp/68-source/dir-$1/foo.txt /var/tmp/68-dirs/current/foo.txt
    cmp /var/tmp/68-source/dir-$1/bar.txt /var/tmp/68-dirs/current/bar.txt
}

# Install initial version and verify
new_version v1
update_now
verify_version v1 1024 3072

# Create second version, update and verify that it is added
new_version v2
update_now
verify_version v2 2048 4096

# Create third version, update and verify it replaced the first version
new_version v3
update_now
verify_version v3 1024 3072

# Create fourth version, and update through a file:// URL. This should be
# almost as good as testing HTTP, but is simpler for us to set up. file:// is
# abstracted in curl for us, and since our main goal is to test our own code
# (and not curl) this test should be quite good even if not comprehensive. This
# will test the SHA256SUMS logic at least (we turn off GPG validation though,
# see above)
new_version v4

cat > /var/tmp/68-defs/02-second.conf <<EOF
[Source]
Type=http-file
Path=file:///var/tmp/68-source
MatchPattern=part2-\$v.raw.gz

[Target]
Type=partition
Path=/var/tmp/68-joined.raw
MatchPattern=part2-\$v
MatchPartitionType=root-x86-64-verity
EOF

cat > /var/tmp/68-defs/03-third.conf <<EOF
[Source]
Type=http-tar
Path=file:///var/tmp/68-source
MatchPattern=dir-\$v.tar.gz

[Target]
Type=directory
Path=/var/tmp/68-dirs
CurrentSymlink=/var/tmp/68-dirs/current
MatchPattern=dir-\$v
InstancesMax=3
EOF

update_now
verify_version v4 2048 4096

rm  /var/tmp/68-joined.raw
rm -r /var/tmp/68-dirs /var/tmp/68-defs /var/tmp/68-source

echo OK >/testok

exit 0
