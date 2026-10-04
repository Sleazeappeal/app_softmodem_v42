#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build fuzz_regress.c with AddressSanitizer/UBSan against the patched
# spandsp v42.c and v42bis.c, and run it. The ASan objects override the same
# symbols of the installed libspandsp.
#
# usage: fuzz_regress.sh <spandsp source dir>   (the patched spandsp-0.0.6 tree)
set -eu
T=${1:?usage: $0 <patched spandsp source dir>}
OUT=$(mktemp -d)
HERE=$(cd "$(dirname "$0")" && pwd)
CF="-g -O0 -fsanitize=address,undefined -fsanitize-recover=address -fno-omit-frame-pointer -DHAVE_CONFIG_H -I$T/src -I$T"
gcc $CF -w -c "$T/src/v42.c" -o "$OUT/v42.o"
gcc $CF -w -c "$T/src/v42bis.c" -o "$OUT/v42bis.o"
gcc $CF -c "$HERE/fuzz_regress.c" -o "$OUT/fuzz_regress.o"
gcc -fsanitize=address,undefined -fsanitize-recover=address "$OUT/fuzz_regress.o" "$OUT/v42.o" "$OUT/v42bis.o" -lspandsp -o "$OUT/fuzz_regress"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 "$OUT/fuzz_regress"
