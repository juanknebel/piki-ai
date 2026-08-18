#!/bin/sh
# Static release builds for Linux (musl + static OpenSSL).
#
# Usage: tools/build-release.sh [x86_64] [i686]     (default: both)
#
# Requires in deps/ (downloaded only once):
#   openssl-$OPENSSL_VER.tar.gz     https://www.openssl.org/source/
#   i686-linux-musl-cross.tgz       https://musl.cc/  (only for i686)
# and the system musl-gcc (musl package) for x86_64.
#
# Output: dist/piki-linux-<arch>, 100% static binaries.
set -eu

cd "$(dirname "$0")/.."
ROOT=$PWD
DEPS=$ROOT/deps
DIST=$ROOT/dist
OPENSSL_VER=3.5.1
JOBS=$(nproc 2>/dev/null || echo 2)

SRCS="src/main.c src/buf.c src/net.c src/http.c src/json.c src/sse.c \
      src/api.c src/chat.c src/config.c src/term.c src/edit.c src/tools.c \
      src/md.c"

build_openssl() { # $1 arch  $2 CC  $3 Configure target
    PREFIX=$DEPS/openssl-$1
    [ -f "$PREFIX/lib/libssl.a" ] && return 0
    echo "== static OpenSSL $OPENSSL_VER for $1 =="
    SRC=$DEPS/openssl-src-$1
    rm -rf "$SRC"
    mkdir -p "$SRC"
    tar -xzf "$DEPS/openssl-$OPENSSL_VER.tar.gz" -C "$SRC" \
        --strip-components=1
    cd "$SRC"
    # no-ktls and OPENSSL_NO_SECURE_MEMORY: avoid kernel headers that
    # the system musl-gcc does not ship; neither is needed in a client.
    ./Configure "$3" no-shared no-dso no-module no-engine no-comp \
        no-zlib no-tests no-apps no-docs no-legacy no-ssl3 no-dtls \
        no-quic no-ktls -DOPENSSL_NO_SECURE_MEMORY \
        --prefix="$PREFIX" --libdir=lib CC="$2" >/dev/null
    make -j"$JOBS" build_libs >/dev/null
    make install_dev >/dev/null 2>&1
    cd "$ROOT"
    rm -rf "$SRC"
}

build_piki() { # $1 arch  $2 CC  $3 strip
    PREFIX=$DEPS/openssl-$1
    mkdir -p "$DIST"
    echo "== static piki for $1 =="
    # shellcheck disable=SC2086
    "$2" -static -std=c99 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L \
        -I"$PREFIX/include" $SRCS \
        "$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a" \
        -o "$DIST/piki-linux-$1"
    "$3" "$DIST/piki-linux-$1"
    ls -l "$DIST/piki-linux-$1"
}

do_x86_64() {
    command -v musl-gcc >/dev/null || {
        echo "musl-gcc missing (musl package)" >&2; exit 1; }
    build_openssl x86_64 musl-gcc linux-x86_64
    build_piki x86_64 musl-gcc strip
}

do_i686() {
    TC=$DEPS/i686-linux-musl-cross
    if [ ! -x "$TC/bin/i686-linux-musl-gcc" ]; then
        echo "== extracting i686-linux-musl toolchain =="
        tar -xzf "$DEPS/i686-linux-musl-cross.tgz" -C "$DEPS"
    fi
    build_openssl i686 "$TC/bin/i686-linux-musl-gcc" linux-x86
    build_piki i686 "$TC/bin/i686-linux-musl-gcc" \
        "$TC/bin/i686-linux-musl-strip"
}

if [ $# -eq 0 ]; then
    set -- x86_64 i686
fi
for t; do
    case $t in
    x86_64) do_x86_64 ;;
    i686)   do_i686 ;;
    *) echo "unknown target: $t (x86_64|i686)" >&2; exit 1 ;;
    esac
done
