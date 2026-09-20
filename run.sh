#!/usr/bin/env bash
# run.sh — one command to compile and play Mini Space Shooter (Linux / Ubuntu).
#
#   ./run.sh              build, then launch the game in a window
#   ./run.sh run          same thing
#   ./run.sh test         build and run the headless self-tests
#   ./run.sh shots        render every screen to out/shots/*.png (no window)
#   ./run.sh strict       rebuild with -Werror
#   ./run.sh sanitize     build and test with memory/undefined-behavior checks
#   ./run.sh window-test  test native window input, scaling and close events
#   ./run.sh audio-test   test real playback, music/mute controls and shutdown
#   ./run.sh clean        remove build artefacts
#
# Extra arguments after the mode are passed to the game binary, e.g.
#   ./run.sh run --frames 600     run 600 frames then exit (smoke test)
#   ./run.sh run --scale 2        force a 2x pixel scale window

set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

BIN=build/mini-space-shooter
TESTBIN=build/selftest
JOBS="$(nproc 2>/dev/null || echo 2)"

# ---------------------------------------------------------------- pretty logs
if [ -t 1 ]; then
  B=$'\033[1m'
  D=$'\033[2m'
  G=$'\033[32m'
  Y=$'\033[33m'
  R=$'\033[31m'
  N=$'\033[0m'
else
  B=
  D=
  G=
  Y=
  R=
  N=
fi
say() { printf '%s==>%s %s\n' "$B" "$N" "$*"; }
warn() { printf '%s[!]%s %s\n' "$Y" "$N" "$*" >&2; }
die() {
  printf '%s[x]%s %s\n' "$R" "$N" "$*" >&2
  exit 1
}

# ------------------------------------------------------------ sanity: tooling
check_toolchain() {
  local missing=()
  command -v make >/dev/null || missing+=(make)
  command -v cc >/dev/null || missing+=(gcc)
  command -v c++ >/dev/null || missing+=(g++)
  if [ ${#missing[@]} -gt 0 ]; then
    die "missing build tools: ${missing[*]}
    sudo apt install build-essential"
  fi

  # libX11 / libXext headers + link stubs are the only build dependencies.
  if ! echo '#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
int main(void){return 0;}' | cc -x c - -o /dev/null -lX11 -lXext 2>/dev/null; then
    die "X11 development files not found.
    sudo apt install libx11-dev libxext-dev"
  fi
}

# ------------------------------------------------------------------ build step
build() {
  local target="${1:-all}"
  say "compiling with $(c++ --version | head -1 | cut -d' ' -f1) $(c++ -dumpversion 2>/dev/null || true)  (-j$JOBS)"
  # Show compiler diagnostics, hide the usual make noise unless something breaks.
  if ! make -j"$JOBS" "$target" >build/.make.log 2>&1; then
    cat build/.make.log >&2
    die "build failed (full log: build/.make.log)"
  fi
  grep -E 'warning:' build/.make.log && warn "compiler warnings above" || true
  printf '%s    ok%s %s\n' "$D" "$N" "${1:+}"
}

mkdir -p build

mode="${1:-run}"
[ $# -gt 0 ] && shift || true

case "$mode" in
run)
  check_toolchain
  build all
  [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || warn "no DISPLAY/WAYLAND_DISPLAY set — the game needs a graphical session"
  say "launching Mini Space Shooter"
  exec "./$BIN" "$@"
  ;;

test)
  check_toolchain
  build all
  say "running self-tests"
  exec "./$TESTBIN" "$@"
  ;;

shots)
  check_toolchain
  build all
  rm -rf out/shots && mkdir -p out/shots
  say "rendering screens offscreen (no window opened)"
  "./$BIN" --shot out/shots "$@"
  if command -v magick >/dev/null; then conv=magick; elif command -v convert >/dev/null; then
    conv=convert
  else conv=; fi
  if [ -n "$conv" ]; then
    for f in out/shots/*.ppm; do [ -e "$f" ] && "$conv" "$f" "${f%.ppm}.png"; done
    rm -f out/shots/*.ppm
    say "wrote PNGs:"
    ls -1 out/shots
  else
    say "wrote PPMs (install imagemagick for PNG):"
    ls -1 out/shots
  fi
  ;;

strict)
  check_toolchain
  say "rebuilding with -Werror"
  make -j"$JOBS" strict
  ;;

sanitize | window-test | audio-test)
  check_toolchain
  make -j"$JOBS" "$mode"
  ;;

clean)
  make clean
  say "cleaned"
  ;;

help | -h | --help)
  sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
  ;;

*)
  # Allow `./run.sh --frames 300` style direct game flags.
  check_toolchain
  build all
  say "launching Mini Space Shooter $*"
  exec "./$BIN" "$mode" "$@"
  ;;
esac
