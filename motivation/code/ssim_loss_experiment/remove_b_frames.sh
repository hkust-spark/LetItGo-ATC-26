#!/usr/bin/env bash
set -euo pipefail

FFMPEG_BIN="${FFMPEG_BIN:-${FFMPEG_PREFIX:-/usr/local}/bin/ffmpeg}"
FFPROBE_BIN="${FFPROBE_BIN:-${FFMPEG_PREFIX:-/usr/local}/bin/ffprobe}"

usage() {
  cat <<'EOF'
Usage:
  bash code/ssim_loss_experiment/remove_b_frames.sh --input INPUT_VIDEO [options]
  bash code/ssim_loss_experiment/remove_b_frames.sh --all-data [options]

Input selection:
  --input PATH       Input video. Can be repeated.
  --list PATH        Text file with one input video per line. Blank lines and # comments are ignored.
  --glob PATTERN     Shell glob for input videos, for example 'data/*.mp4'.
  --all-data         Shortcut for --glob 'data/*.mp4'.

Output options:
  --output PATH      Output path for a single input.
  --output-dir DIR   Output directory for multiple inputs. Default: data/no_b_frames
  --suffix TEXT      Output filename suffix before extension. Default: _no_bframes

Encoding options:
  --crf N            libx264 CRF quality. Lower is larger/better. Default: 11
  --preset NAME      libx264 preset. Default: medium
  --force            Overwrite existing outputs.
  --dry-run          Print commands without running FFmpeg.
  --help             Show this help message.

What it does:
  - re-encodes video with libx264 and B-frame generation disabled
  - preserves displayed frames instead of dropping B-coded source frames
  - writes MP4 outputs and verifies that no output B-frames remain
  - removes audio/subtitle/data streams

Examples:
  bash code/ssim_loss_experiment/remove_b_frames.sh --input data/Animation_1080P-3d67.mp4

  bash code/ssim_loss_experiment/remove_b_frames.sh --all-data --output-dir data/no_b_frames --force

  bash code/ssim_loss_experiment/remove_b_frames.sh \
    --glob 'data/*.mp4' \
    --output-dir data/no_b_frames \
    --crf 20 \
    --preset fast \
    --dry-run
EOF
}

require_binary() {
  local bin="$1"
  if [[ ! -x "$bin" ]]; then
    echo "Missing required binary: $bin" >&2
    exit 1
  fi
}

