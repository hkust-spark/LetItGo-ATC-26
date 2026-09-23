#!/usr/bin/env bash
set -euo pipefail

dependencies_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_root="${1:-$dependencies_dir/src}"
if [[ "${1:-}" == "--help" ]]; then
  echo "Usage: bash dependencies/prepare_sources.sh [OUTPUT_DIRECTORY]"
  echo "Export pinned public FFmpeg/AOM source snapshots and apply the bundled patches."
  exit 0
fi

mkdir -p "$source_root"
source_root="$(realpath "$source_root")"
for name in ffmpeg aom; do
  if [[ -e "$source_root/$name" ]]; then
    echo "Source directory already exists: $source_root/$name" >&2
    exit 1
  fi
done

checkout_tmp="$(mktemp -d)"
trap 'rm -rf "$checkout_tmp"' EXIT

export_snapshot() {
  local name="$1" url="$2" revision="$3" patch_file="$4"
  git init --quiet "$checkout_tmp/$name"
  git -C "$checkout_tmp/$name" fetch --quiet --depth=1 "$url" "$revision"
  git -C "$checkout_tmp/$name" checkout --quiet --detach FETCH_HEAD
  git -C "$checkout_tmp/$name" apply --check "$patch_file"
  git -C "$checkout_tmp/$name" apply "$patch_file"
  mkdir "$source_root/$name"
  tar -C "$checkout_tmp/$name" --exclude='./.git' -cf - . | tar -x -C "$source_root/$name"
}

export_snapshot ffmpeg https://github.com/FFmpeg/FFmpeg.git \
  239f2c733de417201d7ad3b3b8b0d9b63285b2b1 "$dependencies_dir/ffmpeg-ec.patch"
export_snapshot aom https://aomedia.googlesource.com/aom \
  44121a2955e80dd72acf18f75b95b886afa23da6 "$dependencies_dir/aom-ec.patch"

echo "Patched sources exported without Git history: $source_root"
