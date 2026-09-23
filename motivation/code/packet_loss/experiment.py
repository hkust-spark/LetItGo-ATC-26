from __future__ import annotations

from collections import deque
from collections.abc import Iterable, Mapping
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, as_completed, wait
from dataclasses import asdict, dataclass
import csv
import json
import math
import multiprocessing
from pathlib import Path
import shutil
from typing import Any, Callable

import numpy as np

from .ffmpeg_tools import FrameProbe, probe_target_frame
from .metrics import calculate_ssim_arrays, load_rgb_image
from .native_tools import (
    build_native_frame_extractor,
    extract_frame_with_native_decoder,
    extract_frames_with_native_decoder_batch,
)

FRAME_OUTPUT_EXTENSION = "ppm"


@dataclass(frozen=True)
class TrialResult:
    packet_index_1based: int
    lossable_rank_1based: int
    total_packets_in_frame: int
    lossable_packet_count: int
    relative_position_lossable: float
    relative_position_all_packets: float
    zeroed_byte_start: int
    zeroed_byte_end: int
    zeroed_byte_count: int
    corrupted_frame_path: str
    decode_success: bool
    ssim: float
    ssim_drop: float
    decode_message: str


@dataclass(frozen=True)
class ExperimentArtifacts:
    run_directory: Path
    metadata_path: Path
    trials_path: Path
    summary_path: Path
    clean_frame_path: Path


@dataclass(frozen=True)
class PacketTrialBatchTask:
    frame_index: int
    extractor_path: Path
    input_path: Path
    frames_directory: Path
    clean_frame_path: Path
    target_timestamp: int
    packet_offset: int
    encoded_packet_size: int
    transport_packet_size: int
    packet_indices_1based: tuple[int, ...]
    total_packets: int
    lossable_packet_count: int


@dataclass(frozen=True)
class PreparedFrameExperiment:
    input_path: Path
    frame_probe: FrameProbe
    packet_size: int
    run_directory: Path
    frames_directory: Path
    clean_frame_path: Path
    trials_path: Path
    summary_path: Path
    metadata_path: Path
    extractor_path: Path
    total_packets: int
    lossable_packet_count: int
    packet_batch_count: int
    trial_batch_tasks: tuple[PacketTrialBatchTask, ...]
    skip_reason: str | None = None


@dataclass(frozen=True)
class FramePrepareTask:
    input_path: Path
    frame_index: int
    packet_size: int
    output_root: Path
    overwrite: bool
    packet_batch_count: int
    frame_probe: FrameProbe | None
    extractor_path: Path


@dataclass(frozen=True)
class FrameFinalizeTask:
    prepared: PreparedFrameExperiment
    trials: tuple[TrialResult, ...]
    scheduler_mode: str


@dataclass(frozen=True)
class SchedulerFutureContext:
    kind: str
    frame_index: int
    packet_task: PacketTrialBatchTask | None = None


ProgressCallback = Callable[[str], None]


def _safe_video_stem(input_path: Path) -> str:
    return input_path.stem.replace(" ", "_")


def _emit_progress(progress_callback: ProgressCallback | None, message: str) -> None:
    if progress_callback is not None:
        progress_callback(message)


def _run_directory_path(output_root: Path, input_path: Path, frame_index: int) -> Path:
    return output_root / _safe_video_stem(input_path) / f"frame_{frame_index:04d}"


def _prepare_run_directory(output_root: Path, input_path: Path, frame_index: int, overwrite: bool) -> Path:
    run_directory = _run_directory_path(output_root, input_path, frame_index)
    if run_directory.exists():
        if not overwrite:
            raise FileExistsError(
                f"Output directory already exists: {run_directory}. "
                "Use --overwrite to replace it."
            )
        shutil.rmtree(run_directory)

    (run_directory / "frames").mkdir(parents=True, exist_ok=True)
    return run_directory


