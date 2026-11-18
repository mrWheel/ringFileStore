#!/bin/sh
# Host-side test runner for the ring_file_store component.
# Compiles the component sources against the POSIX shims and executes the
# test binary. No hardware or ESP-IDF installation is required.
set -e

cd "$(dirname "$0")"

mkdir -p build/host

cc -std=c11 -g -Wall -Wextra -Werror \
   -I ../include \
   -I ../src \
   -I host/shim \
   host/shim/shim.c \
   ../src/ring_file_store.c \
   host/test_ring_file_store.c \
   -o build/host/test_ring_file_store

# argv[1]: testLog.md destination (repository root).
build/host/test_ring_file_store ../../../testLog.md
