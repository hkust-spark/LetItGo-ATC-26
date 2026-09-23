#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash dependencies/build_dependencies.sh [--dry-run] [--help]

Build and install the patched AOM and FFmpeg sources, then rebuild the native
motivation-experiment helper. Run dependencies/prepare_sources.sh first.
This script works from any directory; it does not fetch sources or install
system packages.

Options:
  --dry-run  Print commands without building or creating directories. Prepared
             sources and installed build dependencies are not required.
  --help     Show this help.

Environment:
  FFMPEG_PREFIX  Installation directory (default: dependencies/install next to
                 this script). Relative values are resolved against your cwd.
  JOBS           Positive build concurrency (default: nproc).
  ENABLE_SVTAV1  1 enables FFmpeg's optional libsvtav1 encoder; default: 0.

Existing PKG_CONFIG_PATH and LD_LIBRARY_PATH entries are preserved after the
installation prefix paths. Expose an external VVenC installation through
PKG_CONFIG_PATH before running this script.

Required build tools and development libraries are listed in dependencies/README.md.
EOF
}

dry_run=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) dry_run=1 ;;
    --help|-h) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
  esac
done

dependency_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
experiment_dir="$(cd -- "$dependency_dir/.." && pwd)"
FFMPEG_PREFIX="${FFMPEG_PREFIX:-$dependency_dir/install}"
if [[ "$FFMPEG_PREFIX" != /* ]]; then
  FFMPEG_PREFIX="$PWD/$FFMPEG_PREFIX"
fi
JOBS="${JOBS:-$(nproc)}"
ENABLE_SVTAV1="${ENABLE_SVTAV1:-0}"

if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  printf 'JOBS must be a positive integer; got: %s\n' "$JOBS" >&2
  exit 2
fi
if [[ "$ENABLE_SVTAV1" != 0 && "$ENABLE_SVTAV1" != 1 ]]; then
  printf 'ENABLE_SVTAV1 must be 0 or 1; got: %s\n' "$ENABLE_SVTAV1" >&2
  exit 2
fi

export FFMPEG_PREFIX
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

print_command() {
  printf '+ '
  printf '%q ' "$@"
  printf '\n'
}

run() {
  print_command "$@"
  if (( ! dry_run )); then
    "$@"
  fi
}

run_in() {
  local directory="$1"
  shift
  printf '+ (cd -- %q && ' "$directory"
  printf '%q ' "$@"
  printf ')\n'
  if (( ! dry_run )); then
    (cd -- "$directory" && "$@")
  fi
}

if (( ! dry_run )); then
  for tool in cmake make pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      printf 'Required build tool not found: %s\n' "$tool" >&2
      exit 1
    fi
  done
  if [[ ! -f "$dependency_dir/src/aom/CMakeLists.txt" || ! -f "$dependency_dir/src/ffmpeg/configure" ]]; then
    printf 'Prepared sources are missing. Run: bash %q\n' "$dependency_dir/prepare_sources.sh" >&2
    exit 1
  fi
fi

print_command export "FFMPEG_PREFIX=$FFMPEG_PREFIX"
print_command export "PKG_CONFIG_PATH=$PKG_CONFIG_PATH"
print_command export "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"

run cmake -S "$dependency_dir/src/aom" -B "$dependency_dir/build/aom" \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_INSTALL_PREFIX=$FFMPEG_PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_SHARED_LIBS=ON -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
run cmake --build "$dependency_dir/build/aom" -j "$JOBS"
run cmake --install "$dependency_dir/build/aom"

ffmpeg_options=(
  "--prefix=$FFMPEG_PREFIX"
  --enable-shared --disable-static --enable-gpl
  --enable-libaom --enable-libx264 --enable-libx265
  --enable-libvpx --enable-libvvenc
  "--extra-cflags=-I$FFMPEG_PREFIX/include"
  "--extra-ldflags=-L$FFMPEG_PREFIX/lib -Wl,-rpath,$FFMPEG_PREFIX/lib"
)
if [[ "$ENABLE_SVTAV1" == 1 ]]; then
  ffmpeg_options+=(--enable-libsvtav1)
fi
run_in "$dependency_dir/src/ffmpeg" ./configure "${ffmpeg_options[@]}"
run_in "$dependency_dir/src/ffmpeg" make -j "$JOBS"
run_in "$dependency_dir/src/ffmpeg" make install

run make -C "$experiment_dir/code/native" clean
run make -C "$experiment_dir/code/native"

if (( dry_run )); then
  printf 'Dry run complete; no build commands were executed.\n'
else
  printf 'Dependencies installed in %s; native helper rebuilt.\n' "$FFMPEG_PREFIX"
fi
