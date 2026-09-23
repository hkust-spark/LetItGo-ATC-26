from __future__ import annotations

from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import time
from typing import Iterator, NoReturn, Sequence


class NativeToolError(RuntimeError):
    """Raised when the native frame extractor cannot be built or executed."""


PRE_GUESS_DC_UNAVAILABLE = -(2**31)


@dataclass(frozen=True)
class NativeErrorBlockResult:
    success: bool
    block_unit: str
    block_width: int | None
    block_height: int | None
    total_blocks: int | None
    damaged_blocks: int | None
    concealed_blocks: int | None
    inter_concealed_blocks: int | None
    damaged_block_ratio: float | None
    decode_error_flags: int
    frame_corrupt: bool
    message: str
    recovery_total_mbs: int | None = None
    recovery_decoded_ok_mbs: int | None = None
    recovery_mv_guessed_mbs: int | None = None
    recovery_spatial_concealed_mbs: int | None = None
    recovery_reference_copied_mbs: int | None = None
    recovery_failed_or_unclassified_mbs: int | None = None


@dataclass(frozen=True)
class NativeMotionVector:
    packet_index: int
    success: bool
    frame_timestamp: int
    source: int
    dst_x: int
    dst_y: int
    mb_x: int
    mb_y: int
    motion_x: int
    motion_y: int
    motion_scale: int
    src_x: int
    src_y: int
    source_ref: int
    w: int
    h: int
    flags: int
    er_origin_mb_x: int | None
    er_origin_mb_y: int | None
    er_total_blocks: int | None
    er_damaged_mb_count: int | None
    manhattan_to_origin: int | None
    decode_error_flags: int
    frame_corrupt: bool


@dataclass(frozen=True)
class NativeGuessDcTruth:
    packet_index: int
    success: bool
    frame_timestamp: int
    component: int
    block_x: int
    block_y: int
    mb_x: int
    mb_y: int
    clean_dc: int


@dataclass(frozen=True)
class NativeGuessDcRecord:
    packet_index: int
    success: bool
    frame_timestamp: int
    component: int
    block_x: int
    block_y: int
    mb_x: int
    mb_y: int
    dist0: int
    dist1: int
    dist2: int
    dist3: int
    weight0: int
    weight1: int
    weight2: int
    weight3: int
    boundary_dc0: int
    boundary_dc1: int
    boundary_dc2: int
    boundary_dc3: int
    guess_dc: int
    pre_guess_dc: int | None


@dataclass(frozen=True)
class NativeGuessMvTruth:
    packet_index: int
    success: bool
    frame_timestamp: int
    mb_x: int
    mb_y: int
    truth_mv_x: int
    truth_mv_y: int
    truth_ref: int
    motion_scale: int


@dataclass(frozen=True)
class NativeGuessMvRecord:
    packet_index: int
    success: bool
    frame_timestamp: int
    mb_x: int
    mb_y: int
    hop_count: int
    pass_index: int
    pred_count: int
    estimated_mv_x: int
    estimated_mv_y: int
    estimated_ref: int
    best_score: int


@dataclass(frozen=True)
class NativeGuessDcBatchDiagnostics:
    start_packet_index: int
    stop_packet_index: int
    packet_count: int
    elapsed_seconds: float
    returncode: int
    stdout_bytes: int
    stderr_bytes: int
    stdout_line_count: int
    parsed_record_count: int
    successful_record_count: int
    reported_packet_count: int
    stderr_tail: str


@dataclass(frozen=True)
class NativeGuessMvBatchDiagnostics:
    start_packet_index: int
    stop_packet_index: int
    packet_count: int
    elapsed_seconds: float
    returncode: int
    stdout_bytes: int
    stderr_bytes: int
    stdout_line_count: int
    parsed_record_count: int
    successful_record_count: int
    reported_packet_count: int
    stderr_tail: str


@dataclass(frozen=True)
class NativeGuessDcBatchResult:
    records_by_packet: dict[int, list[NativeGuessDcRecord]]
    diagnostics: NativeGuessDcBatchDiagnostics


@dataclass(frozen=True)
class NativeGuessDcBatchFileResult:
    output_path: Path
    stderr_path: Path
    diagnostics: NativeGuessDcBatchDiagnostics


@dataclass(frozen=True)
class NativeGuessMvBatchFileResult:
    output_path: Path
    stderr_path: Path
    diagnostics: NativeGuessMvBatchDiagnostics


def _native_dir() -> Path:
    return Path(__file__).resolve().parents[1] / "native"


def _local_ffmpeg_library_path() -> str | None:
    configured_path = os.environ.get("PACKET_LOSS_FFMPEG_LIBRARY_PATH")
    if configured_path:
        return configured_path

    ffmpeg_prefix = os.environ.get("FFMPEG_PREFIX")
    if not ffmpeg_prefix:
        return None

    ffmpeg_root = Path(ffmpeg_prefix).expanduser()
    library_dirs: list[str] = []
    for directory_name in ("lib", "lib64"):
        library_dir = ffmpeg_root / directory_name
        if any(library_dir.glob("*.so*")):
            library_dirs.append(str(library_dir))

    return ":".join(library_dirs) if library_dirs else None


