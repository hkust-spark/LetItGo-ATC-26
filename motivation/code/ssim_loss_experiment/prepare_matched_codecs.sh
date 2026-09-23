#!/usr/bin/env bash
set -euo pipefail

FFMPEG_PREFIX="${FFMPEG_PREFIX:-}"
FFMPEG_BIN="${FFMPEG_BIN:-}"
FFPROBE_BIN="${FFPROBE_BIN:-}"

usage() {
  cat <<'EOF'
Usage:
  bash code/ssim_loss_experiment/prepare_matched_codecs.sh --input INPUT_VIDEO [options]

Required:
  --input PATH                Source video to encode.

Options:
  --output-dir DIR            Directory for generated codec variants.
                              Default: data/generated_codecs/<input_stem>
  --codec NAME                Generate only one target codec.
                              Choices: h265, vp8, vp9, av1, h266
  --bitrate RATE              Target video bitrate for all codecs.
                              Default: inferred from the input video stream.
  --bufsize RATE              Rate-control buffer size.
                              Default: 2x --bitrate when the bitrate format is parseable.
  --gop N                     GOP size / keyframe interval. Default: 30
  --start-seconds SECONDS     Optional clip start offset.
  --duration-seconds SECONDS  Optional clip duration.
  --force                     Overwrite existing outputs.
  --help                      Show this help message.

Override FFmpeg tools with environment variables:
  FFMPEG_PREFIX=/usr/local
  FFMPEG_BIN=/custom/ffmpeg/bin/ffmpeg
  FFPROBE_BIN=/custom/ffmpeg/bin/ffprobe

What it does:
  - treats the input file as the H.264 baseline
  - re-encodes the same source content to H.265/HEVC, VP8, VP9, and AV1
  - re-encodes to H.266/VVC using libvvenc (required in all-codec mode)
  - forces VP8 to one token partition and VP9/AV1 to one tile
  - removes audio and subtitle streams
  - keeps the source resolution and frame rate
  - targets the same bitrate level as the input by default
  - writes <input_stem>_<codec>_encode_report.txt when --codec is specified
    (otherwise <input_stem>_encode_report.txt) to check realized bitrates

Examples:
  bash code/ssim_loss_experiment/prepare_matched_codecs.sh \
    --input data/source.mp4

  bash code/ssim_loss_experiment/prepare_matched_codecs.sh \
    --input data/source.mp4 \
    --codec av1 \
    --bitrate 4M \
    --start-seconds 10 \
    --duration-seconds 5 \
    --force
EOF
}

require_binary() {
  local bin="$1"
  if [[ ! -x "$bin" ]]; then
    echo "Missing required binary: $bin" >&2
    exit 1
  fi
}

prepend_env_path() {
  local env_name="$1"
  local path_value="$2"
  local current_value="${!env_name:-}"

  [[ -d "$path_value" ]] || return 0
  if [[ -n "$current_value" ]]; then
    export "${env_name}=${path_value}:${current_value}"
  else
    export "${env_name}=${path_value}"
  fi
}

configure_ffmpeg_tools() {
  if [[ -n "$FFMPEG_PREFIX" ]]; then
    FFMPEG_PREFIX="${FFMPEG_PREFIX%/}"
    FFMPEG_BIN="${FFMPEG_BIN:-${FFMPEG_PREFIX}/bin/ffmpeg}"
    FFPROBE_BIN="${FFPROBE_BIN:-${FFMPEG_PREFIX}/bin/ffprobe}"

    prepend_env_path PKG_CONFIG_PATH "${FFMPEG_PREFIX}/lib/pkgconfig"
    prepend_env_path PKG_CONFIG_PATH "${FFMPEG_PREFIX}/lib64/pkgconfig"
    prepend_env_path LD_LIBRARY_PATH "${FFMPEG_PREFIX}/lib"
    prepend_env_path LD_LIBRARY_PATH "${FFMPEG_PREFIX}/lib64"

    rpath_flags=()
    [[ -d "${FFMPEG_PREFIX}/lib" ]] && rpath_flags+=("-Wl,-rpath,${FFMPEG_PREFIX}/lib")
    [[ -d "${FFMPEG_PREFIX}/lib64" ]] && rpath_flags+=("-Wl,-rpath,${FFMPEG_PREFIX}/lib64")
    if [[ "${#rpath_flags[@]}" -gt 0 ]]; then
      export LDFLAGS="${rpath_flags[*]}${LDFLAGS:+ ${LDFLAGS}}"
    fi
  fi

  FFMPEG_BIN="${FFMPEG_BIN:-/usr/local/bin/ffmpeg}"
  FFPROBE_BIN="${FFPROBE_BIN:-/usr/local/bin/ffprobe}"
  export FFMPEG_PREFIX FFMPEG_BIN FFPROBE_BIN
}