append_list_inputs() {
  local list_path="$1"
  local input_path

  if [[ ! -f "$list_path" ]]; then
    echo "Input list file not found: $list_path" >&2
    exit 1
  fi

  while IFS= read -r input_path || [[ -n "$input_path" ]]; do
    [[ -z "$input_path" || "$input_path" == \#* ]] && continue
    INPUTS+=("$input_path")
  done < "$list_path"
}

append_glob_inputs() {
  local input_glob="$1"
  local matched_inputs=()

  mapfile -t matched_inputs < <(compgen -G "$input_glob" | sort)
  if [[ "${#matched_inputs[@]}" -eq 0 ]]; then
    echo "No files matched glob: $input_glob" >&2
    exit 1
  fi

  INPUTS+=("${matched_inputs[@]}")
}

probe_picture_types() {
  local video_path="$1"

  "$FFPROBE_BIN" \
    -v error \
    -select_streams v:0 \
    -show_entries frame=pict_type \
    -of csv=p=0 \
    "$video_path" |
    awk -F',' '
      $1 != "" {
        counts[$1] += 1
        total += 1
      }
      END {
        other = total - counts["I"] - counts["P"] - counts["B"]
        printf("I=%d P=%d B=%d other=%d total=%d", counts["I"], counts["P"], counts["B"], other, total)
        exit(counts["B"] > 0 ? 1 : 0)
      }
    '
}

output_path_for_input() {
  local input_path="$1"
  local filename stem

  filename="$(basename "$input_path")"
  stem="${filename%.*}"
  printf '%s/%s%s.mp4\n' "$OUTPUT_DIR" "$stem" "$SUFFIX"
}

run_encode() {
  local input_path="$1"
  local output_path="$2"
  local source_summary
  local output_summary
  local overwrite_arg="-n"
  local command=()

  if [[ ! -f "$input_path" ]]; then
    echo "Input video not found: $input_path" >&2
    exit 1
  fi

  if [[ "$FORCE" == "1" ]]; then
    overwrite_arg="-y"
  elif [[ -e "$output_path" ]]; then
    echo "Skipping existing output: $output_path" >&2
    echo "Use --force to overwrite it." >&2
    return 0
  fi

  mkdir -p "$(dirname "$output_path")"
  source_summary="$(probe_picture_types "$input_path" || true)"

  command=(
    "$FFMPEG_BIN"
    -hide_banner
    "$overwrite_arg"
    -i "$input_path"
    -map 0:v:0
    -an
    -sn
    -dn
    -c:v libx264
    -preset "$PRESET"
    -crf "$CRF"
    -bf 0
    -x264-params "bframes=0:b-adapt=0"
    -pix_fmt yuv420p
    -movflags +faststart
    "$output_path"
  )

  echo "Input:  $input_path"
  echo "Output: $output_path"
  echo "Source frame types: ${source_summary:-unavailable}"
  printf 'Command:'
  printf ' %q' "${command[@]}"
  printf '\n'

  if [[ "$DRY_RUN" == "1" ]]; then
    echo
    return 0
  fi

  "${command[@]}"

  if output_summary="$(probe_picture_types "$output_path")"; then
    echo "Output frame types: $output_summary"
  else
    output_summary="$(probe_picture_types "$output_path" || true)"
    echo "Output still contains B-frames: $output_summary" >&2
    exit 1
  fi
  echo
}

INPUTS=()
OUTPUT_PATH=""
OUTPUT_DIR="data/no_b_frames"
SUFFIX="_no_bframes"
CRF="11"
PRESET="medium"
FORCE="0"
DRY_RUN="0"

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --input)
      INPUTS+=("${2:?--input requires a path}")
      shift 2
      ;;
    --list)
      append_list_inputs "${2:?--list requires a path}"
      shift 2
      ;;
    --glob)
      append_glob_inputs "${2:?--glob requires a pattern}"
      shift 2
      ;;
    --all-data)
      append_glob_inputs "data/*.mp4"
      shift
      ;;
    --output)
      OUTPUT_PATH="${2:?--output requires a path}"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="${2:?--output-dir requires a path}"
      shift 2
      ;;
    --suffix)
      SUFFIX="${2:?--suffix requires text}"
      shift 2
      ;;
    --crf)
      CRF="${2:?--crf requires a value}"
      shift 2
      ;;
    --preset)
      PRESET="${2:?--preset requires a value}"
      shift 2
      ;;
    --force)
      FORCE="1"
      shift
      ;;
    --dry-run)
      DRY_RUN="1"
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      while [[ "$#" -gt 0 ]]; do
        INPUTS+=("$1")
        shift
      done
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      INPUTS+=("$1")
      shift
      ;;
  esac
done

require_binary "$FFMPEG_BIN"
require_binary "$FFPROBE_BIN"

if [[ "${#INPUTS[@]}" -eq 0 ]]; then
  echo "No input videos selected." >&2
  usage >&2
  exit 1
fi

if [[ -n "$OUTPUT_PATH" && "${#INPUTS[@]}" -ne 1 ]]; then
  echo "--output can only be used with exactly one input." >&2
  exit 1
fi

for input_path in "${INPUTS[@]}"; do
  if [[ -n "$OUTPUT_PATH" ]]; then
    run_encode "$input_path" "$OUTPUT_PATH"
  else
    run_encode "$input_path" "$(output_path_for_input "$input_path")"
  fi
done
