#!/usr/bin/env bash
set -e
TEST_DESCRIPTION="test setenv"

. $(dirname ${BASH_SOURCE[0]})/../test-functions

do_test "$@" 26
