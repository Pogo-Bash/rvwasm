#!/bin/bash
# The /proc/cpuinfo rv makes, for lists of online processors with gaps
# (tests/cpuinfo/cpuinfo.c, a native program built with rv/identity.c).
# Usage: cpuinfo-check.sh [BUILD-DIR]
set -eu
S="$(cd "$(dirname "$0")/.." && pwd)"
O="${1:-$S/build}"
mkdir -p "$O"
cc -O1 -Wall -pthread -I"$S/rv" -o "$O/cpuinfo-unit" "$S/tests/cpuinfo/cpuinfo.c" "$S/rv/identity.c" "$S/rv/bitmanip.c"
"$O/cpuinfo-unit"