ffmpeg_has_encoder() {
  local encoder_name="$1"
  awk -v encoder_name="$encoder_name" '$2 == encoder_name { found = 1 } END { exit(found ? 0 : 1) }' <<< "$FFMPEG_ENCODERS"
}

select_av1_encoder() {
  if ffmpeg_has_encoder "libsvtav1"; then
    echo "libsvtav1"
    return 0
  fi
  if ffmpeg_has_encoder "libaom-av1"; then
    echo "libaom-av1"
    return 0
  fi
  return 1
}

select_h266_encoder() {
  if ffmpeg_has_encoder "libvvenc"; then
    echo "libvvenc"
    return 0
  fi
  return 1
}

double_rate() {
  local rate="$1"
  if [[ "$rate" =~ ^([0-9]+)([kKmM])?$ ]]; then
    local number="${BASH_REMATCH[1]}"
    local suffix="${BASH_REMATCH[2]}"
    echo "$(( number * 2 ))${suffix}"
    return 0
  fi
  return 1
}

probe_input_video_bitrate() {
  local input_path="$1"
  local stream_bitrate
  local format_bitrate

  stream_bitrate="$("$FFPROBE_BIN" -v error -select_streams v:0 -show_entries stream=bit_rate -of default=noprint_wrappers=1:nokey=1 "$input_path" | head -n 1 | tr -d '\r')"
  if [[ "$stream_bitrate" =~ ^[0-9]+$ ]] && [[ "$stream_bitrate" -gt 0 ]]; then
    echo "$stream_bitrate"
    return 0
  fi

  format_bitrate="$("$FFPROBE_BIN" -v error -show_entries format=bit_rate -of default=noprint_wrappers=1:nokey=1 "$input_path" | head -n 1 | tr -d '\r')"
  if [[ "$format_bitrate" =~ ^[0-9]+$ ]] && [[ "$format_bitrate" -gt 0 ]]; then
    echo "$format_bitrate"
    return 0
  fi

  return 1
}

run_pass() {
  local pass_number="$1"
  shift
  "$FFMPEG_BIN" -y "${COMMON_INPUT_ARGS[@]}" "${COMMON_STREAM_ARGS[@]}" "$@" -pass "$pass_number" -passlogfile "$PASSLOGFILE" -f null /dev/null
}

encode_two_pass() {
  local codec_name="$1"
  local output_path="$2"
  shift 2
  local codec_args=("$@")

  echo "Encoding ${codec_name} -> ${output_path}"
  rm -f "${PASSLOGFILE}"* "$output_path"

  run_pass 1 "${codec_args[@]}"
  "$FFMPEG_BIN" -y "${COMMON_INPUT_ARGS[@]}" "${COMMON_STREAM_ARGS[@]}" "${codec_args[@]}" -pass 2 -passlogfile "$PASSLOGFILE" "$output_path"

  rm -f "${PASSLOGFILE}"*
}

encode_single_pass() {
  local codec_name="$1"
  local output_path="$2"
  shift 2
  local codec_args=("$@")

  echo "Encoding ${codec_name} -> ${output_path}"
  rm -f "$output_path"
  "$FFMPEG_BIN" -y "${COMMON_INPUT_ARGS[@]}" "${COMMON_STREAM_ARGS[@]}" "${codec_args[@]}" "$output_path"
}

encode_libx265_two_pass() {
  local output_path="$1"

  echo "Encoding h265 -> ${output_path}"
  rm -f "$X265_STATS" "$output_path"

  "$FFMPEG_BIN" -y "${COMMON_INPUT_ARGS[@]}" "${COMMON_STREAM_ARGS[@]}" "${COMMON_8BIT_RC_ARGS[@]}" \
    -c:v libx265 \
    -preset medium \
    -tag:v hvc1 \
    -x265-params "pass=1:stats=${X265_STATS}:keyint=${GOP}:min-keyint=${GOP}:scenecut=0:bframes=0:open-gop=0" \
    -f null /dev/null

  "$FFMPEG_BIN" -y "${COMMON_INPUT_ARGS[@]}" "${COMMON_STREAM_ARGS[@]}" "${COMMON_8BIT_RC_ARGS[@]}" \
    -c:v libx265 \
    -preset medium \
    -tag:v hvc1 \
    -x265-params "pass=2:stats=${X265_STATS}:keyint=${GOP}:min-keyint=${GOP}:scenecut=0:bframes=0:open-gop=0" \
    "$output_path"

  rm -f "$X265_STATS"
}

