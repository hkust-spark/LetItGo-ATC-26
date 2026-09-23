#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash scripts/run_motivation.sh --input H264_BASELINE [options]
       bash scripts/run_motivation.sh --stage summarize [--output-dir DIR]

Stages:
  --stage all|prepare|run|summarize   Default: all.
    prepare    Encode the five codec variants of the supplied H.264 baseline.
    run        Run all six existing codec inputs.
    summarize  Export numeric CSV summaries from existing per-frame results.
    all        Perform prepare, run, then summarize.

Options:
  --input PATH          H.264 baseline, preferably prepared without B frames.
  --generated-dir DIR   Codec variant directory; default: module/data/generated_codecs/<stem>.
  --output-dir DIR      Experiment result root; default: module/result/ssim_loss.
  --start-frame N       First frame, inclusive. Default: 0.
  --stop-frame N        Last frame, inclusive. Default: 599.
  --frame-jobs N        Concurrent frames. Default: 4.
  --packet-jobs N       Concurrent packet batches per frame. Default: 6.
  --packet-size N       Synthetic packet size in bytes. Default: 1500.
  --bitrate RATE        Target bitrate for generated variants, e.g. 4M.
  --force               Replace existing encodes and per-frame experiment outputs.
  --dry-run             Print commands only; no probing, encoding or file writes.
  --help                Show this help.

Environment:
  FFMPEG_PREFIX         Custom decoder installation; default: module/dependencies/install.
  FFMPEG_BIN, FFPROBE_BIN   Optional tool overrides.
  PYTHON_BIN            Python interpreter. Default: python3.

Explicit relative paths are resolved from the caller's current directory.
No figures are generated. Existing per-frame outputs are kept unless --force
is set. Use --stage run to reuse already prepared codec variants.
EOF
}

fail() { echo "$*" >&2; exit 2; }
need_value() { [[ "$#" -ge 2 && -n "$2" ]] || fail "Missing value for $1"; }

module_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage=all
input=""
generated_dir=""
output_dir="$module_root/result/ssim_loss"
start_frame=0
stop_frame=599
frame_jobs=4
packet_jobs=6
packet_size=1500
bitrate=""
force=0
dry_run=0

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --stage) need_value "$@"; stage="$2"; shift 2 ;;
    --input) need_value "$@"; input="$2"; shift 2 ;;
    --generated-dir) need_value "$@"; generated_dir="$2"; shift 2 ;;
    --output-dir) need_value "$@"; output_dir="$2"; shift 2 ;;
    --start-frame) need_value "$@"; start_frame="$2"; shift 2 ;;
    --stop-frame) need_value "$@"; stop_frame="$2"; shift 2 ;;
    --frame-jobs) need_value "$@"; frame_jobs="$2"; shift 2 ;;
    --packet-jobs) need_value "$@"; packet_jobs="$2"; shift 2 ;;
    --packet-size) need_value "$@"; packet_size="$2"; shift 2 ;;
    --bitrate) need_value "$@"; bitrate="$2"; shift 2 ;;
    --force) force=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) fail "Unknown argument: $1 (use --help)" ;;
  esac
done

case "$stage" in all|prepare|run|summarize) ;; *) fail "Invalid stage: $stage" ;; esac
for option in start_frame stop_frame frame_jobs packet_jobs packet_size; do
  value="${!option}"
  [[ "$value" =~ ^[0-9]+$ ]] || fail "$option must be a non-negative integer"
  printf -v "$option" '%d' "$((10#$value))"
done
(( stop_frame >= start_frame )) || fail "--stop-frame must be at least --start-frame"
(( frame_jobs > 0 && packet_jobs > 0 && packet_size > 0 )) || fail "Job counts and packet size must be positive"

if [[ "$stage" != summarize ]]; then
  [[ -n "$input" ]] || fail "--input is required for stage $stage"
  input="$(realpath -m "$input")"
  if [[ "$dry_run" == 0 && ! -f "$input" ]]; then
    fail "Input file not found: $input"
  fi
  stem="$(basename "$input")"
  stem="${stem%.*}"
  generated_dir="${generated_dir:-$module_root/data/generated_codecs/$stem}"
  generated_dir="$(realpath -m "$generated_dir")"
fi
output_dir="$(realpath -m "$output_dir")"

export FFMPEG_PREFIX="$(realpath -m "${FFMPEG_PREFIX:-$module_root/dependencies/install}")"
export FFMPEG_BIN="${FFMPEG_BIN:-$FFMPEG_PREFIX/bin/ffmpeg}"
export FFPROBE_BIN="${FFPROBE_BIN:-$FFMPEG_PREFIX/bin/ffprobe}"
export PYTHON_BIN="${PYTHON_BIN:-python3}"
for tool_var in FFMPEG_BIN FFPROBE_BIN PYTHON_BIN; do
  if [[ "${!tool_var}" == */* ]]; then
    printf -v "$tool_var" '%s' "$(realpath -m "${!tool_var}")"
  fi
done
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

run_command() {
  printf 'Command:'
  printf ' %q' "$@"
  printf '\n'
  if [[ "$dry_run" == 0 ]]; then
    "$@"
  fi
}

echo "Stage: $stage"
echo "FFmpeg prefix: $FFMPEG_PREFIX"
echo "Result root: $output_dir"

if [[ "$stage" == all || "$stage" == prepare ]]; then
  if [[ "$dry_run" == 0 ]]; then
    codec="$("$FFPROBE_BIN" -v error -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$input")"
    [[ "$codec" == h264 ]] || fail "Expected H.264 baseline, got $codec. Prepare it with remove_b_frames.sh first."
  fi
  command=(bash "$module_root/code/ssim_loss_experiment/prepare_matched_codecs.sh"
    --input "$input" --output-dir "$generated_dir")
  [[ -z "$bitrate" ]] || command+=(--bitrate "$bitrate")
  [[ "$force" == 0 ]] || command+=(--force)
  run_command "${command[@]}"
fi

if [[ "$stage" == all || "$stage" == run ]]; then
  run_command env START_FRAME="$start_frame" STOP_FRAME="$stop_frame" \
    FRAME_JOBS="$frame_jobs" PACKET_JOBS="$packet_jobs" PACKET_SIZE="$packet_size" \
    OUTPUT_DIR="$output_dir" OVERWRITE="$force" DRY_RUN=0 \
    bash "$module_root/run_six_codecs.sh" "$input" "$generated_dir"
fi

if [[ "$stage" == all || "$stage" == summarize ]]; then
  run_command "$PYTHON_BIN" "$module_root/summarize_results.py" --result-root "$output_dir"
fi