def _zeroed_packet_range(
    packet_offset: int,
    encoded_packet_size: int,
    packet_index_1based: int,
    transport_packet_size: int,
) -> tuple[int, int]:
    if packet_index_1based < 2:
        raise ValueError("packet_index_1based must be at least 2; packet 1 is protected")

    total_packets = math.ceil(encoded_packet_size / transport_packet_size)
    if packet_index_1based > total_packets:
        raise ValueError(
            f"packet_index_1based={packet_index_1based} exceeds total packet count {total_packets}"
        )

    relative_start = (packet_index_1based - 1) * transport_packet_size
    relative_end = min(packet_index_1based * transport_packet_size, encoded_packet_size)
    absolute_start = packet_offset + relative_start
    absolute_end = packet_offset + relative_end
    return absolute_start, absolute_end


def _relative_position_lossable(packet_index_1based: int, total_packets: int) -> float:
    if total_packets <= 2:
        return 0.0
    return (packet_index_1based - 2) / (total_packets - 2)


def _relative_position_all_packets(packet_index_1based: int, total_packets: int) -> float:
    if total_packets <= 1:
        return 0.0
    return (packet_index_1based - 1) / (total_packets - 1)


def _write_trials_csv(trials: list[TrialResult], output_path: Path) -> None:
    fieldnames = list(asdict(trials[0]).keys()) if trials else list(TrialResult.__dataclass_fields__.keys())
    with output_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for trial in trials:
            writer.writerow(asdict(trial))


def _build_summary(
    input_path: Path,
    frame_probe: FrameProbe,
    packet_size: int,
    clean_frame_path: Path,
    trials: list[TrialResult],
) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "input_path": str(input_path),
        "codec_name": frame_probe.codec_name,
        "frame_index": frame_probe.frame_index,
        "total_frames": frame_probe.total_frames,
        "frame_timestamp": frame_probe.timestamp,
        "frame_timestamp_seconds": frame_probe.timestamp_seconds,
        "stream_time_base": frame_probe.stream_time_base,
        "picture_type": frame_probe.picture_type,
        "is_key_frame": frame_probe.is_key_frame,
        "packet_offset": frame_probe.packet_offset,
        "packet_size_bytes": frame_probe.packet_size,
        "transport_packet_size_bytes": packet_size,
        "total_packets_in_frame": math.ceil(frame_probe.packet_size / packet_size),
        "lossable_packets_tested": len(trials),
        "clean_frame_path": str(clean_frame_path),
    }

    if not trials:
        summary["message"] = "No lossable packets were available for this frame."
        return summary

    most_damaging = max(trials, key=lambda trial: trial.ssim_drop)
    least_damaging = min(trials, key=lambda trial: trial.ssim_drop)

    x_values = np.asarray([trial.packet_index_1based for trial in trials], dtype=float)
    y_values = np.asarray([trial.ssim_drop for trial in trials], dtype=float)
    relative_x = np.asarray([trial.relative_position_lossable for trial in trials], dtype=float)

    summary.update(
        {
            "most_damaging_packet_index": most_damaging.packet_index_1based,
            "most_damaging_ssim_drop": most_damaging.ssim_drop,
            "least_damaging_packet_index": least_damaging.packet_index_1based,
            "least_damaging_ssim_drop": least_damaging.ssim_drop,
            "mean_ssim_drop": float(y_values.mean()),
            "median_ssim_drop": float(np.median(y_values)),
            "min_ssim": float(min(trial.ssim for trial in trials)),
            "max_ssim": float(max(trial.ssim for trial in trials)),
            "failed_decode_count": int(sum(not trial.decode_success for trial in trials)),
            "packet_index_vs_ssim_drop_correlation": (
                float(np.corrcoef(x_values, y_values)[0, 1]) if len(trials) > 1 else None
            ),
            "relative_position_vs_ssim_drop_correlation": (
                float(np.corrcoef(relative_x, y_values)[0, 1]) if len(trials) > 1 else None
            ),
        }
    )
    return summary


