#!/usr/bin/env bash
# Runs the Lua scripting self-tests through a real melonDS executable.
#
# Usage: run-tests.sh path/to/melonDS[.exe] [path/to/rom.nds]
#
# On Windows run it from an MSYS2 shell (UCRT64 for the documented build) so
# the Qt and SDL libraries are on PATH.
#
# Each launch uses a private copy of the executable with a "portable" folder
# beside it, so melonDS reads a throwaway config from there and never touches
# the user's real one. That config sets the volume to zero and the microphone
# to silence; SDL gets the dummy audio driver and Qt the offscreen platform,
# so no window appears and nothing is heard. The one exception is the exit
# test on Windows: closing melonDS politely needs a real window, so that
# launch shows a window for a second or two.
#
# Without a ROM only the no-ROM tests run. With a ROM, the ROM tests run twice,
# once with the JIT and once with the interpreter. Every melonDS process is
# killed as soon as its results are in, or after a timeout.

set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 path/to/melonDS [rom.nds]" >&2
    exit 2
fi

EXE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
ROM=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/melonds-lua-tests.XXXXXX")
SUMMARY="$WORK/summary.txt"
: > "$SUMMARY"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;;
    *) WINDOWS=0 ;;
esac

if [ -n "$ROM" ]; then
    ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
    if [ "$WINDOWS" = 1 ]; then ROM=$(cygpath -m "$ROM"); fi
fi

note() { echo "$1" | tee -a "$SUMMARY"; }

# winpath PATH: the path as Windows programs want it, unchanged elsewhere
winpath() {
    if [ "$WINDOWS" = 1 ]; then cygpath -m "$1"; else echo "$1"; fi
}

# setup NAME JIT: creates $WORK/NAME with a private executable and config
setup() {
    local dir="$WORK/$1"
    mkdir -p "$dir/portable"
    cp "$EXE" "$dir/"
    cat > "$dir/portable/melonDS.toml" <<EOF
LimitFPS = false
AudioSync = false

[Emu]
DirectBoot = true

[JIT]
Enable = $2

[Mic]
InputType = 0

[Instance0]

[Instance0.Audio]
Volume = 0
EOF
    : > "$dir/results.txt"
    echo "$dir"
}

# wait_for FILE PATTERN SECONDS: waits until a line matching PATTERN appears
wait_for() {
    local i
    for ((i = 0; i < $3 * 10; i++)); do
        if grep -q "$2" "$1" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

# start DIR PLATFORM ARGS...: starts melonDS in DIR, sets PID
start() {
    local dir=$1 platform=$2
    shift 2
    (
        cd "$dir" || exit 1
        export QT_QPA_PLATFORM=$platform
        export SDL_AUDIODRIVER=dummy
        export LUA_TEST_RESULTS=$(winpath "$dir/results.txt")
        export LUA_TEST_DIR=$(winpath "$dir")
        export LUA_TEST_ROM=$ROM
        export LUA_TEST_JIT=$JIT
        exec "./$(basename "$EXE")" "$@" > "$dir/stdout.txt" 2>&1
    ) &
    PID=$!
}

stop_now() {
    kill -9 "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
}

collect() {
    grep -E '^(PASS|FAIL|INFO)' "$1/results.txt" | sed "s/^/[$2] /" >> "$SUMMARY"
}

# ---- launch 1: no ROM ----

JIT=false
DIR=$(setup core false)
printf 'from the harness\n' > "$DIR/préparé_файл.txt"
start "$DIR" offscreen --lua "$HERE/test_core.lua" --lua "$HERE/test_error.lua"
if wait_for "$DIR/results.txt" '^DONE core' 30; then
    wait_for "$DIR/stdout.txt" 'deliberate test error' 5
else
    note "FAIL core: timed out waiting for test_core.lua"
fi
stop_now
collect "$DIR" core

if [ -f "$DIR/renommé_✓.txt" ]; then
    note "PASS core: the UTF-8 file name is correct on disk"
else
    note "FAIL core: the UTF-8 file name is correct on disk -- $(ls "$DIR")"
fi

if grep -q 'error\] .*deliberate test error' "$DIR/stdout.txt" \
    && grep -q 'stack traceback:' "$DIR/stdout.txt" \
    && grep -q "innerFailure" "$DIR/stdout.txt" \
    && grep -q "outerCaller" "$DIR/stdout.txt"; then
    note "PASS error: uncaught error reported with message and traceback"
else
    note "FAIL error: uncaught error reported with message and traceback"
fi
if grep -q 'FAIL error' "$DIR/results.txt"; then
    note "FAIL error: execution continued past the error"
fi

# ---- launch 2: exit handling ----
# Needs a real window on Windows, since closing politely means WM_CLOSE.

# LUA_TESTS_SKIP_EXIT=1 skips this launch, to avoid its window while iterating
if [ -z "${LUA_TESTS_SKIP_EXIT:-}" ]; then
    DIR=$(setup exit false)
    if [ "$WINDOWS" = 1 ]; then PLATFORM=windows; else PLATFORM=offscreen; fi
    start "$DIR" $PLATFORM --lua "$HERE/test_exit.lua"
    if wait_for "$DIR/results.txt" 'READY' 30; then
        sleep 0.5
        if [ "$WINDOWS" = 1 ]; then
            taskkill //PID "$(cat /proc/$PID/winpid)" > /dev/null 2>&1
        else
            kill -TERM "$PID"
        fi
        exited=0
        for ((i = 0; i < 100; i++)); do
            if ! kill -0 "$PID" 2>/dev/null; then exited=1; break; fi
            sleep 0.1
        done
        if [ $exited = 1 ]; then
            note "PASS exit: melonDS exits on its own with a runaway script and a stop handler that never returns"
        else
            note "FAIL exit: melonDS did not exit within 10 s of being asked to close"
        fi
    else
        note "FAIL exit: timed out waiting for test_exit.lua"
    fi
    stop_now
    collect "$DIR" exit
    if ! grep -q '^DONE exit' "$DIR/results.txt"; then
        note "FAIL exit: the stop handler did not finish"
    fi
fi

# ---- launches 3 and 4: with a ROM, JIT on and off ----

if [ -n "$ROM" ]; then
    for JIT in true false; do
        DIR=$(setup "rom-jit-$JIT" $JIT)
        start "$DIR" offscreen --lua "$HERE/test_rom.lua" --lua "$HERE/test_cberror.lua" --lua "$HERE/test_nohook.lua" "$ROM"
        if ! wait_for "$DIR/results.txt" '^DONE rom' 120; then
            note "FAIL rom (jit $JIT): timed out waiting for test_rom.lua"
        fi
        stop_now
        collect "$DIR" "rom jit=$JIT"
        if ! grep -q '^DONE nohook' "$DIR/results.txt"; then
            note "FAIL nohook (jit $JIT): test_nohook.lua did not finish"
        fi

        if grep -q "error\] .*test_cberror.lua:15: attempt to index a nil value" "$DIR/stdout.txt" \
            && grep -q "stack traceback:" "$DIR/stdout.txt"; then
            note "PASS cberror (jit $JIT): callback error reported with message and traceback"
        else
            note "FAIL cberror (jit $JIT): callback error reported with message and traceback"
        fi
    done
fi

echo
echo "Passed: $(grep -c 'PASS' "$SUMMARY"), failed: $(grep -c 'FAIL' "$SUMMARY")"
grep 'FAIL' "$SUMMARY"
grep 'INFO' "$SUMMARY"
echo "Full results in $WORK"

if grep -q 'FAIL' "$SUMMARY"; then exit 1; fi
exit 0
