#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys

CODE_ROOT = Path(__file__).resolve().parents[1]
if str(CODE_ROOT) not in sys.path:
    sys.path.insert(0, str(CODE_ROOT))

from packet_loss.experiment import (
    ExperimentArtifacts,
    run_frame_experiments_with_global_scheduler,
    run_single_frame_experiment,
)
from packet_loss.ffmpeg_tools import probe_video_frames
from packet_loss.native_tools import build_native_frame_extractor


def _print_artifacts(artifacts: ExperimentArtifacts) -> None:
    print(f"Run directory: {artifacts.run_directory}")
    print(f"Metadata: {artifacts.metadata_path}")
    print(f"Trials CSV: {artifacts.trials_path}")
    print(f"Summary: {artifacts.summary_path}")
    print(f"Clean frame path: {artifacts.clean_frame_path} (deleted after frame if created)")


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]

    parser = argparse.ArgumentParser(
        description=(
            "Run a single-frame packet-loss experiment by zeroing one 1500-byte packet "
            "at a time, starting from packet 2, and measuring SSIM against the clean frame."
        )
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Path to the input video file.",
    )
    parser.add_argument(
        "--frame-index",
        type=int,
        help="0-based display frame index to test. Use this for a single-frame run.",
    )
    parser.add_argument(
        "--start-frame-index",
        type=int,
        help="0-based display frame index to start from, inclusive.",
    )
    parser.add_argument(
        "--stop-frame-index",
        type=int,
        help="0-based display frame index to stop at, inclusive.",
    )
    parser.add_argument(
        "--packet-size",
        type=int,
        default=1500,
        help="Packet size in bytes used for the synthetic loss model. Default: 1500.",
    )
    parser.add_argument(
        "--output-dir",
        default=str(repo_root / "result" / "ssim_loss"),
        help="Directory used to store experiment outputs. Default: ./result/ssim_loss",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="Number of packet trials to process in parallel. Default: 1.",
    )
    parser.add_argument(
        "--frame-jobs",
        type=int,
        default=1,
        help="Number of frame experiments to process in parallel. Default: 1.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Replace the existing output directory for the same input/frame pair.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    def log_progress(message: str) -> None:
        print(f"[progress] {message}", flush=True)

    single_frame_requested = args.frame_index is not None
    if single_frame_requested:
        if args.start_frame_index is not None or args.stop_frame_index is not None:
            raise SystemExit("Use either --frame-index or --start-frame-index/--stop-frame-index, not both.")
        start_frame_index = args.frame_index
        stop_frame_index = args.frame_index
    else:
        if args.start_frame_index is None or args.stop_frame_index is None:
            raise SystemExit(
                "Provide --frame-index for a single frame, or both --start-frame-index and --stop-frame-index for a range."
            )
        start_frame_index = args.start_frame_index
        stop_frame_index = args.stop_frame_index

    if start_frame_index > stop_frame_index:
        raise SystemExit("--start-frame-index must be less than or equal to --stop-frame-index.")
    if args.jobs <= 0:
        raise SystemExit("--jobs must be a positive integer.")
    if args.frame_jobs <= 0:
        raise SystemExit("--frame-jobs must be a positive integer.")

    input_path = Path(args.input).resolve()
    output_root = Path(args.output_dir).resolve()

    log_progress("Building native frame extractor")
    extractor_path = build_native_frame_extractor()

    if start_frame_index == stop_frame_index:
        log_progress(f"Probing frame {start_frame_index} in {input_path.name}")
    else:
        log_progress(f"Probing frames {start_frame_index}..{stop_frame_index} in {input_path.name}")
    all_frame_probes = probe_video_frames(input_path)
    last_frame_index = len(all_frame_probes) - 1
    if start_frame_index < 0:
        raise SystemExit("Frame indices must be non-negative.")
    if start_frame_index > last_frame_index:
        raise SystemExit(
            f"--start-frame-index {start_frame_index} is out of range for {input_path.name}; "
            f"valid range is 0..{last_frame_index}."
        )
    if stop_frame_index > last_frame_index:
        if single_frame_requested:
            raise SystemExit(
                f"--frame-index {stop_frame_index} is out of range for {input_path.name}; "
                f"valid range is 0..{last_frame_index}."
            )
        log_progress(
            f"Clamping stop frame {stop_frame_index} to last available frame {last_frame_index}"
        )
        stop_frame_index = last_frame_index

    frame_indices = tuple(range(start_frame_index, stop_frame_index + 1))
    frame_probes = {frame_index: all_frame_probes[frame_index] for frame_index in frame_indices}

    frame_workers = min(args.frame_jobs, len(frame_indices))
    cpu_count = os.cpu_count()
    estimated_workers = frame_workers * args.jobs
    if frame_workers > 1:
        pipeline_workers = frame_workers
        if cpu_count is not None:
            pipeline_workers = min(pipeline_workers, max(0, cpu_count - estimated_workers))
        estimated_workers += pipeline_workers
    if cpu_count is not None and estimated_workers > cpu_count:
        log_progress(
            f"Warning: requested concurrency is about {estimated_workers} native workers on {cpu_count} CPUs"
        )

    if frame_workers == 1:
        for frame_index in frame_indices:
            print(f"[progress] Starting frame {frame_index}", flush=True)
            artifacts = run_single_frame_experiment(
                input_path=input_path,
                frame_index=frame_index,
                packet_size=args.packet_size,
                output_root=output_root,
                overwrite=args.overwrite,
                jobs=args.jobs,
                progress_callback=log_progress,
                frame_probe=frame_probes[frame_index],
                extractor_path=extractor_path,
            )
            if artifacts is not None:
                _print_artifacts(artifacts)
        return

    artifacts_list = run_frame_experiments_with_global_scheduler(
        input_path=input_path,
        frame_indices=frame_indices,
        packet_size=args.packet_size,
        output_root=output_root,
        overwrite=args.overwrite,
        packet_jobs=args.jobs,
        frame_jobs=frame_workers,
        progress_callback=log_progress,
        frame_probes=frame_probes,
        extractor_path=extractor_path,
    )
    for artifacts in artifacts_list:
        _print_artifacts(artifacts)


if __name__ == "__main__":
    main()