def _build_trial_result(
    task: PacketTrialBatchTask,
    packet_index: int,
    decode_success: bool,
    decode_message: str,
    ssim_value: float | None,
    clean_frame: np.ndarray | None,
) -> TrialResult:
    zeroed_byte_start, zeroed_byte_end = _zeroed_packet_range(
        packet_offset=task.packet_offset,
        encoded_packet_size=task.encoded_packet_size,
        packet_index_1based=packet_index,
        transport_packet_size=task.transport_packet_size,
    )

    corrupted_frame_path = task.frames_directory / f"packet_{packet_index:04d}.{FRAME_OUTPUT_EXTENSION}"
    try:
        if decode_success:
            if ssim_value is None:
                try:
                    if clean_frame is None:
                        clean_frame = load_rgb_image(task.clean_frame_path)
                    candidate_frame = load_rgb_image(corrupted_frame_path)
                    ssim_value = calculate_ssim_arrays(clean_frame, candidate_frame)
                except Exception as exc:
                    decode_success = False
                    decode_message = "\n".join(
                        message
                        for message in (
                            decode_message.strip(),
                            f"SSIM calculation failed: {exc}",
                        )
                        if message
                    )
                    ssim_value = 0.0
        else:
            ssim_value = 0.0
    finally:
        if corrupted_frame_path.exists():
            corrupted_frame_path.unlink()

    return TrialResult(
        packet_index_1based=packet_index,
        lossable_rank_1based=packet_index - 1,
        total_packets_in_frame=task.total_packets,
        lossable_packet_count=task.lossable_packet_count,
        relative_position_lossable=_relative_position_lossable(packet_index, task.total_packets),
        relative_position_all_packets=_relative_position_all_packets(packet_index, task.total_packets),
        zeroed_byte_start=zeroed_byte_start,
        zeroed_byte_end=zeroed_byte_end,
        zeroed_byte_count=zeroed_byte_end - zeroed_byte_start,
        corrupted_frame_path=str(corrupted_frame_path),
        decode_success=decode_success,
        ssim=ssim_value,
        ssim_drop=1.0 - ssim_value,
        decode_message=decode_message,
    )


def _run_packet_trial_batch(task: PacketTrialBatchTask) -> list[TrialResult]:
    decode_results = extract_frames_with_native_decoder_batch(
        extractor_path=task.extractor_path,
        input_path=task.input_path,
        output_dir=task.frames_directory,
        target_timestamp=task.target_timestamp,
        target_packet_offset=task.packet_offset,
        transport_packet_size=task.transport_packet_size,
        corrupt_packet_indices=task.packet_indices_1based,
        output_extension=FRAME_OUTPUT_EXTENSION,
        ssim_reference_path=task.clean_frame_path,
    )
    clean_frame = None
    if any(result[0] and result[2] is None for result in decode_results.values()):
        clean_frame = load_rgb_image(task.clean_frame_path)

    return [
        _build_trial_result(
            task=task,
            packet_index=packet_index,
            decode_success=decode_results[packet_index][0],
            decode_message=decode_results[packet_index][1],
            ssim_value=decode_results[packet_index][2],
            clean_frame=clean_frame,
        )
        for packet_index in task.packet_indices_1based
    ]


def _process_pool_kwargs(max_workers: int) -> dict[str, Any]:
    pool_kwargs: dict[str, Any] = {"max_workers": max_workers}
    try:
        pool_kwargs["mp_context"] = multiprocessing.get_context("fork")
    except ValueError:
        pass
    return pool_kwargs


def _split_contiguous_packet_indices(packet_indices: list[int], chunk_count: int) -> list[tuple[int, ...]]:
    chunks: list[tuple[int, ...]] = []
    start_index = 0
    for chunk_index in range(chunk_count):
        chunk_size = len(packet_indices) // chunk_count
        if chunk_index < len(packet_indices) % chunk_count:
            chunk_size += 1
        stop_index = start_index + chunk_size
        if start_index < stop_index:
            chunks.append(tuple(packet_indices[start_index:stop_index]))
        start_index = stop_index
    return chunks


