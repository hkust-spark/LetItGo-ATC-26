# LetItGo motivation experiments

This repository contains the standalone offline motivation experiment for
LetItGo ATC '26. It measures the quality loss caused by independently
corrupting one packet within an encoded frame across **H.264, H.265/HEVC,
VP8, VP9, AV1, and H.266/VVC**.

Follow the complete reproduction guide: [English](motivation/README.md) |
[简体中文](motivation/README.zh-CN.md).

The guide covers the YouTube-UGC video inputs, a user-managed Conda
environment, native dependencies, input preparation, packet-corruption
experiments, and numeric result summaries. Prepare and run **one codec at a
time**, checking its outputs before proceeding to the next.

Place downloaded videos in `motivation/data/original/`. The guides explain
how to prepare H.264 baselines in `motivation/data/h264/` and generate each
codec's input under `motivation/data/generated_codecs/`.

Build the supplied patched FFmpeg/AOM dependencies using the
[dependency guide](motivation/dependencies/README.md)
([中文](motivation/dependencies/README.zh-CN.md)). The experiment uses custom
decoder recovery behavior and runs as a standalone offline workflow.

| Path | Purpose |
| --- | --- |
| `motivation/code/ssim_loss_experiment/` | Input preparation and experiment entry point |
| `motivation/code/packet_loss/` | Python probing, scheduling, and metrics |
| `motivation/code/native/` | Native decoding and independent packet-corruption trials |
| `motivation/dependencies/` | Pinned source patches and dependency build scripts |
| `motivation/summarize_results.py` | Numeric summaries across frames and codecs |

Build artifacts and local experiment data are ignored by Git.
