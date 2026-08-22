#!/bin/sh
# Build the elephc-PHP middleware + the ePHPm ABI adapter shim.
#
# Produces:
#   libmw_php.so     — elephc-compiled PHP (exports mw_invoke, needs mw_set_header/mw_log)
#   libelephc_mw.so  — the adapter ePHPm actually loads (exports ephpm_middleware_*)
#   test_host        — standalone harness that mimics ePHPm's loader
set -eu

ELEPHC="${ELEPHC:-elephc}"
cd "$(dirname "$0")"

echo "==> compiling PHP -> cdylib with elephc"
"$ELEPHC" --emit cdylib src/middleware.php
# elephc writes the artifact next to the source file.
mv -f src/libmiddleware.so libmw_php.so

echo "==> building adapter shim"
gcc -shared -fPIC -o libelephc_mw.so shim/ephpm_elephc_shim.c \
    -L. -lmw_php -Wl,-rpath,'$ORIGIN'

echo "==> building test host"
gcc -o test_host shim/test_host.c -ldl

echo "==> exports of libelephc_mw.so (what ePHPm dlsym()s):"
nm -D --defined-only libelephc_mw.so | grep ' T '

echo "==> done"
