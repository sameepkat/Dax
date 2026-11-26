#!/bin/sh
# Find libraries needed by Janet on this host; some systems include these in libc.
set -eu
tmp=$(mktemp -d "${TMPDIR:-/tmp}/dax-link.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
libs=
probe() {
    code=$1
    lib=$2
    printf '%s\n' "$code" > "$tmp/test.c"
    if $CC $CFLAGS "$tmp/test.c" $LDFLAGS $libs -o "$tmp/test" >/dev/null 2>&1; then
        return
    fi
    if $CC $CFLAGS "$tmp/test.c" $LDFLAGS $libs "$lib" -o "$tmp/test" >/dev/null 2>&1; then
        libs="$libs $lib"
    else
        echo "Could not link required system function with $lib" >&2
        exit 1
    fi
}
probe '#include <math.h>
int main(int argc, char **argv) { (void)argv; volatile double x = argc; return sin(x) > 2; }' -lm
probe '#include <pthread.h>
int main(void) { pthread_t t = pthread_self(); return pthread_equal(t, t) == 0; }' -lpthread
probe '#include <time.h>
int main(void) { struct timespec t; return clock_gettime(CLOCK_REALTIME, &t); }' -lrt
probe '#include <dlfcn.h>
int main(void) { return dlopen(0, RTLD_LAZY) == 0; }' -ldl
printf '%s\n' "$libs"
