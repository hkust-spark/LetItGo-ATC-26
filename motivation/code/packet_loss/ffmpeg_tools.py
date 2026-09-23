from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass
import json
import os
from pathlib import Path
import subprocess


def _resolve_ffmpeg_tool_path(tool_env_name: str, tool_name: str) -> Path:
    tool_path = os.environ.get(tool_env_name)
    if tool_path:
        return Path(tool_path).expanduser()

    ffmpeg_prefix = os.environ.get("FFMPEG_PREFIX")
    if ffmpeg_prefix:
        return Path(ffmpeg_prefix).expanduser() / "bin" / tool_name

    return Path("/usr/local/bin") / tool_name


FFMPEG_BIN = _resolve_ffmpeg_tool_path("FFMPEG_BIN", "ffmpeg")
FFPROBE_BIN = _resolve_ffmpeg_tool_path("FFPROBE_BIN", "ffprobe")


class FFmpegToolError(RuntimeError):
    """Raised when ffprobe or ffmpeg fails in a way we cannot recover from."""


@dataclass(frozen=True)
class FrameProbe:
    codec_name: str
    width: int
    height: int
    frame_index: int
    total_frames: int
    timestamp: int
    timestamp_seconds: float
    stream_time_base: str
    packet_offset: int
    packet_size: int
    picture_type: str
    is_key_frame: bool


def _require_tool_path(tool_path: Path) -> str:
    if not tool_path.is_file():
        raise FFmpegToolError(f"Required tool was not found: {tool_path}")
    return str(tool_path)


def _run_json_command(command: list[str]) -> dict:
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise FFmpegToolError(
            f"Command failed with exit code {completed.returncode}: {' '.join(command)}\n"
            f"{completed.stderr.strip()}"
        )
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise FFmpegToolError(
            f"Failed to parse JSON output from: {' '.join(command)}"
        ) from exc


def _probe_video_payload(input_path: Path) -> dict:
    command = [
        _require_tool_path(FFPROBE_BIN),
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_streams",
        "-show_frames",
        "-show_entries",
        "stream=codec_name,width,height,time_base:"
        "frame=best_effort_timestamp,best_effort_timestamp_time,pkt_pos,pkt_size,pict_type,key_frame",
        "-print_format",
        "json",
        str(input_path),
    ]
    return _run_json_command(command)


def _parse_frame_probes(input_path: Path, payload: dict) -> list[FrameProbe]:
    streams = payload.get("streams", [])
    if not streams:
        raise FFmpegToolError(f"No video stream found in {input_path}")

    frames = [
        frame
        for frame in payload.get("frames", [])
        if "pkt_pos" in frame and "pkt_size" in frame
    ]
    if not frames:
        raise FFmpegToolError(f"No decodable video frames found in {input_path}")

    stream = streams[0]
    return [
        FrameProbe(
            codec_name=stream.get("codec_name", "unknown"),
            width=int(stream.get("width", 0)),
            height=int(stream.get("height", 0)),
            frame_index=frame_index,
            total_frames=len(frames),
            timestamp=int(frame.get("best_effort_timestamp", "0")),
            timestamp_seconds=float(frame.get("best_effort_timestamp_time", "0.0")),
            stream_time_base=stream.get("time_base", "0/1"),
            packet_offset=int(frame["pkt_pos"]),
            packet_size=int(frame["pkt_size"]),
            picture_type=frame.get("pict_type", "unknown"),
            is_key_frame=bool(int(frame.get("key_frame", 0))),
        )
        for frame_index, frame in enumerate(frames)
    ]


def probe_video_frames(input_path: Path) -> list[FrameProbe]:
    return _parse_frame_probes(input_path, _probe_video_payload(input_path))


def probe_target_frames(input_path: Path, frame_indices: Iterable[int]) -> dict[int, FrameProbe]:
    requested_indices = tuple(frame_indices)
    if not requested_indices:
        return {}

    probes = probe_video_frames(input_path)
    invalid_indices = [
        frame_index
        for frame_index in requested_indices
        if frame_index < 0 or frame_index >= len(probes)
    ]
    if invalid_indices:
        first_invalid = invalid_indices[0]
        raise ValueError(
            f"frame_index={first_invalid} is out of range for {input_path}; "
            f"valid range is 0..{len(probes) - 1}"
        )

    return {frame_index: probes[frame_index] for frame_index in requested_indices}


def probe_target_frame(input_path: Path, frame_index: int) -> FrameProbe:
    return probe_target_frames(input_path, (frame_index,))[frame_index]


def extract_frame(input_path: Path, frame_index: int, output_path: Path) -> tuple[bool, str]:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        output_path.unlink()

    command = [
        _require_tool_path(FFMPEG_BIN),
        "-v",
        "error",
        "-y",
        "-i",
        str(input_path),
        "-vf",
        f"select=eq(n\\,{frame_index})",
        "-frames:v",
        "1",
        str(output_path),
    ]
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        check=False,
    )

    output_exists = output_path.exists() and output_path.stat().st_size > 0
    message = "\n".join(
        part.strip()
        for part in (completed.stdout, completed.stderr)
        if part and part.strip()
    )

    if not output_exists and output_path.exists():
        output_path.unlink()

    return output_exists, message