append_probe_report() {
  local label="$1"
  local path="$2"
  {
    echo "== ${label} =="
    echo "path=${path}"
    "$FFPROBE_BIN" -v error \
      -select_streams v:0 \
      -show_entries stream=codec_name,bit_rate,width,height,avg_frame_rate,pix_fmt \
      -show_entries format=bit_rate,duration,size \
      -of default=noprint_wrappers=1:nokey=0 \
      "$path"
    echo
  } >> "$REPORT_PATH"
}

INPUT=""
OUTPUT_DIR=""
CODEC=""
BITRATE=""
BUFSIZE=""
GOP="30"
START_SECONDS=""
DURATION_SECONDS=""
FORCE="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input)
      INPUT="${2:-}"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="${2:-}"
      shift 2
      ;;
    --codec)
      CODEC="${2:-}"
      shift 2
      ;;
    --bitrate)
      BITRATE="${2:-}"
      shift 2
      ;;
    --bufsize)
      BUFSIZE="${2:-}"
      shift 2
      ;;
    --gop)
      GOP="${2:-}"
      shift 2
      ;;
    --start-seconds)
      START_SECONDS="${2:-}"
      shift 2
      ;;
    --duration-seconds)
      DURATION_SECONDS="${2:-}"
      shift 2
      ;;
    --force)
      FORCE="1"
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$INPUT" ]]; then
  echo "--input is required" >&2
  usage >&2
  exit 1
fi

configure_ffmpeg_tools
require_binary "$FFMPEG_BIN"
require_binary "$FFPROBE_BIN"
FFMPEG_ENCODERS="$("$FFMPEG_BIN" -hide_banner -encoders 2>/dev/null || true)"

if [[ ! -f "$INPUT" ]]; then
  echo "Input file does not exist: $INPUT" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

INPUT_ABS="$(realpath "$INPUT")"
INPUT_BASENAME="$(basename "$INPUT_ABS")"
INPUT_STEM="${INPUT_BASENAME%.*}"

if [[ -z "$OUTPUT_DIR" ]]; then
  OUTPUT_DIR="${REPO_ROOT}/data/generated_codecs/${INPUT_STEM}"
fi

if [[ -z "$BITRATE" ]]; then
  if BITRATE="$(probe_input_video_bitrate "$INPUT_ABS")"; then
    :
  else
    echo "Unable to infer input bitrate from ${INPUT_ABS}. Please pass --bitrate explicitly." >&2
    exit 1
  fi
fi

if [[ -z "$BUFSIZE" ]]; then
  if BUFSIZE="$(double_rate "$BITRATE")"; then
    :
  else
    echo "Unable to derive bufsize from bitrate '${BITRATE}'. Please pass --bufsize explicitly." >&2
    exit 1
  fi
fi

if [[ ! "$GOP" =~ ^[0-9]+$ ]] || [[ "$GOP" -le 0 ]]; then
  echo "--gop must be a positive integer" >&2
  exit 1
fi

if [[ -n "$CODEC" ]]; then
  case "$CODEC" in
    h265|vp8|vp9|av1|h266)
      ;;
    *)
      echo "--codec must be one of: h265, vp8, vp9, av1, h266" >&2
      exit 1
      ;;
  esac
fi

AV1_ENCODER="$(select_av1_encoder || true)"
H266_ENCODER="$(select_h266_encoder || true)"