def prepare_single_frame_experiment(
    input_path: Path,
    frame_index: int,
    packet_size: int,
    output_root: Path,
    overwrite: bool = False,
    packet_batch_count: int = 1,
    progress_callback: ProgressCallback | None = None,
    frame_probe: FrameProbe | None = None,
    extractor_path: Path | None = None,
) -> PreparedFrameExperiment:
    input_path = input_path.resolve()
    output_root = output_root.resolve()

    if packet_size <= 0:
        raise ValueError("packet_size must be a positive integer")
    if packet_batch_count <= 0:
        raise ValueError("packet_batch_count must be a positive integer")
    if not input_path.exists():
        raise FileNotFoundError(f"Input file not found: {input_path}")

    if frame_probe is None:
        _emit_progress(progress_callback, f"Probing frame {frame_index} in {input_path.name}")
        frame_probe = probe_target_frame(input_path, frame_index)
    elif frame_probe.frame_index != frame_index:
        raise ValueError(
            f"frame_probe.frame_index={frame_probe.frame_index} does not match frame_index={frame_index}"
        )

    total_packets = math.ceil(frame_probe.packet_size / packet_size)
    lossable_packet_count = max(0, total_packets - 1)
    _emit_progress(
        progress_callback,
        (
            f"Target frame ready: codec={frame_probe.codec_name}, timestamp={frame_probe.timestamp} "
            f"({frame_probe.timestamp_seconds:.3f}s), payload={frame_probe.packet_size} bytes, "
            f"packets={total_packets}, testing={lossable_packet_count}"
        ),
    )

    if extractor_path is None:
        _emit_progress(progress_callback, "Building native frame extractor")
        extractor_path = build_native_frame_extractor()
    elif not extractor_path.exists():
        raise FileNotFoundError(f"Native frame extractor not found: {extractor_path}")

    run_directory = _run_directory_path(output_root, input_path, frame_index)
    frames_directory = run_directory / "frames"
    clean_frame_path = frames_directory / f"clean_frame.{FRAME_OUTPUT_EXTENSION}"

    if run_directory.exists() and not overwrite:
        skip_reason = (
            f"Output directory already exists for frame {frame_index}: {run_directory}. "
            "Skipping because overwrite is disabled."
        )
        _emit_progress(progress_callback, skip_reason)
        return PreparedFrameExperiment(
            input_path=input_path,
            frame_probe=frame_probe,
            packet_size=packet_size,
            run_directory=run_directory,
            frames_directory=frames_directory,
            clean_frame_path=clean_frame_path,
            trials_path=run_directory / "trials.csv",
            summary_path=run_directory / "summary.json",
            metadata_path=run_directory / "metadata.json",
            extractor_path=extractor_path,
            total_packets=total_packets,
            lossable_packet_count=lossable_packet_count,
            packet_batch_count=0,
            trial_batch_tasks=(),
            skip_reason=skip_reason,
        )

    if lossable_packet_count == 0:
        skip_reason = (
            f"Frame {frame_index} has only {total_packets} packet(s); "
            "packet 1 is protected so there is nothing to test."
        )
        if overwrite and run_directory.exists():
            shutil.rmtree(run_directory)
        _emit_progress(progress_callback, f"Skipping frame {frame_index}: {skip_reason}")
        return PreparedFrameExperiment(
            input_path=input_path,
            frame_probe=frame_probe,
            packet_size=packet_size,
            run_directory=run_directory,
            frames_directory=frames_directory,
            clean_frame_path=clean_frame_path,
            trials_path=run_directory / "trials.csv",
            summary_path=run_directory / "summary.json",
            metadata_path=run_directory / "metadata.json",
            extractor_path=extractor_path,
            total_packets=total_packets,
            lossable_packet_count=0,
            packet_batch_count=0,
            trial_batch_tasks=(),
            skip_reason=skip_reason,
        )

    _emit_progress(progress_callback, f"Preparing output directory under {output_root}")
    run_directory = _prepare_run_directory(output_root, input_path, frame_index, overwrite)

    _emit_progress(progress_callback, "Extracting clean reference frame")
    clean_ok, clean_message = extract_frame_with_native_decoder(
        extractor_path=extractor_path,
        input_path=input_path,
        output_path=clean_frame_path,
        target_timestamp=frame_probe.timestamp,
        target_packet_offset=frame_probe.packet_offset,
        transport_packet_size=packet_size,
        corrupt_packet_index=0,
        frame_index=frame_index,
    )
    if not clean_ok:
        raise RuntimeError(
            f"Failed to extract the clean reference frame for frame_index={frame_index}.\n{clean_message}"
        )

    packet_indices = list(range(2, total_packets + 1))
    packet_batch_count = min(packet_batch_count, lossable_packet_count)
    packet_chunks = _split_contiguous_packet_indices(packet_indices, packet_batch_count)
    trial_batch_tasks = tuple(
        PacketTrialBatchTask(
            frame_index=frame_index,
            extractor_path=extractor_path,
            input_path=input_path,
            frames_directory=frames_directory,
            clean_frame_path=clean_frame_path,
            target_timestamp=frame_probe.timestamp,
            packet_offset=frame_probe.packet_offset,
            encoded_packet_size=frame_probe.packet_size,
            transport_packet_size=packet_size,
            packet_indices_1based=packet_chunk,
            total_packets=total_packets,
            lossable_packet_count=lossable_packet_count,
        )
        for packet_chunk in packet_chunks
    )

    return PreparedFrameExperiment(
        input_path=input_path,
        frame_probe=frame_probe,
        packet_size=packet_size,
        run_directory=run_directory,
        frames_directory=frames_directory,
        clean_frame_path=clean_frame_path,
        trials_path=run_directory / "trials.csv",
        summary_path=run_directory / "summary.json",
        metadata_path=run_directory / "metadata.json",
        extractor_path=extractor_path,
        total_packets=total_packets,
        lossable_packet_count=lossable_packet_count,
        packet_batch_count=packet_batch_count,
        trial_batch_tasks=trial_batch_tasks,
    )


