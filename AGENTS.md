# Repository guidance

## Project and layout

This repository contains the standalone offline six-codec motivation
experiment for LetItGo ATC '26. Read `README.md` first, then follow
`motivation/README.md` for the complete reproduction workflow.

- `motivation/code/ssim_loss_experiment/run_experiment.py`:
  recommended experiment entry point for one codec input at a time.
- `motivation/code/ssim_loss_experiment/prepare_matched_codecs.sh`:
  prepare a selected variant with `--codec`.
- `motivation/scripts/run_motivation.sh`: optional batch preparation,
  experiment execution, and numeric summarization.
- `motivation/code/packet_loss/`: Python probing, scheduling,
  metrics, and native-helper orchestration.
- `motivation/code/native/`: C/libavcodec trial helper and Makefile.
- `motivation/dependencies/`: pinned FFmpeg/AOM patches and build scripts.
- `motivation/summarize_results.py`: numeric summaries across frames and codecs.

The motivation experiment uses its own patched FFmpeg/AOM installation.
Do not substitute stock FFmpeg for that build.

## Build and run

Run from `motivation/` and follow
`dependencies/README.md` for system prerequisites:

```bash
conda activate letitgo-motivation
export PYTHON_BIN=python3
python3 -m pip install -r requirements.txt
export FFMPEG_PREFIX="$PWD/dependencies/install"
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
bash dependencies/prepare_sources.sh
JOBS=8 bash dependencies/build_dependencies.sh
python3 code/ssim_loss_experiment/run_experiment.py \
  --input /path/to/baseline_h264.mp4 --frame-index 0 --jobs 1 \
  --output-dir result/smoke
```

Create the Conda environment using the motivation README before these steps.
Prepare and evaluate codecs one by one, checking each codec's outputs before
starting the next. Interrupted-run resumption is not supported; rerun the
current codec's full range into a new result root or explicitly overwrite its
frame outputs. Input videos are user-supplied, and the supplied dependency
build requires all documented encoder libraries, including VVenC. Keep build,
frame, and packet concurrency appropriate to available resources. Rebuild the
native helper after changing the FFmpeg installation.

## Validation

Choose checks appropriate to the changed component. These commands run from
the repository root without requiring videos or downloaded decoder sources:

```bash
bash motivation/scripts/run_motivation.sh --help
bash motivation/scripts/run_motivation.sh --input /tmp/example_h264.mp4 --dry-run
bash motivation/dependencies/build_dependencies.sh --dry-run
python3 motivation/summarize_results.py --help
git diff --check
```

For shell changes, run `bash -n` on changed scripts. For Python changes, check
syntax and exercise the affected CLI or behavior. For native changes,
build the native helper and exercise the affected behavior when dependencies are
available. Report missing prerequisites and distinguish syntax/dry-run checks
from a compiled build or completed experiment. Do not claim reproduction of
paper measurements without actually running and inspecting them.

## Editing and experiment semantics

- Follow surrounding style for Python, shell, and C changes.
  Keep changes focused; avoid unrelated formatting of upstream code.
- Keep English and Chinese motivation/dependency guides consistent when
  changing documented behavior.
- Preserve pinned dependency revisions and patch provenance. If changing a
  vendored decoder, account for the corresponding reconstruction patch.
- The experiment zeroes synthetic payload packets while preserving payload
  length and packet 1. Each trial uses intact decoder reference history.
  Its metric is `1 - SSIM` against the clean reconstruction of the same codec.
  Changes to these semantics must be explicit and documented.
- Results depend on custom decoder recovery behavior. Do not describe them
  as measurements of six unmodified stock decoders.
- The unified workflow preserves existing per-frame results by default;
  `--force` replaces generated variants/results. The lower-level
  `run_six_codecs.sh` instead defaults to `OVERWRITE=1`. Preserve user data and
  use separate output directories for different experiment configurations.
- Keep generated videos, results, caches, dependency builds/installations,
  native binaries, and personal automation out of commits. Current artifact
  outputs are numeric; plotting scripts and input videos are not bundled.
