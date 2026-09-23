#!/usr/bin/env python3
"""Export numeric cross-codec summaries without producing figures."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean

CODECS = {"h264": "h264", "hevc": "h265", "vp8": "vp8", "vp9": "vp9", "av1": "av1", "vvc": "h266"}


def finite_number(value):
    return value if isinstance(value, (int, float)) and math.isfinite(value) else None


def write_csv(path, rows, columns):
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def summarize(result_root: Path, output_dir: Path) -> None:
    frames = []
    for path in sorted(result_root.glob("*/frame_*/summary.json")):
        summary = json.loads(path.read_text())
        frames.append({
            "codec": CODECS.get(summary.get("codec_name"), summary.get("codec_name", "unknown")),
            "input_path": summary.get("input_path", ""),
            "frame_index": summary.get("frame_index"),
            "status": summary.get("experiment_status", "unknown"),
            "lossable_packets_tested": summary.get("lossable_packets_tested", 0),
            "failed_decode_count": summary.get("failed_decode_count", 0),
            "mean_ssim_drop": finite_number(summary.get("mean_ssim_drop")),
            "position_drop_pearson": finite_number(summary.get("relative_position_vs_ssim_drop_correlation")),
        })
    if not frames:
        raise SystemExit(f"No per-frame summary.json files found under {result_root}")

    codec_rows = []
    for codec in CODECS.values():
        selected = [row for row in frames if row["codec"] == codec]
        completed = [row for row in selected if row["status"] == "completed"]
        correlations = [row["position_drop_pearson"] for row in completed if row["position_drop_pearson"] is not None]
        losses = [row["mean_ssim_drop"] for row in completed if row["mean_ssim_drop"] is not None]
        codec_rows.append({
            "codec": codec,
            "input_videos": len({row["input_path"] for row in selected}),
            "frames_recorded": len(selected),
            "frames_completed": len(completed),
            "packet_trials": sum(row["lossable_packets_tested"] for row in completed),
            "failed_packet_trials": sum(row["failed_decode_count"] for row in completed),
            "frames_with_defined_correlation": len(correlations),
            "frames_with_negative_correlation": sum(value < 0 for value in correlations),
            "negative_correlation_fraction": mean(value < 0 for value in correlations) if correlations else None,
            "mean_frame_pearson": mean(correlations) if correlations else None,
            "mean_frame_ssim_drop": mean(losses) if losses else None,
        })

    output_dir.mkdir(parents=True, exist_ok=True)
    write_csv(output_dir / "frames.csv", frames, list(frames[0]))
    write_csv(output_dir / "codecs.csv", codec_rows, list(codec_rows[0]))
    print(f"Wrote {len(frames)} frame records and six codec rows to {output_dir}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result-root", type=Path, default=Path(__file__).resolve().parent / "result" / "ssim_loss")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    summarize(args.result_root, args.output_dir or args.result_root / "numeric_summary")


if __name__ == "__main__":
    main()
