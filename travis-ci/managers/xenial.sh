#!/bin/bash
set -e
set -x

apt-get update
apt-get build-dep systemd -y
apt-get install -y util-linux libmount-dev libblkid-dev liblzma-dev libqrencode-dev libmicrohttpd-dev iptables-dev liblz4-dev libcurl4-gnutls-dev unifont itstool kbd cryptsetup-bin net-tools isc-dhcp-client iputils-ping strace qemu-system-x86 linux-image-virtual mount libgpg-error-dev libxkbcommon-dev python-lxml python3-lxml python3-pip libcap-dev
apt-get install -y gettext python3-evdev python3-pyparsing libmount-dev python3-setuptools ninja-build
pip3 install meson

cd $REPO_ROOT

sed -i 's/2\.30/2.27/' meson.build

meson --werror -Db_sanitize=address,undefined -Dsplit-usr=true build
ninja -v -C build

export ASAN_OPTIONS=strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1
export UBSAN_OPTIONS=print_stacktrace=1:print_summary=1:halt_on_error=1
source "$(dirname $0)/travis_wait.bash"
SYSTEMD_LOG_LEVEL=debug ./build/test-bpf
travis_wait meson test --timeout-multiplier=3 -C build --print-errorlogs

make -C test/TEST-01-BASIC clean setup run TEST_NO_QEMU=yes NSPAWN_ARGUMENTS=--keep-unit RUN_IN_UNPRIVILEGED_CONTAINER=no

# Now that we're more or less sure that ASan isn't going to crash systemd and cause a kernel panic
# let's also run the test with QEMU to cover udevd, sysctl and everything else that isn't run
# in containers.
make -C test/TEST-01-BASIC clean setup run TEST_NO_NSPAWN=yes
