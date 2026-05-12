#!/usr/bin/env python3
"""Small WD14 tagger bridge for SD_LoRA_Manager.

The Qt application calls this script as a subprocess and reads one JSON object
from stdout. Keep stderr for diagnostics only so the C++ side can parse stdout
reliably.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
import time
from pathlib import Path


def emit(payload: dict, exit_code: int = 0) -> None:
    print(json.dumps(payload, ensure_ascii=False, allow_nan=False), flush=True)
    raise SystemExit(exit_code)


def fail(message: str, exit_code: int = 1) -> None:
    emit({"ok": False, "error": message, "elapsed_sec": 0.0, "ratings": [], "tags": []}, exit_code)


def load_labels(csv_path: Path) -> list[dict]:
    if not csv_path.exists():
        fail(f"selected_tags.csv not found: {csv_path}")

    with csv_path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        if not reader.fieldnames or "name" not in reader.fieldnames or "category" not in reader.fieldnames:
            fail("selected_tags.csv must contain name and category columns")

        labels: list[dict] = []
        for row in reader:
            try:
                category = int(row.get("category", "-1"))
            except ValueError:
                continue
            name = (row.get("name") or "").strip()
            if name:
                labels.append({"name": name, "category": category})
        return labels


def category_name(category: int) -> str:
    if category == 9:
        return "rating"
    if category == 0:
        return "general"
    if category == 4:
        return "character"
    return "other"


def resolve_hw(shape: list) -> tuple[int, int, bool]:
    """Return width, height, nchw."""
    dims = [d if isinstance(d, int) and d > 0 else None for d in shape]
    if len(dims) != 4:
        return 448, 448, False

    if dims[1] == 3:
        return dims[3] or 448, dims[2] or 448, True
    return dims[2] or 448, dims[1] or 448, False


def preprocess(image_path: Path, width: int, height: int, nchw: bool):
    try:
        import numpy as np
        from PIL import Image, ImageOps
    except Exception as exc:
        fail(f"Missing Python dependency: {exc}. Please install pillow and numpy in the selected Python environment.")

    try:
        image = Image.open(image_path)
        image = ImageOps.exif_transpose(image).convert("RGB")
    except Exception as exc:
        fail(f"Failed to read image: {exc}")

    image.thumbnail((width, height), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (width, height), (255, 255, 255))
    left = (width - image.width) // 2
    top = (height - image.height) // 2
    canvas.paste(image, (left, top))

    array = np.asarray(canvas, dtype=np.float32)
    array = array[:, :, ::-1]  # RGB -> BGR, matching common WD14 ONNX exports.
    if nchw:
        array = array.transpose(2, 0, 1)
    return array[None, ...]


def main() -> None:
    parser = argparse.ArgumentParser(description="Run WD14 ONNX tagger and emit JSON.")
    parser.add_argument("--image", required=True)
    parser.add_argument("--model-dir", required=True)
    parser.add_argument("--threshold", type=float, default=0.35)
    args = parser.parse_args()

    started = time.perf_counter()
    image_path = Path(args.image)
    model_dir = Path(args.model_dir)
    model_path = model_dir / "model.onnx"
    csv_path = model_dir / "selected_tags.csv"

    if not image_path.exists():
        fail(f"Image not found: {image_path}")
    if not model_path.exists():
        fail(f"model.onnx not found: {model_path}")

    try:
        import onnxruntime as ort
    except Exception as exc:
        fail(f"Missing Python dependency: {exc}. Please install onnxruntime in the selected Python environment.")

    labels = load_labels(csv_path)
    try:
        session_options = ort.SessionOptions()
        session_options.log_severity_level = 3
        providers = ort.get_available_providers()
        if "CUDAExecutionProvider" in providers and os.environ.get("SDLM_WD14_USE_CUDA", "1") != "0":
            selected_providers = ["CUDAExecutionProvider", "CPUExecutionProvider"]
        else:
            selected_providers = ["CPUExecutionProvider"]
        session = ort.InferenceSession(str(model_path), sess_options=session_options, providers=selected_providers)
        input_info = session.get_inputs()[0]
        output_info = session.get_outputs()[0]
        width, height, nchw = resolve_hw(list(input_info.shape))
        input_tensor = preprocess(image_path, width, height, nchw)
        scores = session.run([output_info.name], {input_info.name: input_tensor})[0]
    except Exception as exc:
        fail(f"WD14 inference failed: {exc}")

    flat_scores = scores.reshape(-1).tolist()
    usable_count = min(len(flat_scores), len(labels))
    ratings: list[dict] = []
    tags: list[dict] = []

    for index in range(usable_count):
        label = labels[index]
        confidence = float(flat_scores[index])
        if not math.isfinite(confidence):
            confidence = 0.0
        category = int(label["category"])
        item = {
            "tag": label["name"],
            "category": category_name(category),
            "confidence": confidence,
        }
        if category == 9:
            ratings.append(item)
        elif confidence >= args.threshold:
            tags.append(item)

    ratings.sort(key=lambda item: item["confidence"], reverse=True)
    tags.sort(key=lambda item: (-item["confidence"], item["tag"].casefold()))

    emit(
        {
            "ok": True,
            "error": "",
            "elapsed_sec": round(time.perf_counter() - started, 4),
            "ratings": ratings,
            "tags": tags,
        }
    )


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as exc:
        fail(str(exc))