def run_prepared_frame_packet_trials(
    prepared: PreparedFrameExperiment,
    progress_callback: ProgressCallback | None = None,
) -> list[TrialResult]:
    trial_batch_tasks = prepared.trial_batch_tasks
    if not trial_batch_tasks:
        _emit_progress(
            progress_callback,
            prepared.skip_reason or "Skipping packet trials because no lossable packets are available",
        )
        return []
    if len(trial_batch_tasks) == 1:
        _emit_progress(progress_callback, "Processing packet trials in one native batch")
        trials = _run_packet_trial_batch(trial_batch_tasks[0])
        for lossable_rank, trial in enumerate(trials, start=1):
            _emit_progress(
                progress_callback,
                (
                    f"[{lossable_rank}/{prepared.lossable_packet_count}] Packet {trial.packet_index_1based} complete: "
                    f"decode={'ok' if trial.decode_success else 'failed'}, ssim={trial.ssim:.6f}"
                ),
            )
        return trials

    _emit_progress(
        progress_callback,
        (
            f"Processing packet trials in {len(trial_batch_tasks)} native batches "
            f"with {len(trial_batch_tasks)} workers"
        ),
    )
    trials: list[TrialResult] = []
    with ProcessPoolExecutor(**_process_pool_kwargs(len(trial_batch_tasks))) as executor:
        future_to_task = {
            executor.submit(_run_packet_trial_batch, task): task
            for task in trial_batch_tasks
        }
        completed_count = 0
        for future in as_completed(future_to_task):
            task = future_to_task[future]
            try:
                batch_trials = future.result()
            except Exception as exc:
                raise RuntimeError(
                    f"Packet batch failed for packets "
                    f"{task.packet_indices_1based[0]}..{task.packet_indices_1based[-1]}: {exc}"
                ) from exc
            for trial in batch_trials:
                completed_count += 1
                _emit_progress(
                    progress_callback,
                    (
                        f"[{completed_count}/{prepared.lossable_packet_count}] "
                        f"Packet {trial.packet_index_1based} complete: "
                        f"decode={'ok' if trial.decode_success else 'failed'}, ssim={trial.ssim:.6f}"
                    ),
                )
                trials.append(trial)

    trials.sort(key=lambda trial: trial.packet_index_1based)
    return trials