mkdir -p "$OUTPUT_DIR"
if [[ "$FORCE" != "1" ]]; then
  OUTPUT_PATHS_TO_CHECK=()
  if [[ -z "$CODEC" || "$CODEC" == "h265" ]]; then
    OUTPUT_PATHS_TO_CHECK+=("${OUTPUT_DIR}/${INPUT_STEM}_h265.mp4")
  fi
  if [[ -z "$CODEC" || "$CODEC" == "vp8" ]]; then
    OUTPUT_PATHS_TO_CHECK+=("${OUTPUT_DIR}/${INPUT_STEM}_vp8.webm")
  fi
  if [[ -z "$CODEC" || "$CODEC" == "vp9" ]]; then
    OUTPUT_PATHS_TO_CHECK+=("${OUTPUT_DIR}/${INPUT_STEM}_vp9.webm")
  fi
  if [[ -n "$AV1_ENCODER" ]] && [[ -z "$CODEC" || "$CODEC" == "av1" ]]; then
    OUTPUT_PATHS_TO_CHECK+=("${OUTPUT_DIR}/${INPUT_STEM}_av1.mkv")
  fi
  if [[ -n "$H266_ENCODER" ]] && [[ -z "$CODEC" || "$CODEC" == "h266" ]]; then
    OUTPUT_PATHS_TO_CHECK+=("${OUTPUT_DIR}/${INPUT_STEM}_h266.mp4")
  fi
  for path in "${OUTPUT_PATHS_TO_CHECK[@]}"; do
    if [[ -e "$path" ]]; then
      echo "Refusing to overwrite existing file without --force: $path" >&2
      exit 1
    fi
  done
fi

COMMON_INPUT_ARGS=()
if [[ -n "$START_SECONDS" ]]; then
  COMMON_INPUT_ARGS+=(-ss "$START_SECONDS")
fi
COMMON_INPUT_ARGS+=(-i "$INPUT_ABS")
if [[ -n "$DURATION_SECONDS" ]]; then
  COMMON_INPUT_ARGS+=(-t "$DURATION_SECONDS")
fi

COMMON_STREAM_ARGS=(
  -an
  -sn
  -map 0:v:0
)

COMMON_8BIT_RC_ARGS=(
  -pix_fmt yuv420p
  -b:v "$BITRATE"
  -maxrate "$BITRATE"
  -bufsize "$BUFSIZE"
  -g "$GOP"
  -keyint_min "$GOP"
)

COMMON_8BIT_VBR_ARGS=(
  -pix_fmt yuv420p
  -b:v "$BITRATE"
  -g "$GOP"
  -keyint_min "$GOP"
)

COMMON_10BIT_VBR_ARGS=(
  -pix_fmt yuv420p10le
  -b:v "$BITRATE"
  -g "$GOP"
  -keyint_min "$GOP"
)

if [[ -n "$CODEC" ]]; then
  REPORT_PATH="${OUTPUT_DIR}/${INPUT_STEM}_${CODEC}_encode_report.txt"
else
  REPORT_PATH="${OUTPUT_DIR}/${INPUT_STEM}_encode_report.txt"
fi
PASSLOGFILE="${OUTPUT_DIR}/ffmpeg2pass-${INPUT_STEM}"
X265_STATS="${OUTPUT_DIR}/x265-2pass-${INPUT_STEM}.log"
rm -f "$REPORT_PATH" "${PASSLOGFILE}"* "$X265_STATS"

H265_OUTPUT="${OUTPUT_DIR}/${INPUT_STEM}_h265.mp4"
VP8_OUTPUT="${OUTPUT_DIR}/${INPUT_STEM}_vp8.webm"
VP9_OUTPUT="${OUTPUT_DIR}/${INPUT_STEM}_vp9.webm"
AV1_OUTPUT="${OUTPUT_DIR}/${INPUT_STEM}_av1.mkv"
H266_OUTPUT="${OUTPUT_DIR}/${INPUT_STEM}_h266.mp4"

if [[ -z "$CODEC" || "$CODEC" == "h265" ]]; then
  encode_libx265_two_pass "$H265_OUTPUT"
fi

if [[ -z "$CODEC" || "$CODEC" == "vp8" ]]; then
  encode_two_pass "vp8" "$VP8_OUTPUT" \
    "${COMMON_8BIT_RC_ARGS[@]}" \
    -c:v libvpx \
    -deadline good \
    -cpu-used 2 \
    -threads 8 \
    -slices 1 \
    -auto-alt-ref 0 \
    -lag-in-frames 0
fi

if [[ -z "$CODEC" || "$CODEC" == "vp9" ]]; then
  encode_two_pass "vp9" "$VP9_OUTPUT" \
    "${COMMON_8BIT_RC_ARGS[@]}" \
    -c:v libvpx-vp9 \
    -deadline good \
    -cpu-used 2 \
    -row-mt 1 \
    -tile-columns 0 \
    -tile-rows 0 \
    -threads 8
fi

