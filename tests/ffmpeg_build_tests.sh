#!/bin/sh
# Exercise the production build script in a disposable tree, without compiling
# FFmpeg or touching the checkout's configured dependencies and DLLs.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$repo_root/work"
test_root=$(mktemp -d "$repo_root/work/ffmpeg-build-tests.XXXXXX")
cleanup() {
    case "$test_root" in
        "$repo_root"/work/ffmpeg-build-tests.*) rm -rf -- "$test_root" ;;
    esac
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

export TEST_ROOT="$test_root"
export TEST_ZLIB="$test_root/libz.a"
mkdir -p "$test_root/bin" "$test_root/ffmpeg/ffbuild" "$test_root/thirdparty/64"
touch "$TEST_ZLIB"
cat > "$test_root/bin/x86_64-w64-mingw32-gcc" <<'EOF'
#!/bin/sh
printf '%s\n' "$TEST_ZLIB"
EOF
cat > "$test_root/bin/make" <<'EOF'
#!/bin/sh
if [ "$1" = distclean ]; then
    exit 0
fi
touch "$TEST_ROOT/build-called"
exit 23
EOF
cat > "$test_root/ffmpeg/configure" <<'EOF'
#!/bin/sh
touch "$TEST_ROOT/configure-called"
echo 'test configure output'
exit "$TEST_CONFIGURE_STATUS"
EOF
chmod +x "$test_root/bin/"*
export PATH="$test_root/bin:$PATH"

run_build() {
    status=0
    (cd "$test_root" && sh "$repo_root/build_ffmpeg.sh" x64) > "$test_root/log" 2>&1 || status=$?
}
fail() {
    cat "$test_root/log"
    echo "FAIL: $*" >&2
    exit 1
}

export TEST_CONFIGURE_STATUS=37
run_build
[ "$status" -eq 37 ] || fail "configure failure must propagate its exit status (got $status)"
[ -f "$test_root/configure-called" ] || fail 'configure was not invoked'
[ ! -f "$test_root/build-called" ] || fail 'build ran after configure failed'
grep -q 'test configure output' "$test_root/log" || fail 'configure diagnostics were lost'

export TEST_CONFIGURE_STATUS=0
run_build
[ "$status" -ne 0 ] || fail 'make failure must fail the build script'
[ -f "$test_root/build-called" ] || fail 'successful configure did not reach make'
rm -f "$test_root/configure-called" "$test_root/build-called"

export TEST_ZLIB="$test_root/missing-libz.a"
run_build
[ "$status" -ne 0 ] || fail 'missing static zlib must fail the build script'
[ ! -f "$test_root/configure-called" ] || fail 'configure ran without static zlib'
[ ! -f "$test_root/build-called" ] || fail 'build ran without static zlib'

echo 'PASS: configure status and diagnostics, make failure, missing static zlib'
