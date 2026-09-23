# Motivation: packet importance versus position across six codecs

[English](README.md) | [简体中文](README.zh-CN.md)

This module contains the offline experiment used to examine how packet
position within an encoded frame relates to the quality loss caused by a
single corrupted packet. The six codecs are **H.264, H.265/HEVC, VP8, VP9,
AV1, and H.266/VVC**.

It produces numeric experiment outputs. Plotting scripts and automatic figure
generation are excluded. It runs as a standalone offline experiment and does
not need network traces.

## Experiment videos: YouTube-UGC

The motivation experiment uses videos from **YouTube-UGC**, covering its
**15 categories**, including HDR and VR. The dataset and category definitions
are described in the [dataset paper](https://arxiv.org/abs/1904.06457).

| Category | Filename prefix |
| --- | --- |
| Animation | `Animation` |
| Cover Song | `CoverSong` |
| Gaming | `Gaming` |
| HDR | `HDR` |
| How To | `HowTo` |
| Lecture | `Lecture` |
| Live Music | `LiveMusic` |
| Lyric Video | `LyricVideo` |
| Music Video | `MusicVideo` |
| News Clip | `NewsClip` |
| Sports | `Sports` |
| Television Clip | `TelevisionClip` |
| Vertical Video | `VerticalVideo` |
| Vlog | `Vlog` |
| VR | `VR` |

Use the [official dataset site](https://media.withyoutube.com/) to select and
download videos; category labels and filenames are also available in its
[video index](https://storage.googleapis.com/ugc-dataset/website/ugc_dataset.json).
The site offers both raw and H.264 versions. The
[original H.264 download folder](https://console.cloud.google.com/storage/browser/ugc-dataset/original_videos_h264)
provides MP4 inputs suitable for the preparation commands below. Record which
version you use; website previews are not the original download files.

Prepare one H.264 baseline per source video, then derive all five other codec
variants from that same baseline. Preserve the source identifier in the
baseline filename, for example `<source_stem>_h264.mp4`. The commands below set
`BASELINE` to this path and derive `BASELINE_STEM` and `GENERATED_DIR` from it.
This keeps different source videos' outputs separate. Prepare, run and inspect
one codec at a time for each video.

## Quick start: one codec at a time

Prepare and evaluate one codec, inspect its outputs, then move to the next.
The Python experiment entry point accepts one input video per invocation.

Use a Linux environment with Bash and the build prerequisites listed in the
[dependency guide](dependencies/README.md). Install and initialize Conda
before running the commands below. Download the selected YouTube-UGC videos
locally before preparing the inputs.
Complete the [system prerequisites](dependencies/README.md#build-prerequisites)
and [VVenC installation](dependencies/README.md#install-vvenc) before the
AOM/FFmpeg build commands below, even if starting with H.264 only.

```bash
# Create the Python environment once, then activate it in each new shell.
conda create -n letitgo-motivation python=3.12 pip
conda activate letitgo-motivation
export PYTHON_BIN=python3

cd motivation
python3 -m pip install -r requirements.txt

# Select this installation in each new shell.
export FFMPEG_PREFIX="$PWD/dependencies/install"
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# After installing VVenC, fetch and patch the AOM/FFmpeg sources once.
bash dependencies/prepare_sources.sh
JOBS=8 bash dependencies/build_dependencies.sh
```

`requirements.txt` pins the direct Python dependencies: NumPy, Pillow and
scikit-image. These versions were tested with Python 3.12.6; pip installs
their transitive dependencies. Check the Python installation with:

```bash
python3 -c "import numpy; from PIL import Image; from skimage.metrics import structural_similarity"
```

In each new shell, activate `letitgo-motivation`, set `PYTHON_BIN=python3`,
and export the three native dependency paths above from this module directory.
The commands use the activated environment's `python3` through `PATH`.
Users manage the Conda environment themselves. The Python requirements cover
the Python packages; build FFmpeg/AOM and install the native development
libraries using the dependency guide.

`build_dependencies.sh` does not install system packages. It expects the
patched source directories from `prepare_sources.sh`; its default `JOBS` is
the machine's `nproc` value. Set `ENABLE_SVTAV1=1` when building if the optional
SVT-AV1 development library is installed and you want that encoder.

### Prepare an input video

Run the following commands from `motivation/`. Keep the downloaded source
unchanged in `data/original/`, and write the prepared H.264 baseline separately
under `data/h264/`. The recommended layout is:

```text
motivation/
├── data/
│   ├── original/<downloaded_filename>.mp4
│   ├── h264/<source_stem>_h264.mp4
│   └── generated_codecs/<baseline_stem>/    # Other codecs and encode reports
└── result/
    ├── logs/                              # Preparation and experiment logs
    ├── smoke/                             # Single-frame checks
    └── ssim_loss/                         # Frame-range results and summaries
```

`data/` and `result/` are Git-ignored. The scripts create the generated codec
and result directories as needed. Videos may also stay elsewhere: supply an
absolute path via `--input`; the layout above is a convention, not a requirement.

Create the local directories, then download a selected MP4 from the
[YouTube-UGC H.264 folder](https://console.cloud.google.com/storage/browser/ugc-dataset/original_videos_h264)
into `data/original/`, retaining its original filename:

```bash
mkdir -p data/original data/h264 result/logs
```

Replace `<downloaded_filename>` below with the actual filename without its
final `.mp4` extension. Set these variables again for each new source video
or shell:

```bash
SOURCE_VIDEO="data/original/<downloaded_filename>.mp4"
SOURCE_STEM="$(basename "${SOURCE_VIDEO%.*}")"
BASELINE="data/h264/${SOURCE_STEM}_h264.mp4"
BASELINE_STEM="$(basename "${BASELINE%.*}")"
GENERATED_DIR="data/generated_codecs/$BASELINE_STEM"

set -o pipefail
bash code/ssim_loss_experiment/remove_b_frames.sh \
  --input "$SOURCE_VIDEO" --output "$BASELINE" \
  2>&1 | tee "result/logs/${BASELINE_STEM}_prepare.log"
```

This helper re-encodes the first video stream with libx264, CRF 11, preset
`medium`, B frames disabled, and 8-bit `yuv420p`. It preserves displayed frames
rather than dropping B-coded source frames, processes the full video without
an explicit resize or frame-rate change, and removes audio, subtitles and data
streams. It verifies the output; the final `Output frame types` line must show
`B=0`. The same 8-bit `yuv420p` conversion applies to clips in the HDR category.
Inspect the prepared baseline's codec, dimensions, pixel format, frame rate
and frame count as well:

```bash
"${FFPROBE_BIN:-$FFMPEG_PREFIX/bin/ffprobe}" -v error -select_streams v:0 \
  -show_entries stream=codec_name,has_b_frames,width,height,pix_fmt,avg_frame_rate,nb_frames \
  -of default=noprint_wrappers=1 "$BASELINE"
```

Expect `codec_name=h264`, `has_b_frames=0`, and `pix_fmt=yuv420p`; confirm the
resolution, frame rate and frame count match the video you intend to evaluate.

If the baseline output already exists, the helper skips it without rechecking
its contents. Use a new output filename, or explicitly add `--force` to
overwrite it. If you already have a suitable no-B-frame H.264 baseline, skip
conversion, set `BASELINE` to its path, and compute `BASELINE_STEM` and
`GENERATED_DIR` from it using the assignments above.

### Run and inspect each codec

Start with H.264. Check one frame first, writing to a separate smoke-test
directory so it is not reused by the full experiment:

```bash
python3 code/ssim_loss_experiment/run_experiment.py \
  --input "$BASELINE" --frame-index 0 --jobs 1 \
  --output-dir result/smoke
```

Inspect the printed `trials.csv` and `summary.json` paths. If the frame has
only the protected first packet, it has nothing to test; choose another frame.
Once the small run works, evaluate frames 0 through 599 for H.264:

```bash
set -o pipefail
python3 code/ssim_loss_experiment/run_experiment.py \
  --input "$BASELINE" \
  --start-frame-index 0 --stop-frame-index 599 --frame-jobs 4 --jobs 6 \
  --output-dir result/ssim_loss 2>&1 | tee "result/logs/${BASELINE_STEM}_run.log"

python3 summarize_results.py --result-root result/ssim_loss
```

Check the **current codec's** `frames_completed` and `packet_trials` in
`result/ssim_loss/numeric_summary/codecs.csv`, and the skip messages in its log.
Codecs not yet run have zero counts and blank metrics. Short inputs are
clamped to their available frames. Reduce `--frame-jobs` and `--jobs` if needed.

After checking H.264, prepare only H.265 and evaluate that input:

```bash
bash code/ssim_loss_experiment/prepare_matched_codecs.sh \
  --input "$BASELINE" --codec h265
```

The default output directory is `data/generated_codecs/<baseline_stem>/`,
matching `GENERATED_DIR` above. Inspect
`"${GENERATED_DIR}/${BASELINE_STEM}_h265_encode_report.txt"` before running the
experiment; the report and prepared video are saved in that same directory.

```bash
set -o pipefail
python3 code/ssim_loss_experiment/run_experiment.py \
  --input "${GENERATED_DIR}/${BASELINE_STEM}_h265.mp4" \
  --start-frame-index 0 --stop-frame-index 599 --frame-jobs 4 --jobs 6 \
  --output-dir result/ssim_loss 2>&1 | tee "result/logs/${BASELINE_STEM}_h265_run.log"

python3 summarize_results.py --result-root result/ssim_loss
```

For a one-frame check of each new codec, use the first Python command above
with its input path and `--output-dir result/smoke`. Continue manually with
VP8, VP9, AV1, and H.266: change `--codec`, the experiment's `--input`, and the
log filename together. Inspect each codec's results before starting the next.
Always prepare variants from the same H.264 baseline in `"$BASELINE"`, rather
than using the previous codec's generated video as the next source:

| Codec | Preparation `--codec` | Experiment `--input` |
| --- | --- | --- |
| H.264 | No preparation needed | `"$BASELINE"` |
| H.265 | `h265` | `"${GENERATED_DIR}/${BASELINE_STEM}_h265.mp4"` |
| VP8 | `vp8` | `"${GENERATED_DIR}/${BASELINE_STEM}_vp8.webm"` |
| VP9 | `vp9` | `"${GENERATED_DIR}/${BASELINE_STEM}_vp9.webm"` |
| AV1 | `av1` | `"${GENERATED_DIR}/${BASELINE_STEM}_av1.mkv"` |
| H.266 | `h266` | `"${GENERATED_DIR}/${BASELINE_STEM}_h266.mp4"` |

Each summarization includes the results collected so far. Use the same result
root for codecs with the same experiment settings, and separate roots for
different packet-size or encoding configurations. Use distinct input stems
for different source videos because per-frame directories are keyed by stem.

Interrupted-run resumption is not supported. If an experiment is interrupted,
rerun that codec's entire frame range with a new `--output-dir`, or add
`--overwrite` to its Python command to replace the corresponding frame outputs.
Existing frame directories are otherwise skipped without a completeness check.
Prepared video inputs can be reused; `--force` on the preparation script
regenerates the selected variant.

The native helper builds automatically. Run `make -C code/native clean` after
changing the FFmpeg installation so the next run links the selected build.

## Optional batch tools

`scripts/run_motivation.sh` and `run_six_codecs.sh` remain available for batch
automation after checking the codecs individually. The unified script's
default `all` stage prepares five variants, runs all six inputs, and summarizes
their results. It keeps existing per-frame outputs; preparation refuses to
overwrite existing variants. `--stage run` reuses prepared videos, while
`--force` replaces generated variants and corresponding experiment outputs.

```bash
bash scripts/run_motivation.sh --help
bash dependencies/build_dependencies.sh --help
```

| Parameter | Default | Meaning |
| --- | --- | --- |
| `--stage` | `all` | `prepare`, `run`, `summarize`, or the three stages in order |
| `--input` | Required except for `summarize` | H.264 baseline; use the helper above to convert other inputs |
| `--generated-dir` | Module `data/generated_codecs/<input_stem>` | Five prepared codec variants |
| `--output-dir` | Module `result/ssim_loss` | Per-frame results and `numeric_summary/` |
| `--start-frame`, `--stop-frame` | `0`, `599` | Inclusive frame range; short inputs are clamped |
| `--frame-jobs`, `--packet-jobs` | `4`, `6` | Frame and per-frame packet-batch concurrency |
| `--packet-size` | `1500` | Bytes per synthetic packet |
| `--bitrate` | Inferred from baseline | Target bitrate for generated variants, e.g. `4M`; applies to preparation |
| `--force` | Off | Replace generated variants and per-frame results in the selected stages |
| `--dry-run` | Off | Print commands only, without probing, encoding, decoding or file writes |

Paths explicitly supplied on the command line are relative to the caller's
current directory; omitted path defaults are relative to this module. The
script can also be invoked from elsewhere using its full path. It defaults
`FFMPEG_PREFIX` to this module's `dependencies/install` and `PYTHON_BIN` to
`python3`. Set `FFMPEG_PREFIX` to use another custom decoder installation.
To select a Python environment, activate it and set `PYTHON_BIN=python3` as
above. `FFMPEG_BIN` and `FFPROBE_BIN` can override the individual tools.

Examples of separate stages:

```bash
# Preview the complete workflow, without requiring downloaded sources or inputs.
bash scripts/run_motivation.sh --input "$BASELINE" --dry-run

# Batch preparation and execution for all six codecs.
bash scripts/run_motivation.sh --stage prepare --input "$BASELINE"
bash scripts/run_motivation.sh --stage run --input "$BASELINE" \
  --start-frame 0 --stop-frame 599 --frame-jobs 4 --packet-jobs 6

# Summarization needs existing results, but not the video input or FFmpeg.
bash scripts/run_motivation.sh --stage summarize --output-dir result/ssim_loss
```

`--stage run` does not invoke summarization; `--stage all` does. Numeric
summary CSVs are regenerated from the existing per-frame results whenever
the summarize stage is run. Use separate `--output-dir` values for different
packet-size or encoding configurations to avoid mixing their results.

The lower-level `run_six_codecs.sh H264_BASELINE GENERATED_CODEC_DIR` checks
and runs all six inputs. Its `DRY_RUN=1` probes inputs before printing commands,
and it defaults to `OVERWRITE=1`; set `OVERWRITE=0` to preserve existing frame
directories. This differs from the unified entry point's defaults.

## Contents

| Path | Purpose |
| --- | --- |
| `scripts/run_motivation.sh` | Optional batch prepare/run/summarize workflow |
| `dependencies/build_dependencies.sh` | Build patched AOM, FFmpeg and native helper |
| `code/ssim_loss_experiment/remove_b_frames.sh` | Prepare a no-B-frame H.264 baseline when needed |
| `code/ssim_loss_experiment/prepare_matched_codecs.sh` | Prepare one codec with `--codec`, or all five variants if omitted |
| `run_six_codecs.sh` | Optional batch check and run of all six prepared inputs |
| `code/ssim_loss_experiment/run_experiment.py` | Main experiment entry: one codec input, one frame or a frame range |
| `code/packet_loss/` | Probing, scheduling, SSIM, native-helper orchestration |
| `code/native/` | C/libavcodec decoding and independent packet-corruption trials |
| `summarize_results.py` | Export cross-frame and cross-codec numeric CSV summaries |
| `dependencies/` | History-free decoder patches and build instructions |

## Experimental definition

For each target frame, the helper obtains its encoded payload from the
container and divides it into synthetic packets of `P=1500` bytes by default.
For payload length `L`, the packet count is `N=ceil(L/P)`.

1. Decode the intact input to obtain the reference frame for this codec.
2. Keep packet 1 intact. For each packet `j=2..N`, replace that packet's bytes
   with zeros, preserving payload length. The last packet can be shorter.
3. Create a fresh decoder context for each trial and replay the intact
   reference history from cached encoded packets, so losses and concealment
   state from different trials do not accumulate.
4. Decode the damaged target frame and measure RGB SSIM against the intact
   decoded frame. Packet importance is `ssim_drop = 1 - SSIM`.

This is a synthetic payload-corruption model, not removal of actual RTP
packets. The reference is the clean reconstruction of the **same codec**, not
the original uncompressed source. The metric is raw SSIM loss, not dB-SSIM.
The native implementation uses a 7x7 uniform SSIM window and averages the
three RGB channels; the Python fallback uses the corresponding scikit-image
SSIM calculation.

Two position columns are retained in `trials.csv`:

| Column | Definition |
| --- | --- |
| `relative_position_lossable` | `(j-2)/(N-2)`; 0 for the single lossable-packet case |
| `relative_position_all_packets` | `(j-1)/(N-1)` |

The per-frame correlation uses `relative_position_lossable`. A negative
correlation means later packet losses tend to cause less damage in that frame.

An explicitly reported unsuccessful damaged-frame decode/SSIM evaluation is
retained with `decode_success=False`, `SSIM=0`, and `ssim_drop=1`. Frames with
no lossable packets are skipped. These skips are reported in the run log and
do not produce completed per-frame JSONs.
Frame ranges index the frames that ffprobe reports with payload positions and
sizes. Preserve the run log when checking how many of the requested frames
were actually evaluated.

A failure to extract the intact reference frame is an experiment error in
both single-frame and frame-range runs, including parallel runs. This includes
a native process that cannot start, exits with a nonzero status, or returns
status zero without a nonempty output file. The frame directory and diagnostic
report are retained, and the codec command exits unsuccessfully.

A native packet batch that exits with a nonzero status, or omits a requested
packet result even with status zero, is an experiment error. The codec command
exits unsuccessfully after writing the diagnostic report described below.
In either case, the affected frame does not produce completed `trials.csv` or
`summary.json`, and missing measurements are not assigned SSIM loss 1. Other
concurrently running frames may still complete.

## Codec configuration and dependencies

| Codec | Encoder/input | Relevant preparation settings |
| --- | --- | --- |
| H.264 | Supplied H.264 baseline | Optional no-B-frame helper uses libx264, CRF 11 by default |
| H.265 | `libx265` | Two pass; B frames/open GOP disabled |
| VP8 | `libvpx` | Two pass; one token partition; alt-ref disabled |
| VP9 | `libvpx-vp9` | Two pass; one tile |
| AV1 | `libsvtav1`, otherwise `libaom-av1` | One tile; SVT single pass or AOM two pass |
| H.266 | `libvvenc` | Single pass; 10-bit input |

The preparation script targets the same bitrate parameter for the five
generated variants, inferred from the H.264 input unless `--bitrate` is set.
Their rate-control algorithms and achieved rates differ; inspect the generated
`*_encode_report.txt`. With `--codec`, each report is saved as
`<input_stem>_<codec>_encode_report.txt`, so preparing the next codec preserves
earlier reports. H.264 is reused, so its original GOP/rate-control
configuration is not changed by this script. If preparing a temporal subclip,
first create the H.264 baseline for that same subclip, then encode all variants.
Each `--codec` invocation uses the selected codec's encoder; preparing all five
variants requires all listed encoders. Unavailable requested AV1 or H.266
encoders cause an error. The supplied dependency build still enables all
documented codec libraries.

Build the experiment's patched FFmpeg/AOM using the
[dependency guide](dependencies/README.md). The patches apply to pinned public
upstream revisions and include error concealment based on the known synthetic
corruption byte range. The measurements reflect these encoder settings and
custom decoder recovery rules.

## Outputs and numeric summary

Each completed frame writes
`result/ssim_loss/<input_stem>/frame_<index>/` containing:

- `trials.csv`: packet indices, both relative positions, corruption byte
  ranges, decode success, SSIM and SSIM loss.
- `metadata.json`: input/frame identity, packet size, decoder helper and
  scheduling information.
- `summary.json`: per-frame means, failed-decode count and Pearson correlation
  between packet position and SSIM loss.

Temporary reconstructed PPM frames are deleted after scoring. No PNG/PDF
figures are generated.

Native failures retain diagnostic reports inside the affected frame directory:

| Report | Failure and additional details |
| --- | --- |
| `frames/native_reference_error.json` | Intact reference extraction failed; includes the working directory, frame index, output path and whether a nonempty output exists. The return code is `null` if the process could not start. |
| `frames/native_batch_error_<start>_<stop>.json` | A packet batch failed or returned incomplete results; includes expected/reported/missing packet indices. Each batch has its own report, with the first and last indices padded to at least four digits. |

Both reports include the command, input path, return code, signal name when
applicable (such as `SIGSEGV`), target timestamp and payload offset, packet
size, and captured stdout/stderr. Inspect the report and the run log before
rerunning the current codec's entire frame range into a new result root or
with `--overwrite`, as above.

```bash
python3 summarize_results.py --result-root result/ssim_loss
```

This writes `numeric_summary/frames.csv` and `numeric_summary/codecs.csv`.
The codec table includes input-video count, completed-frame count, packet and
failure counts, fraction of defined frame correlations that are negative,
mean frame correlation, and mean frame SSIM loss. The latter two weight
completed frames equally. Undefined correlations (for example, constant
SSIM loss) are left blank and excluded only from correlation aggregates.
Failed packet trials remain included in loss aggregates. A codec with no
records has zero counts and blank metrics. Check the current codec's
`frames_completed` and `packet_trials` against its run log.

The numerical summary uses the recorded per-frame SSIM loss and correlation
values from all completed frames under the result root. Count skipped frames
from the run log.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| Missing `libvvenc` or an encoder | Install/build the required development library, expose its pkg-config path, and rebuild the custom FFmpeg |
| Missing `AV_CODEC_ID_VVC` during native compilation | Point `FFMPEG_PREFIX` and pkg-config at the supplied recent custom FFmpeg build |
| Shared library cannot be loaded | Use the same custom prefix for tools, native linking and `LD_LIBRARY_PATH` |
| Python import error | Activate `letitgo-motivation`, set `PYTHON_BIN=python3`, then run `python3 -m pip install -r requirements.txt` |
| Codec variants already exist | Pass the prepared video to the Python experiment, or add `--force` to the preparation command to regenerate it |
| Zero counts for a codec | Expected for codecs not yet run; for the current codec, check its log for skipped frames and reference-decode failures |
| Intact reference extraction failed | Inspect the reported `native_reference_error.json` and run log; resolve the error before rerunning the current codec's full frame range |
| Native packet batch failed or returned incomplete results | Inspect the reported `native_batch_error_*.json` and run log; rerun the current codec's full frame range after resolving the error |
| Blank correlation | A constant loss sequence or insufficient samples can make Pearson correlation undefined |
