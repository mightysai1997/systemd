#!/usr/bin/env bash
set -e
TEST_DESCRIPTION="test OnClockChange= + OnTimezoneChange="
TEST_NO_NSPAWN=1
. $(dirname ${BASH_SOURCE[0]})/../test-functions

do_test "$@" 30
