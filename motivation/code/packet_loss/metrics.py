from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image
from skimage.metrics import structural_similarity


def _next_ppm_token(data: bytes, offset: int) -> tuple[bytes, int]:
    while offset < len(data):
        if data[offset] == ord("#"):
            while offset < len(data) and data[offset] not in b"\r\n":
                offset += 1
        elif data[offset] in b" \t\r\n":
            offset += 1
        else:
            break

    start = offset
    while offset < len(data) and data[offset] not in b" \t\r\n":
        offset += 1
    return data[start:offset], offset


def _load_ppm_image(path: Path) -> np.ndarray:
    data = path.read_bytes()
    magic, offset = _next_ppm_token(data, 0)
    if magic != b"P6":
        raise ValueError(f"Unsupported PPM magic in {path}: {magic!r}")

    width_token, offset = _next_ppm_token(data, offset)
    height_token, offset = _next_ppm_token(data, offset)
    max_value_token, offset = _next_ppm_token(data, offset)
    width = int(width_token)
    height = int(height_token)
    max_value = int(max_value_token)
    if max_value != 255:
        raise ValueError(f"Unsupported PPM max value in {path}: {max_value}")

    if offset < len(data) and data[offset:offset + 2] == b"\r\n":
        offset += 2
    elif offset < len(data) and data[offset] in b" \t\r\n":
        offset += 1

    expected_size = width * height * 3
    payload = data[offset:offset + expected_size]
    if len(payload) != expected_size:
        raise ValueError(
            f"PPM payload size does not match for {path}: {len(payload)} vs {expected_size}"
        )

    return np.frombuffer(payload, dtype=np.uint8).reshape((height, width, 3))


def load_rgb_image(path: Path) -> np.ndarray:
    if path.suffix.lower() == ".ppm":
        return _load_ppm_image(path)
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"))


def calculate_ssim_arrays(reference: np.ndarray, candidate: np.ndarray) -> float:
    if reference.shape != candidate.shape:
        raise ValueError(
            f"Image shapes do not match for SSIM: {reference.shape} vs {candidate.shape}"
        )

    return float(
        structural_similarity(
            reference,
            candidate,
            channel_axis=-1,
            data_range=255,
        )
    )


def calculate_ssim(reference_path: Path, candidate_path: Path) -> float:
    reference = load_rgb_image(reference_path)
    candidate = load_rgb_image(candidate_path)
    return calculate_ssim_arrays(reference, candidate)