def _native_error_block_env() -> dict[str, str]:
    env = os.environ.copy()
    ffmpeg_library_path = _local_ffmpeg_library_path()
    if ffmpeg_library_path:
        existing_library_path = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = (
            f"{ffmpeg_library_path}:{existing_library_path}"
            if existing_library_path
            else ffmpeg_library_path
        )
    return env


def _native_exit_signal(returncode: int | None) -> str | None:
    if returncode is not None and returncode < 0:
        try:
            return signal.Signals(-returncode).name
        except ValueError:
            pass
    return None


def _raise_native_error_with_report(
    report_path: Path, report: dict[str, object], message: str,
) -> NoReturn:
    report_path = report_path.resolve()
    try:
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except OSError as exc:
        raise NativeToolError(
            f"{message}\nCould not write error report {report_path}: {exc}\n"
            f"{str(report['stderr']).strip()}"
        ) from exc
    raise NativeToolError(f"{message}\nError report: {report_path}")


def build_native_frame_extractor() -> Path:
    native_dir = _native_dir()
    binary_path = native_dir / "frame_extract"
    completed = subprocess.run(
        ["make", "-C", str(native_dir), "frame_extract"],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise NativeToolError(
            "Failed to build the native frame extractor.\n"
            f"{completed.stdout.strip()}\n{completed.stderr.strip()}".strip()
        )
    if not binary_path.exists():
        raise NativeToolError(f"Native frame extractor was not produced at {binary_path}")
    return binary_path


def extract_frame_with_native_decoder(
    *,
    extractor_path: Path,
    input_path: Path,
    output_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    corrupt_packet_index: int = 0,
    frame_index: int | None = None,
) -> tuple[bool, str]:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        output_path.unlink()

    command = [
        str(extractor_path),
        str(input_path),
        str(output_path),
        str(target_timestamp),
        str(target_packet_offset),
        str(transport_packet_size),
        str(corrupt_packet_index),
    ]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=False,
            env=_native_error_block_env(),
        )
    except OSError as exc:
        if corrupt_packet_index != 0:
            raise
        returncode = None
        stdout = ""
        stderr = str(exc)
    else:
        returncode = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr

    output_exists = output_path.exists() and output_path.stat().st_size > 0
    success = returncode == 0 and output_exists
    if corrupt_packet_index == 0 and not success:
        signal_name = _native_exit_signal(returncode)
        error = "launch_error" if returncode is None else (
            "nonzero_exit" if returncode != 0 else "missing_output"
        )
        report = {
            "stage": "clean_reference",
            "error": error,
            "command": command,
            "working_directory": str(Path.cwd()),
            "input_path": str(input_path),
            "output_path": str(output_path),
            "frame_index": frame_index,
            "target_timestamp": target_timestamp,
            "target_packet_offset": target_packet_offset,
            "transport_packet_size": transport_packet_size,
            "corrupt_packet_index": corrupt_packet_index,
            "returncode": returncode,
            "signal": signal_name,
            "output_exists": output_exists,
            "stdout": stdout,
            "stderr": stderr,
        }
        detail = "could not start native process" if returncode is None else f"exit code {returncode}"
        if signal_name is not None:
            detail += f" ({signal_name})"
        if not output_exists:
            detail += "; no nonempty reference frame was produced"
        _raise_native_error_with_report(
            output_path.parent / "native_reference_error.json",
            report,
            f"Native clean reference extraction failed: {detail}.",
        )

    message = "\n".join(
        part.strip()
        for part in (stdout, stderr)
        if part and part.strip()
    )

    if not output_exists and output_path.exists():
        output_path.unlink()

    return success, message