def finalize_prepared_frame_experiment(
    prepared: PreparedFrameExperiment,
    trials: list[TrialResult],
    progress_callback: ProgressCallback | None = None,
    scheduler_mode: str = "per_frame_packet_batches",
) -> ExperimentArtifacts:
    trials = sorted(trials, key=lambda trial: trial.packet_index_1based)
    if prepared.skip_reason is not None:
        raise RuntimeError("Skipped frames should not be finalized")

    clean_frame_retention = "deleted_after_frame"
    corrupted_frame_retention = "deleted_after_ssim"

    _emit_progress(progress_callback, "Writing CSV and JSON outputs")
    _write_trials_csv(trials, prepared.trials_path)

    summary = _build_summary(
        input_path=prepared.input_path,
        frame_probe=prepared.frame_probe,
        packet_size=prepared.packet_size,
        clean_frame_path=prepared.clean_frame_path,
        trials=trials,
    )
    summary["clean_frame_retention"] = clean_frame_retention
    summary["experiment_status"] = "completed"
    with prepared.summary_path.open("w") as handle:
        json.dump(summary, handle, indent=2)

    metadata = {
        "input_path": str(prepared.input_path),
        "codec_name": prepared.frame_probe.codec_name,
        "frame_index": prepared.frame_probe.frame_index,
        "total_frames": prepared.frame_probe.total_frames,
        "frame_timestamp": prepared.frame_probe.timestamp,
        "frame_timestamp_seconds": prepared.frame_probe.timestamp_seconds,
        "stream_time_base": prepared.frame_probe.stream_time_base,
        "frame_packet_offset": prepared.frame_probe.packet_offset,
        "frame_packet_size_bytes": prepared.frame_probe.packet_size,
        "picture_type": prepared.frame_probe.picture_type,
        "is_key_frame": prepared.frame_probe.is_key_frame,
        "transport_packet_size_bytes": prepared.packet_size,
        "frame_output_format": FRAME_OUTPUT_EXTENSION,
        "corrupted_frame_retention": corrupted_frame_retention,
        "clean_frame_retention": clean_frame_retention,
        "ssim_mode": "native_fast_uniform_7x7",
        "packet_trial_workers": prepared.packet_batch_count,
        "native_decode_batches": len(prepared.trial_batch_tasks),
        "frame_extractor": str(prepared.extractor_path),
        "decode_mode": "native_batch_in_memory_packet_corruption",
        "scheduler_mode": scheduler_mode,
        "experiment_status": "completed",
        "skip_reason": None,
        "packet_1_is_protected": True,
        "lossable_packet_indices_1based": list(range(2, prepared.total_packets + 1)),
        "clean_frame_path": str(prepared.clean_frame_path),
        "trials_path": str(prepared.trials_path),
        "summary_path": str(prepared.summary_path),
    }
    with prepared.metadata_path.open("w") as handle:
        json.dump(metadata, handle, indent=2)

    if prepared.clean_frame_path.exists():
        _emit_progress(progress_callback, "Deleting clean reference frame")
        prepared.clean_frame_path.unlink()

    _emit_progress(progress_callback, "Experiment complete")

    return ExperimentArtifacts(
        run_directory=prepared.run_directory,
        metadata_path=prepared.metadata_path,
        trials_path=prepared.trials_path,
        summary_path=prepared.summary_path,
        clean_frame_path=prepared.clean_frame_path,
    )


def run_single_frame_experiment(
    input_path: Path,
    frame_index: int,
    packet_size: int,
    output_root: Path,
    overwrite: bool = False,
    jobs: int = 1,
    progress_callback: ProgressCallback | None = None,
    frame_probe: FrameProbe | None = None,
    extractor_path: Path | None = None,
) -> ExperimentArtifacts | None:
    if jobs <= 0:
        raise ValueError("jobs must be a positive integer")

    prepared = prepare_single_frame_experiment(
        input_path=input_path,
        frame_index=frame_index,
        packet_size=packet_size,
        output_root=output_root,
        overwrite=overwrite,
        packet_batch_count=jobs,
        progress_callback=progress_callback,
        frame_probe=frame_probe,
        extractor_path=extractor_path,
    )
    if prepared.skip_reason is not None:
        return None

    trials = run_prepared_frame_packet_trials(prepared, progress_callback)
    return finalize_prepared_frame_experiment(prepared, trials, progress_callback)


def _prepare_frame_experiment_task(task: FramePrepareTask) -> PreparedFrameExperiment:
    return prepare_single_frame_experiment(
        input_path=task.input_path,
        frame_index=task.frame_index,
        packet_size=task.packet_size,
        output_root=task.output_root,
        overwrite=task.overwrite,
        packet_batch_count=task.packet_batch_count,
        progress_callback=None,
        frame_probe=task.frame_probe,
        extractor_path=task.extractor_path,
    )


def _finalize_frame_experiment_task(task: FrameFinalizeTask) -> ExperimentArtifacts:
    return finalize_prepared_frame_experiment(
        task.prepared,
        list(task.trials),
        progress_callback=None,
        scheduler_mode=task.scheduler_mode,
    )


