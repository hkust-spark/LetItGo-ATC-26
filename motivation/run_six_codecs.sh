#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" || "$#" -ne 2 ]]; then
  cat <<'EOF'
Usage: bash run_six_codecs.sh H264_BASELINE GENERATED_CODEC_DIR

Run the packet-position SSIM experiment on the H.264 baseline and its five
variants created by code/ssim_loss_experiment/prepare_matched_codecs.sh.
GENERATED_CODEC_DIR must contain <baseline_stem>_{h265,vp8,vp9,av1,h266}
with extensions .mp4, .webm, .webm, .mkv, .mp4 respectively.

Environment: FFMPEG_PREFIX (custom decoder installation), START_FRAME=0,
STOP_FRAME=599, PACKET_SIZE=1500, FRAME_JOBS=4, PACKET_JOBS=6,
OUTPUT_DIR=result/ssim_loss, OVERWRITE=1, DRY_RUN=0, PYTHON_BIN=python3.
DRY_RUN validates input formats and prints the six commands without decoding.
EOF
  [[ "${1:-}" == "--help" ]] && exit 0
  exit 2
fi

experiment_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
baseline="$(realpath "$1")"
generated_dir="$(realpath "$2")"
stem="$(basename "$baseline")"
stem="${stem%.*}"
# Keep the public runner independent of personal or batch launch scripts.
if [[ -n "${FFMPEG_PREFIX:-}" ]]; then
  export FFMPEG_PREFIX="$(realpath -m "$FFMPEG_PREFIX")"
  export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
export FFMPEG_BIN="${FFMPEG_BIN:-${FFMPEG_PREFIX:-/usr/local}/bin/ffmpeg}"
export FFPROBE_BIN="${FFPROBE_BIN:-${FFMPEG_PREFIX:-/usr/local}/bin/ffprobe}"
python_bin="${PYTHON_BIN:-python3}"
for tool_var in FFMPEG_BIN FFPROBE_BIN python_bin; do
  if [[ "${!tool_var}" == */* ]]; then
    printf -v "$tool_var" '%s' "$(realpath -m "${!tool_var}")"
  fi
done
for tool_var in FFMPEG_BIN FFPROBE_BIN; do
  [[ -x "${!tool_var}" ]] || { echo "Required tool not executable: ${!tool_var}" >&2; exit 1; }
done
probe_bin="$FFPROBE_BIN"

inputs=(
  "$baseline"
  "$generated_dir/${stem}_h265.mp4"
  "$generated_dir/${stem}_vp8.webm"
  "$generated_dir/${stem}_vp9.webm"
  "$generated_dir/${stem}_av1.mkv"
  "$generated_dir/${stem}_h266.mp4"
)
expected_codecs=(h264 hevc vp8 vp9 av1 vvc)

for index in "${!inputs[@]}"; do
  input="${inputs[$index]}"
  [[ -f "$input" ]] || { echo "Missing codec input: $input" >&2; exit 1; }
  codec="$("$probe_bin" -v error -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$input")"
  if [[ "$codec" != "${expected_codecs[$index]}" ]]; then
    echo "Expected ${expected_codecs[$index]}, got $codec: $input" >&2
    exit 1
  fi
done

# Relative output paths retain their original interpretation from the module root.
cd "$experiment_root"
for input in "${inputs[@]}"; do
  command=(
    "$python_bin" "$experiment_root/code/ssim_loss_experiment/run_experiment.py"
    --input "$input"
    --start-frame-index "${START_FRAME:-0}"
    --stop-frame-index "${STOP_FRAME:-599}"
    --packet-size "${PACKET_SIZE:-1500}"
    --output-dir "${OUTPUT_DIR:-result/ssim_loss}"
    --frame-jobs "${FRAME_JOBS:-4}"
    --jobs "${PACKET_JOBS:-6}"
  )
  [[ "${OVERWRITE:-1}" != 1 ]] || command+=(--overwrite)
  printf 'Running command:'
  printf ' %q' "${command[@]}"
  printf '\n'
  if [[ "${DRY_RUN:-0}" != 1 ]]; then
    "${command[@]}"
  fi
done