def extract_frames_with_native_decoder_batch(
    *,
    extractor_path: Path,
    input_path: Path,
    output_dir: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    corrupt_packet_indices: Sequence[int],
    output_extension: str = "png",
    ssim_reference_path: Path | None = None,
) -> dict[int, tuple[bool, str, float | None]]:
    if not corrupt_packet_indices:
        return {}
    if output_extension not in {"png", "ppm"}:
        raise ValueError("output_extension must be 'png' or 'ppm'")
    if ssim_reference_path is not None and output_extension != "ppm":
        raise ValueError("Native SSIM calculation requires PPM output")

    output_dir.mkdir(parents=True, exist_ok=True)
    start_packet_index = min(corrupt_packet_indices)
    stop_packet_index = max(corrupt_packet_indices)
    expected_packet_indices = set(range(start_packet_index, stop_packet_index + 1))
    if set(corrupt_packet_indices) != expected_packet_indices:
        raise ValueError("Native batch decode requires a contiguous packet-index range")

    for packet_index in corrupt_packet_indices:
        output_path = output_dir / f"packet_{packet_index:04d}.{output_extension}"
        if output_path.exists():
            output_path.unlink()

    command = [
        str(extractor_path),
        "--batch",
        str(input_path),
        str(output_dir),
        str(target_timestamp),
        str(target_packet_offset),
        str(transport_packet_size),
        str(start_packet_index),
        str(stop_packet_index),
        output_extension,
    ]
    if ssim_reference_path is not None:
        command.append(str(ssim_reference_path))

    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
        env=_native_error_block_env(),
    )

    results: dict[int, tuple[bool, str, float | None]] = {}
    for line in completed.stdout.splitlines():
        if not line.strip() or line.startswith("packet_index\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        try:
            packet_index = int(parts[0])
        except ValueError:
            continue
        output_path = Path(parts[2])
        output_exists = output_path.exists() and output_path.stat().st_size > 0
        success = parts[1] == "1" and output_exists
        ssim_value = None
        if success and len(parts) >= 4 and parts[3] != "nan":
            try:
                ssim_value = float(parts[3])
            except ValueError:
                ssim_value = None
        if not output_exists and output_path.exists():
            output_path.unlink()
        results[packet_index] = (
            success,
            "" if success else f"Native batch decode failed for packet {packet_index}",
            ssim_value,
        )

    missing_packet_indices = sorted(expected_packet_indices - results.keys())
    if completed.returncode != 0 or missing_packet_indices:
        signal_name = _native_exit_signal(completed.returncode)
        report_path = (
            output_dir / f"native_batch_error_{start_packet_index:04d}_{stop_packet_index:04d}.json"
        ).resolve()
        report = {
            "error": "nonzero_exit" if completed.returncode != 0 else "incomplete_output",
            "command": command,
            "working_directory": str(Path.cwd()),
            "input_path": str(input_path),
            "returncode": completed.returncode,
            "signal": signal_name,
            "target_timestamp": target_timestamp,
            "target_packet_offset": target_packet_offset,
            "transport_packet_size": transport_packet_size,
            "expected_packet_indices": sorted(expected_packet_indices),
            "reported_packet_indices": sorted(results),
            "missing_packet_indices": missing_packet_indices,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
        }
        detail = f"exit code {completed.returncode}"
        if signal_name is not None:
            detail += f" ({signal_name})"
        if missing_packet_indices:
            detail += f"; missing packet records: {missing_packet_indices}"
        _raise_native_error_with_report(
            report_path, report, f"Native batch frame extraction failed: {detail}.",
        )

    return results


def _parse_optional_int(value: str) -> int | None:
    if value == "" or value == "-1":
        return None
    try:
        return int(value)
    except ValueError:
        return None


def _parse_optional_float(value: str) -> float | None:
    if value == "" or value.lower() == "nan":
        return None
    try:
        parsed = float(value)
    except ValueError:
        return None
    if math.isnan(parsed):
        return None
    return parsed


def _missing_error_block_result(packet_index: int, message: str) -> NativeErrorBlockResult:
    formatted_message = message.replace("{packet_index}", str(packet_index))
    return NativeErrorBlockResult(
        success=False,
        block_unit="unsupported",
        block_width=None,
        block_height=None,
        total_blocks=None,
        damaged_blocks=None,
        concealed_blocks=None,
        inter_concealed_blocks=None,
        damaged_block_ratio=None,
        decode_error_flags=0,
        frame_corrupt=False,
        message=formatted_message,
        recovery_total_mbs=None,
        recovery_decoded_ok_mbs=None,
        recovery_mv_guessed_mbs=None,
        recovery_spatial_concealed_mbs=None,
        recovery_reference_copied_mbs=None,
        recovery_failed_or_unclassified_mbs=None,
    )


def _native_error_block_command(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    start_packet_index: int,
    stop_packet_index: int,
) -> list[str]:
    return [
        str(extractor_path),
        "--error-blocks",
        str(input_path),
        str(target_timestamp),
        str(target_packet_offset),
        str(transport_packet_size),
        str(start_packet_index),
        str(stop_packet_index),
    ]


def _parse_native_error_block_stdout(stdout: str) -> dict[int, NativeErrorBlockResult]:
    results: dict[int, NativeErrorBlockResult] = {}
    for line in stdout.splitlines():
        if not line.strip() or line.startswith("packet_index\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 12:
            continue
        try:
            packet_index = int(parts[0])
        except ValueError:
            continue

        success = parts[1] == "1"
        decode_error_flags = _parse_optional_int(parts[10]) or 0
        frame_corrupt = parts[11] == "1"
        block_unit = parts[2] or "unsupported"
        if block_unit == "unsupported":
            block_unit = "unsupported"

        results[packet_index] = NativeErrorBlockResult(
            success=success,
            block_unit=block_unit,
            block_width=_parse_optional_int(parts[3]),
            block_height=_parse_optional_int(parts[4]),
            total_blocks=_parse_optional_int(parts[5]),
            damaged_blocks=_parse_optional_int(parts[6]),
            concealed_blocks=_parse_optional_int(parts[7]),
            inter_concealed_blocks=_parse_optional_int(parts[8]),
            damaged_block_ratio=_parse_optional_float(parts[9]),
            decode_error_flags=decode_error_flags,
            frame_corrupt=frame_corrupt,
            message="" if success else f"Native error-block decode failed for packet {packet_index}",
            recovery_total_mbs=_parse_optional_int(parts[12]) if len(parts) > 12 else None,
            recovery_decoded_ok_mbs=_parse_optional_int(parts[13]) if len(parts) > 13 else None,
            recovery_mv_guessed_mbs=_parse_optional_int(parts[14]) if len(parts) > 14 else None,
            recovery_spatial_concealed_mbs=_parse_optional_int(parts[15]) if len(parts) > 15 else None,
            recovery_reference_copied_mbs=_parse_optional_int(parts[16]) if len(parts) > 16 else None,
            recovery_failed_or_unclassified_mbs=_parse_optional_int(parts[17]) if len(parts) > 17 else None,
        )
    return results


def _native_error_block_failure_result(
    packet_index: int,
    completed: subprocess.CompletedProcess[str],
) -> NativeErrorBlockResult:
    stderr_message = completed.stderr.strip()
    stdout_message = completed.stdout.strip()
    message = (
        "Native error-block decode crashed for packet {packet_index}"
        f" with exit code {completed.returncode}."
    )
    details = "\n".join(part for part in (stdout_message, stderr_message) if part)
    if details:
        message = f"{message}\n{details}"
    return _missing_error_block_result(packet_index, message)


def _run_native_error_block_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
        env=_native_error_block_env(),
    )


def extract_error_blocks_with_native_decoder_batch(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    corrupt_packet_indices: Sequence[int],
) -> dict[int, NativeErrorBlockResult]:
    if not corrupt_packet_indices:
        return {}

    start_packet_index = min(corrupt_packet_indices)
    stop_packet_index = max(corrupt_packet_indices)
    expected_packet_indices = set(range(start_packet_index, stop_packet_index + 1))
    if set(corrupt_packet_indices) != expected_packet_indices:
        raise ValueError("Native error-block batch decode requires a contiguous packet-index range")

    command = _native_error_block_command(
        extractor_path=extractor_path,
        input_path=input_path,
        target_timestamp=target_timestamp,
        target_packet_offset=target_packet_offset,
        transport_packet_size=transport_packet_size,
        start_packet_index=start_packet_index,
        stop_packet_index=stop_packet_index,
    )
    completed = _run_native_error_block_command(command)

    results = _parse_native_error_block_stdout(completed.stdout)

    if completed.returncode != 0:
        for packet_index in corrupt_packet_indices:
            if packet_index in results:
                continue
            single_command = _native_error_block_command(
                extractor_path=extractor_path,
                input_path=input_path,
                target_timestamp=target_timestamp,
                target_packet_offset=target_packet_offset,
                transport_packet_size=transport_packet_size,
                start_packet_index=packet_index,
                stop_packet_index=packet_index,
            )
            single_completed = _run_native_error_block_command(single_command)
            single_results = _parse_native_error_block_stdout(single_completed.stdout)
            if packet_index in single_results:
                results[packet_index] = single_results[packet_index]
            else:
                results[packet_index] = _native_error_block_failure_result(packet_index, single_completed)

    for packet_index in corrupt_packet_indices:
        if packet_index not in results:
            results[packet_index] = _missing_error_block_result(
                packet_index,
                "Native error-block batch decode did not report packet {packet_index}",
            )

    return results


def _parse_required_int(value: str, field_name: str) -> int:
    try:
        return int(value)
    except ValueError as exc:
        raise NativeToolError(f"Invalid integer in native dump field {field_name}: {value}") from exc


def _parse_pre_guess_dc(value: str) -> int | None:
    parsed = _parse_required_int(value, "pre_guess_dc")
    if parsed == PRE_GUESS_DC_UNAVAILABLE:
        return None
    return parsed


def extract_motion_vectors_with_native_decoder_batch(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    corrupt_packet_indices: Sequence[int],
) -> dict[int, list[NativeMotionVector]]:
    if not corrupt_packet_indices:
        return {}

    start_packet_index = min(corrupt_packet_indices)
    stop_packet_index = max(corrupt_packet_indices)
    expected_packet_indices = set(range(start_packet_index, stop_packet_index + 1))
    clean_decode_requested = start_packet_index == 0 and stop_packet_index == 0
    if not clean_decode_requested and set(corrupt_packet_indices) != expected_packet_indices:
        raise ValueError("Native MV batch decode requires a contiguous packet-index range")
    if start_packet_index == 0 and stop_packet_index != 0:
        raise ValueError("Native MV clean decode must use only packet index 0")

    command = [
        str(extractor_path),
        "--mv-dump",
        str(input_path),
        str(target_timestamp),
        str(target_packet_offset),
        str(transport_packet_size),
        str(start_packet_index),
        str(stop_packet_index),
    ]
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
        env=_native_error_block_env(),
    )

    stderr_message = completed.stderr.strip()
    results: dict[int, list[NativeMotionVector]] = {
        packet_index: [] for packet_index in corrupt_packet_indices
    }
    for line in completed.stdout.splitlines():
        if not line.strip() or line.startswith("packet_index\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 24:
            continue

        packet_index = _parse_required_int(parts[0], "packet_index")
        if packet_index not in results:
            results[packet_index] = []
        results[packet_index].append(
            NativeMotionVector(
                packet_index=packet_index,
                success=parts[1] == "1",
                frame_timestamp=_parse_required_int(parts[2], "frame_timestamp"),
                source=_parse_required_int(parts[3], "source"),
                dst_x=_parse_required_int(parts[4], "dst_x"),
                dst_y=_parse_required_int(parts[5], "dst_y"),
                mb_x=_parse_required_int(parts[6], "mb_x"),
                mb_y=_parse_required_int(parts[7], "mb_y"),
                motion_x=_parse_required_int(parts[8], "motion_x"),
                motion_y=_parse_required_int(parts[9], "motion_y"),
                motion_scale=_parse_required_int(parts[10], "motion_scale"),
                src_x=_parse_required_int(parts[11], "src_x"),
                src_y=_parse_required_int(parts[12], "src_y"),
                source_ref=_parse_required_int(parts[13], "source_ref"),
                w=_parse_required_int(parts[14], "w"),
                h=_parse_required_int(parts[15], "h"),
                flags=_parse_required_int(parts[16], "flags"),
                er_origin_mb_x=_parse_optional_int(parts[17]),
                er_origin_mb_y=_parse_optional_int(parts[18]),
                er_total_blocks=_parse_optional_int(parts[19]),
                er_damaged_mb_count=_parse_optional_int(parts[20]),
                manhattan_to_origin=_parse_optional_int(parts[21]),
                decode_error_flags=_parse_optional_int(parts[22]) or 0,
                frame_corrupt=parts[23] == "1",
            )
        )

    if completed.returncode != 0 and not any(results.values()):
        raise NativeToolError(
            "Native MV dump failed.\n"
            f"{completed.stdout.strip()}\n{stderr_message}".strip()
        )

    return results


def _guess_dc_command(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    start_packet_index: int,
    stop_packet_index: int,
) -> list[str]:
    return [
        str(extractor_path),
        "--guess-dc-dump",
        str(input_path),
        str(target_timestamp),
        str(target_packet_offset),
        str(transport_packet_size),
        str(start_packet_index),
        str(stop_packet_index),
    ]


def extract_guess_dc_truth_with_native_decoder(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
) -> list[NativeGuessDcTruth]:
    command = _guess_dc_command(
        extractor_path=extractor_path,
        input_path=input_path,
        target_timestamp=target_timestamp,
        target_packet_offset=target_packet_offset,
        transport_packet_size=transport_packet_size,
        start_packet_index=0,
        stop_packet_index=0,
    )
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
        env=_native_error_block_env(),
    )

    rows: list[NativeGuessDcTruth] = []
    for line in completed.stdout.splitlines():
        if not line.strip() or line.startswith("packet_index\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 9:
            continue
        rows.append(
            NativeGuessDcTruth(
                packet_index=_parse_required_int(parts[0], "packet_index"),
                success=parts[1] == "1",
                frame_timestamp=_parse_required_int(parts[2], "frame_timestamp"),
                component=_parse_required_int(parts[3], "component"),
                block_x=_parse_required_int(parts[4], "block_x"),
                block_y=_parse_required_int(parts[5], "block_y"),
                mb_x=_parse_required_int(parts[6], "mb_x"),
                mb_y=_parse_required_int(parts[7], "mb_y"),
                clean_dc=_parse_required_int(parts[8], "clean_dc"),
            )
        )

    if completed.returncode != 0 and not rows:
        raise NativeToolError(
            "Native guess_dc clean dump failed.\n"
            f"{completed.stdout.strip()}\n{completed.stderr.strip()}".strip()
        )
    return rows


def extract_guess_dc_records_with_native_decoder_batch(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    corrupt_packet_indices: Sequence[int],
) -> dict[int, list[NativeGuessDcRecord]]:
    return extract_guess_dc_records_with_native_decoder_batch_with_diagnostics(
        extractor_path=extractor_path,
        input_path=input_path,
        target_timestamp=target_timestamp,
        target_packet_offset=target_packet_offset,
        transport_packet_size=transport_packet_size,
        corrupt_packet_indices=corrupt_packet_indices,
    ).records_by_packet


def extract_guess_dc_records_with_native_decoder_batch_with_diagnostics(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    corrupt_packet_indices: Sequence[int],
) -> NativeGuessDcBatchResult:
    if not corrupt_packet_indices:
        diagnostics = NativeGuessDcBatchDiagnostics(
            start_packet_index=0,
            stop_packet_index=0,
            packet_count=0,
            elapsed_seconds=0.0,
            returncode=0,
            stdout_bytes=0,
            stderr_bytes=0,
            stdout_line_count=0,
            parsed_record_count=0,
            successful_record_count=0,
            reported_packet_count=0,
            stderr_tail="",
        )
        return NativeGuessDcBatchResult(records_by_packet={}, diagnostics=diagnostics)

    start_packet_index = min(corrupt_packet_indices)
    stop_packet_index = max(corrupt_packet_indices)
    expected_packet_indices = set(range(start_packet_index, stop_packet_index + 1))
    if start_packet_index < 2:
        raise ValueError("Native guess_dc corrupt dump must start at packet index 2 or later")
    if set(corrupt_packet_indices) != expected_packet_indices:
        raise ValueError("Native guess_dc batch decode requires a contiguous packet-index range")

    command = _guess_dc_command(
        extractor_path=extractor_path,
        input_path=input_path,
        target_timestamp=target_timestamp,
        target_packet_offset=target_packet_offset,
        transport_packet_size=transport_packet_size,
        start_packet_index=start_packet_index,
        stop_packet_index=stop_packet_index,
    )
    start_time = time.monotonic()
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
        env=_native_error_block_env(),
    )
    elapsed_seconds = time.monotonic() - start_time

    results: dict[int, list[NativeGuessDcRecord]] = {
        packet_index: [] for packet_index in corrupt_packet_indices
    }
    for line in completed.stdout.splitlines():
        if not line.strip() or line.startswith("packet_index\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 22:
            continue
        packet_index = _parse_required_int(parts[0], "packet_index")
        if packet_index not in results:
            results[packet_index] = []
        results[packet_index].append(
            NativeGuessDcRecord(
                packet_index=packet_index,
                success=parts[1] == "1",
                frame_timestamp=_parse_required_int(parts[2], "frame_timestamp"),
                component=_parse_required_int(parts[3], "component"),
                block_x=_parse_required_int(parts[4], "block_x"),
                block_y=_parse_required_int(parts[5], "block_y"),
                mb_x=_parse_required_int(parts[6], "mb_x"),
                mb_y=_parse_required_int(parts[7], "mb_y"),
                dist0=_parse_required_int(parts[8], "dist0"),
                dist1=_parse_required_int(parts[9], "dist1"),
                dist2=_parse_required_int(parts[10], "dist2"),
                dist3=_parse_required_int(parts[11], "dist3"),
                weight0=_parse_required_int(parts[12], "weight0"),
                weight1=_parse_required_int(parts[13], "weight1"),
                weight2=_parse_required_int(parts[14], "weight2"),
                weight3=_parse_required_int(parts[15], "weight3"),
                boundary_dc0=_parse_required_int(parts[16], "boundary_dc0"),
                boundary_dc1=_parse_required_int(parts[17], "boundary_dc1"),
                boundary_dc2=_parse_required_int(parts[18], "boundary_dc2"),
                boundary_dc3=_parse_required_int(parts[19], "boundary_dc3"),
                guess_dc=_parse_required_int(parts[20], "guess_dc"),
                pre_guess_dc=_parse_pre_guess_dc(parts[21]),
            )
        )

    parsed_record_count = sum(len(records) for records in results.values())
    successful_record_count = sum(
        1
        for records in results.values()
        for record in records
        if record.success
    )
    diagnostics = NativeGuessDcBatchDiagnostics(
        start_packet_index=start_packet_index,
        stop_packet_index=stop_packet_index,
        packet_count=len(corrupt_packet_indices),
        elapsed_seconds=elapsed_seconds,
        returncode=completed.returncode,
        stdout_bytes=len(completed.stdout.encode("utf-8", errors="surrogateescape")),
        stderr_bytes=len(completed.stderr.encode("utf-8", errors="surrogateescape")),
        stdout_line_count=len(completed.stdout.splitlines()),
        parsed_record_count=parsed_record_count,
        successful_record_count=successful_record_count,
        reported_packet_count=sum(1 for records in results.values() if records),
        stderr_tail=completed.stderr[-4000:],
    )

    if completed.returncode != 0 and not any(results.values()):
        raise NativeToolError(
            "Native guess_dc corrupt dump failed.\n"
            f"{completed.stdout.strip()}\n{completed.stderr.strip()}".strip()
        )
    return NativeGuessDcBatchResult(records_by_packet=results, diagnostics=diagnostics)


def iter_guess_dc_records_from_native_dump(path: Path) -> Iterator[NativeGuessDcRecord]:
    with path.open() as input_file:
        for line in input_file:
            if not line.strip() or line.startswith("packet_index\t"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 22:
                continue
            yield NativeGuessDcRecord(
                packet_index=_parse_required_int(parts[0], "packet_index"),
                success=parts[1] == "1",
                frame_timestamp=_parse_required_int(parts[2], "frame_timestamp"),
                component=_parse_required_int(parts[3], "component"),
                block_x=_parse_required_int(parts[4], "block_x"),
                block_y=_parse_required_int(parts[5], "block_y"),
                mb_x=_parse_required_int(parts[6], "mb_x"),
                mb_y=_parse_required_int(parts[7], "mb_y"),
                dist0=_parse_required_int(parts[8], "dist0"),
                dist1=_parse_required_int(parts[9], "dist1"),
                dist2=_parse_required_int(parts[10], "dist2"),
                dist3=_parse_required_int(parts[11], "dist3"),
                weight0=_parse_required_int(parts[12], "weight0"),
                weight1=_parse_required_int(parts[13], "weight1"),
                weight2=_parse_required_int(parts[14], "weight2"),
                weight3=_parse_required_int(parts[15], "weight3"),
                boundary_dc0=_parse_required_int(parts[16], "boundary_dc0"),
                boundary_dc1=_parse_required_int(parts[17], "boundary_dc1"),
                boundary_dc2=_parse_required_int(parts[18], "boundary_dc2"),
                boundary_dc3=_parse_required_int(parts[19], "boundary_dc3"),
                guess_dc=_parse_required_int(parts[20], "guess_dc"),
                pre_guess_dc=_parse_pre_guess_dc(parts[21]),
            )


def _file_tail(path: Path, byte_count: int = 4000) -> str:
    try:
        with path.open("rb") as input_file:
            input_file.seek(0, os.SEEK_END)
            size = input_file.tell()
            input_file.seek(max(0, size - byte_count), os.SEEK_SET)
            return input_file.read().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def dump_guess_dc_records_with_native_decoder_batch_to_file(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    corrupt_packet_indices: Sequence[int],
    output_path: Path,
    stderr_path: Path,
) -> NativeGuessDcBatchFileResult:
    if not corrupt_packet_indices:
        diagnostics = NativeGuessDcBatchDiagnostics(
            start_packet_index=0,
            stop_packet_index=0,
            packet_count=0,
            elapsed_seconds=0.0,
            returncode=0,
            stdout_bytes=0,
            stderr_bytes=0,
            stdout_line_count=0,
            parsed_record_count=0,
            successful_record_count=0,
            reported_packet_count=0,
            stderr_tail="",
        )
        return NativeGuessDcBatchFileResult(
            output_path=output_path,
            stderr_path=stderr_path,
            diagnostics=diagnostics,
        )

    start_packet_index = min(corrupt_packet_indices)
    stop_packet_index = max(corrupt_packet_indices)
    expected_packet_indices = set(range(start_packet_index, stop_packet_index + 1))
    if start_packet_index < 2:
        raise ValueError("Native guess_dc corrupt dump must start at packet index 2 or later")
    if set(corrupt_packet_indices) != expected_packet_indices:
        raise ValueError("Native guess_dc batch decode requires a contiguous packet-index range")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        output_path.unlink()
    if stderr_path.exists():
        stderr_path.unlink()

    command = _guess_dc_command(
        extractor_path=extractor_path,
        input_path=input_path,
        target_timestamp=target_timestamp,
        target_packet_offset=target_packet_offset,
        transport_packet_size=transport_packet_size,
        start_packet_index=start_packet_index,
        stop_packet_index=stop_packet_index,
    )
    start_time = time.monotonic()
    with output_path.open("w") as stdout_file, stderr_path.open("w") as stderr_file:
        completed = subprocess.run(
            command,
            stdout=stdout_file,
            stderr=stderr_file,
            text=True,
            check=False,
            env=_native_error_block_env(),
        )
    elapsed_seconds = time.monotonic() - start_time

    stdout_line_count = 0
    parsed_record_count = 0
    successful_record_count = 0
    reported_packet_indices: set[int] = set()
    for line in output_path.open():
        if not line.strip():
            continue
        stdout_line_count += 1
        if line.startswith("packet_index\t"):
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 22:
            continue
        packet_index = _parse_required_int(parts[0], "packet_index")
        parsed_record_count += 1
        reported_packet_indices.add(packet_index)
        if parts[1] == "1":
            successful_record_count += 1

    diagnostics = NativeGuessDcBatchDiagnostics(
        start_packet_index=start_packet_index,
        stop_packet_index=stop_packet_index,
        packet_count=len(corrupt_packet_indices),
        elapsed_seconds=elapsed_seconds,
        returncode=completed.returncode,
        stdout_bytes=output_path.stat().st_size if output_path.exists() else 0,
        stderr_bytes=stderr_path.stat().st_size if stderr_path.exists() else 0,
        stdout_line_count=stdout_line_count,
        parsed_record_count=parsed_record_count,
        successful_record_count=successful_record_count,
        reported_packet_count=len(reported_packet_indices),
        stderr_tail=_file_tail(stderr_path),
    )

    if completed.returncode != 0 and parsed_record_count == 0:
        raise NativeToolError(
            "Native guess_dc corrupt dump failed.\n"
            f"{_file_tail(output_path)}\n{diagnostics.stderr_tail}".strip()
        )

    return NativeGuessDcBatchFileResult(
        output_path=output_path,
        stderr_path=stderr_path,
        diagnostics=diagnostics,
    )


def _guess_mv_command(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    start_packet_index: int,
    stop_packet_index: int,
) -> list[str]:
    return [
        str(extractor_path),
        "--guess-mv-dump",
        str(input_path),
        str(target_timestamp),
        str(target_packet_offset),
        str(transport_packet_size),
        str(start_packet_index),
        str(stop_packet_index),
    ]


def extract_guess_mv_truth_with_native_decoder(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
) -> list[NativeGuessMvTruth]:
    command = _guess_mv_command(
        extractor_path=extractor_path,
        input_path=input_path,
        target_timestamp=target_timestamp,
        target_packet_offset=target_packet_offset,
        transport_packet_size=transport_packet_size,
        start_packet_index=0,
        stop_packet_index=0,
    )
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
        env=_native_error_block_env(),
    )

    rows: list[NativeGuessMvTruth] = []
    for line in completed.stdout.splitlines():
        if not line.strip() or line.startswith("packet_index\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 9:
            continue
        rows.append(
            NativeGuessMvTruth(
                packet_index=_parse_required_int(parts[0], "packet_index"),
                success=parts[1] == "1",
                frame_timestamp=_parse_required_int(parts[2], "frame_timestamp"),
                mb_x=_parse_required_int(parts[3], "mb_x"),
                mb_y=_parse_required_int(parts[4], "mb_y"),
                truth_mv_x=_parse_required_int(parts[5], "truth_mv_x"),
                truth_mv_y=_parse_required_int(parts[6], "truth_mv_y"),
                truth_ref=_parse_required_int(parts[7], "truth_ref"),
                motion_scale=_parse_required_int(parts[8], "motion_scale"),
            )
        )

    if completed.returncode != 0 and not rows:
        raise NativeToolError(
            "Native guess_mv clean dump failed.\n"
            f"{completed.stdout.strip()}\n{completed.stderr.strip()}".strip()
        )
    return rows


def iter_guess_mv_records_from_native_dump(path: Path) -> Iterator[NativeGuessMvRecord]:
    with path.open() as input_file:
        for line in input_file:
            if not line.strip() or line.startswith("packet_index\t"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 12:
                continue
            yield NativeGuessMvRecord(
                packet_index=_parse_required_int(parts[0], "packet_index"),
                success=parts[1] == "1",
                frame_timestamp=_parse_required_int(parts[2], "frame_timestamp"),
                mb_x=_parse_required_int(parts[3], "mb_x"),
                mb_y=_parse_required_int(parts[4], "mb_y"),
                hop_count=_parse_required_int(parts[5], "hop_count"),
                pass_index=_parse_required_int(parts[6], "pass_index"),
                pred_count=_parse_required_int(parts[7], "pred_count"),
                estimated_mv_x=_parse_required_int(parts[8], "estimated_mv_x"),
                estimated_mv_y=_parse_required_int(parts[9], "estimated_mv_y"),
                estimated_ref=_parse_required_int(parts[10], "estimated_ref"),
                best_score=_parse_required_int(parts[11], "best_score"),
            )


def dump_guess_mv_records_with_native_decoder_batch_to_file(
    *,
    extractor_path: Path,
    input_path: Path,
    target_timestamp: int,
    target_packet_offset: int,
    transport_packet_size: int,
    corrupt_packet_indices: Sequence[int],
    output_path: Path,
    stderr_path: Path,
) -> NativeGuessMvBatchFileResult:
    if not corrupt_packet_indices:
        diagnostics = NativeGuessMvBatchDiagnostics(
            start_packet_index=0,
            stop_packet_index=0,
            packet_count=0,
            elapsed_seconds=0.0,
            returncode=0,
            stdout_bytes=0,
            stderr_bytes=0,
            stdout_line_count=0,
            parsed_record_count=0,
            successful_record_count=0,
            reported_packet_count=0,
            stderr_tail="",
        )
        return NativeGuessMvBatchFileResult(
            output_path=output_path,
            stderr_path=stderr_path,
            diagnostics=diagnostics,
        )

    start_packet_index = min(corrupt_packet_indices)
    stop_packet_index = max(corrupt_packet_indices)
    expected_packet_indices = set(range(start_packet_index, stop_packet_index + 1))
    if start_packet_index < 2:
        raise ValueError("Native guess_mv corrupt dump must start at packet index 2 or later")
    if set(corrupt_packet_indices) != expected_packet_indices:
        raise ValueError("Native guess_mv batch decode requires a contiguous packet-index range")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        output_path.unlink()
    if stderr_path.exists():
        stderr_path.unlink()

    command = _guess_mv_command(
        extractor_path=extractor_path,
        input_path=input_path,
        target_timestamp=target_timestamp,
        target_packet_offset=target_packet_offset,
        transport_packet_size=transport_packet_size,
        start_packet_index=start_packet_index,
        stop_packet_index=stop_packet_index,
    )
    start_time = time.monotonic()
    with output_path.open("w") as stdout_file, stderr_path.open("w") as stderr_file:
        completed = subprocess.run(
            command,
            stdout=stdout_file,
            stderr=stderr_file,
            text=True,
            check=False,
            env=_native_error_block_env(),
        )
    elapsed_seconds = time.monotonic() - start_time

    stdout_line_count = 0
    parsed_record_count = 0
    successful_record_count = 0
    reported_packet_indices: set[int] = set()
    for line in output_path.open():
        if not line.strip():
            continue
        stdout_line_count += 1
        if line.startswith("packet_index\t"):
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 12:
            continue
        packet_index = _parse_required_int(parts[0], "packet_index")
        parsed_record_count += 1
        reported_packet_indices.add(packet_index)
        if parts[1] == "1":
            successful_record_count += 1

    diagnostics = NativeGuessMvBatchDiagnostics(
        start_packet_index=start_packet_index,
        stop_packet_index=stop_packet_index,
        packet_count=len(corrupt_packet_indices),
        elapsed_seconds=elapsed_seconds,
        returncode=completed.returncode,
        stdout_bytes=output_path.stat().st_size if output_path.exists() else 0,
        stderr_bytes=stderr_path.stat().st_size if stderr_path.exists() else 0,
        stdout_line_count=stdout_line_count,
        parsed_record_count=parsed_record_count,
        successful_record_count=successful_record_count,
        reported_packet_count=len(reported_packet_indices),
        stderr_tail=_file_tail(stderr_path),
    )

    if completed.returncode != 0 and parsed_record_count == 0:
        raise NativeToolError(
            "Native guess_mv corrupt dump failed.\n"
            f"{_file_tail(output_path)}\n{diagnostics.stderr_tail}".strip()
        )

    return NativeGuessMvBatchFileResult(
        output_path=output_path,
        stderr_path=stderr_path,
        diagnostics=diagnostics,
    )