def run_frame_experiments_with_global_scheduler(
    input_path: Path,
    frame_indices: Iterable[int],
    packet_size: int,
    output_root: Path,
    overwrite: bool = False,
    packet_jobs: int = 1,
    frame_jobs: int = 1,
    progress_callback: ProgressCallback | None = None,
    frame_probes: Mapping[int, FrameProbe] | None = None,
    extractor_path: Path | None = None,
) -> list[ExperimentArtifacts]:
    frame_indices = tuple(frame_indices)
    if not frame_indices:
        return []
    if packet_jobs <= 0:
        raise ValueError("packet_jobs must be a positive integer")
    if frame_jobs <= 0:
        raise ValueError("frame_jobs must be a positive integer")

    input_path = input_path.resolve()
    output_root = output_root.resolve()
    if extractor_path is None:
        _emit_progress(progress_callback, "Building native frame extractor")
        extractor_path = build_native_frame_extractor()
    elif not extractor_path.exists():
        raise FileNotFoundError(f"Native frame extractor not found: {extractor_path}")

    if frame_probes is not None:
        missing_probe_indices = [frame_index for frame_index in frame_indices if frame_index not in frame_probes]
        if missing_probe_indices:
            raise KeyError(f"Missing frame probe for frame_index={missing_probe_indices[0]}")

    active_frame_limit = min(frame_jobs, len(frame_indices))
    packet_worker_count = max(1, active_frame_limit * packet_jobs)
    try:
        cpu_count = multiprocessing.cpu_count()
    except NotImplementedError:
        cpu_count = None
    pipeline_worker_count = active_frame_limit
    if cpu_count is not None:
        pipeline_worker_count = min(pipeline_worker_count, max(0, cpu_count - packet_worker_count))
    global_worker_count = max(1, packet_worker_count + pipeline_worker_count)
    pending_frame_indices = deque(frame_indices)
    preparing_frames: set[int] = set()
    active_frames: dict[int, PreparedFrameExperiment] = {}
    future_to_context: dict[Any, SchedulerFutureContext] = {}
    remaining_batches_by_frame: dict[int, int] = {}
    trials_by_frame: dict[int, list[TrialResult]] = {}
    artifacts_by_frame: dict[int, ExperimentArtifacts] = {}

    def submit_pending_prepare_tasks(executor: ProcessPoolExecutor) -> int:
        submitted_count = 0
        while pending_frame_indices and len(preparing_frames) + len(active_frames) < active_frame_limit:
            frame_index = pending_frame_indices.popleft()
            preparing_frames.add(frame_index)
            prepare_task = FramePrepareTask(
                input_path=input_path,
                frame_index=frame_index,
                packet_size=packet_size,
                output_root=output_root,
                overwrite=overwrite,
                packet_batch_count=packet_jobs,
                frame_probe=frame_probes[frame_index] if frame_probes is not None else None,
                extractor_path=extractor_path,
            )
            future = executor.submit(_prepare_frame_experiment_task, prepare_task)
            future_to_context[future] = SchedulerFutureContext(kind="prepare", frame_index=frame_index)
            _emit_progress(progress_callback, f"Preparing frame {frame_index}")
            submitted_count += 1
        return submitted_count

    def submit_packet_batches(executor: ProcessPoolExecutor, prepared: PreparedFrameExperiment) -> int:
        frame_index = prepared.frame_probe.frame_index
        if not prepared.trial_batch_tasks:
            raise RuntimeError(f"Frame {frame_index} has no packet trial batches")

        active_frames[frame_index] = prepared
        remaining_batches_by_frame[frame_index] = len(prepared.trial_batch_tasks)
        trials_by_frame[frame_index] = []
        for task in prepared.trial_batch_tasks:
            future = executor.submit(_run_packet_trial_batch, task)
            future_to_context[future] = SchedulerFutureContext(
                kind="packet",
                frame_index=frame_index,
                packet_task=task,
            )
        _emit_progress(
            progress_callback,
            (
                f"Frame {frame_index} prepared: codec={prepared.frame_probe.codec_name}, "
                f"timestamp={prepared.frame_probe.timestamp} "
                f"({prepared.frame_probe.timestamp_seconds:.3f}s), "
                f"payload={prepared.frame_probe.packet_size} bytes, "
                f"packets={prepared.total_packets}, testing={prepared.lossable_packet_count}, "
                f"submitted={len(prepared.trial_batch_tasks)} packet batch(es)"
            ),
        )
        return len(prepared.trial_batch_tasks)

    _emit_progress(
        progress_callback,
        (
            f"Processing {len(frame_indices)} frames with one global task scheduler: "
            f"{active_frame_limit} active frame(s), {packet_jobs} packet batch(es) per frame, "
            f"{packet_worker_count} packet worker(s), {pipeline_worker_count} pipeline worker(s), "
            f"{global_worker_count} total worker(s)"
        ),
    )
    with ProcessPoolExecutor(**_process_pool_kwargs(global_worker_count)) as executor:
        total_packet_batches = 0
        completed_packet_batches = 0
        completed_frames = 0
        submit_pending_prepare_tasks(executor)

        while future_to_context:
            done_futures, _ = wait(future_to_context, return_when=FIRST_COMPLETED)
            for future in done_futures:
                context = future_to_context.pop(future)
                frame_index = context.frame_index

                if context.kind == "prepare":
                    preparing_frames.remove(frame_index)
                    try:
                        prepared = future.result()
                    except Exception as exc:
                        raise RuntimeError(f"Frame {frame_index} preparation failed: {exc}") from exc
                    if prepared.skip_reason is not None:
                        completed_frames += 1
                        _emit_progress(
                            progress_callback,
                            f"Frame {frame_index} skipped ({completed_frames}/{len(frame_indices)}): "
                            f"{prepared.skip_reason}",
                        )
                        submit_pending_prepare_tasks(executor)
                        continue
                    total_packet_batches += submit_packet_batches(executor, prepared)
                    submit_pending_prepare_tasks(executor)
                    continue

                if context.kind == "packet":
                    task = context.packet_task
                    if task is None:
                        raise RuntimeError("Scheduler packet context is missing its packet task")
                    try:
                        batch_trials = future.result()
                    except Exception as exc:
                        raise RuntimeError(
                            f"Packet batch failed for frame {frame_index} "
                            f"packets {task.packet_indices_1based[0]}..{task.packet_indices_1based[-1]}: {exc}"
                        ) from exc

                    completed_packet_batches += 1
                    trials_by_frame[frame_index].extend(batch_trials)
                    remaining_batches_by_frame[frame_index] -= 1
                    _emit_progress(
                        progress_callback,
                        (
                            f"[{completed_packet_batches}/{total_packet_batches}] Frame {frame_index} "
                            f"packets {task.packet_indices_1based[0]}..{task.packet_indices_1based[-1]} complete"
                        ),
                    )

                    if remaining_batches_by_frame[frame_index] == 0:
                        prepared = active_frames.pop(frame_index)
                        finalize_task = FrameFinalizeTask(
                            prepared=prepared,
                            trials=tuple(trials_by_frame[frame_index]),
                            scheduler_mode="global_task_graph",
                        )
                        finalize_future = executor.submit(_finalize_frame_experiment_task, finalize_task)
                        future_to_context[finalize_future] = SchedulerFutureContext(
                            kind="finalize",
                            frame_index=frame_index,
                        )
                        del remaining_batches_by_frame[frame_index]
                        del trials_by_frame[frame_index]
                        _emit_progress(progress_callback, f"Finalizing frame {frame_index}")
                        submit_pending_prepare_tasks(executor)
                    continue

                if context.kind == "finalize":
                    try:
                        artifacts_by_frame[frame_index] = future.result()
                    except Exception as exc:
                        raise RuntimeError(f"Frame {frame_index} finalization failed") from exc
                    completed_frames += 1
                    _emit_progress(
                        progress_callback,
                        f"Frame {frame_index} complete ({completed_frames}/{len(frame_indices)})",
                    )
                    submit_pending_prepare_tasks(executor)
                    continue

                raise RuntimeError(f"Unknown scheduler future kind: {context.kind}")

    return [
        artifacts_by_frame[frame_index]
        for frame_index in frame_indices
        if frame_index in artifacts_by_frame
    ]