if [[ -z "$CODEC" || "$CODEC" == "av1" ]]; then
  if [[ -n "$AV1_ENCODER" ]]; then
    if [[ "$AV1_ENCODER" == "libsvtav1" ]]; then
      encode_single_pass "av1 (${AV1_ENCODER})" "$AV1_OUTPUT" \
        "${COMMON_8BIT_VBR_ARGS[@]}" \
        -c:v libsvtav1 \
        -preset 8 \
        -svtav1-params tile_rows=0:tile_columns=0
    else
      encode_two_pass "av1 (${AV1_ENCODER})" "$AV1_OUTPUT" \
        "${COMMON_8BIT_RC_ARGS[@]}" \
        -c:v libaom-av1 \
        -cpu-used 4 \
        -row-mt 1 \
        -tiles 1x1 \
        -tile-columns 0 \
        -tile-rows 0 \
        -usage good \
        -tune ssim
    fi
  else
    echo "Requested codec av1, but no supported FFmpeg encoder was found (expected libsvtav1 or libaom-av1)." >&2
    exit 1
  fi
elif [[ -z "$CODEC" ]] && [[ -z "$AV1_ENCODER" ]]; then
  echo "Skipping av1: no supported FFmpeg encoder found (expected libsvtav1 or libaom-av1)." >&2
fi

if [[ -z "$CODEC" || "$CODEC" == "h266" ]]; then
  if [[ -n "$H266_ENCODER" ]]; then
    encode_single_pass "h266 (${H266_ENCODER})" "$H266_OUTPUT" \
      "${COMMON_10BIT_VBR_ARGS[@]}" \
      -c:v libvvenc \
      -preset medium
  else
    echo "Requested codec h266, but no supported FFmpeg encoder was found (expected libvvenc)." >&2
    exit 1
  fi
elif [[ -z "$CODEC" ]] && [[ -z "$H266_ENCODER" ]]; then
  echo "Skipping h266: no supported FFmpeg encoder found (expected libvvenc)." >&2
fi

{
  echo "input=${INPUT_ABS}"
  echo "source_bitrate=${BITRATE}"
  echo "target_bitrate=${BITRATE}"
  echo "bufsize=${BUFSIZE}"
  echo "gop=${GOP}"
  echo "codec=${CODEC:-all}"
  echo "start_seconds=${START_SECONDS}"
  echo "duration_seconds=${DURATION_SECONDS}"
  echo "av1_encoder=${AV1_ENCODER:-unavailable}"
  echo "h266_encoder=${H266_ENCODER:-unavailable}"
  echo
} > "$REPORT_PATH"

append_probe_report "input_h264_baseline" "$INPUT_ABS"
if [[ -z "$CODEC" || "$CODEC" == "h265" ]]; then
  append_probe_report "h265" "$H265_OUTPUT"
fi
if [[ -z "$CODEC" || "$CODEC" == "vp8" ]]; then
  append_probe_report "vp8" "$VP8_OUTPUT"
fi
if [[ -z "$CODEC" || "$CODEC" == "vp9" ]]; then
  append_probe_report "vp9" "$VP9_OUTPUT"
fi
if [[ -n "$AV1_ENCODER" ]] && [[ -z "$CODEC" || "$CODEC" == "av1" ]]; then
  append_probe_report "av1 (${AV1_ENCODER})" "$AV1_OUTPUT"
fi
if [[ -n "$H266_ENCODER" ]] && [[ -z "$CODEC" || "$CODEC" == "h266" ]]; then
  append_probe_report "h266 (${H266_ENCODER})" "$H266_OUTPUT"
fi

echo "Generated files:"
if [[ -z "$CODEC" || "$CODEC" == "h265" ]]; then
  echo "  $H265_OUTPUT"
fi
if [[ -z "$CODEC" || "$CODEC" == "vp8" ]]; then
  echo "  $VP8_OUTPUT"
fi
if [[ -z "$CODEC" || "$CODEC" == "vp9" ]]; then
  echo "  $VP9_OUTPUT"
fi
if [[ -n "$AV1_ENCODER" ]] && [[ -z "$CODEC" || "$CODEC" == "av1" ]]; then
  echo "  $AV1_OUTPUT"
fi
if [[ -n "$H266_ENCODER" ]] && [[ -z "$CODEC" || "$CODEC" == "h266" ]]; then
  echo "  $H266_OUTPUT"
fi
echo "Probe report:"
echo "  $REPORT_PATH"
