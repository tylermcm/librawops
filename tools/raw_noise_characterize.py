"""Bounded local camera noise/texture diagnostics; no denoise or noise ground truth.

Optional NumPy/Pillow research runtime and current native module required.
Protocol: docs/research/RAW_NOISE_CHARACTERIZATION_V1.md. All images/data are local.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import sys
import time

import numpy as np

from raw_camera_extract import sha256_file
from raw_camera_engine_check import camera_matrix, native_metadata, pixels, request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bench"))
from raw_session_benchmark import memory, stage_manifest

MARGIN = 8
RADII = (1, 4)
LAGS = (1, 2, 4, 8)
ROLES = ("background", "skin", "plain_fabric", "printed_detail")
PATTERNS = ((0, 1, 1, 2), (2, 1, 1, 0), (1, 0, 2, 1), (1, 2, 0, 1))


def plane(value):
    result = np.asarray(value, dtype=np.float64)
    if (result.ndim != 2 or min(result.shape) < 2 * MARGIN + max(LAGS) + 1
            or max(result.shape) > 1024 or not np.isfinite(result).all()):
        raise ValueError("diagnostic planes must be finite, 2D and between 25 and 1024 pixels per axis")
    return result


def residual(value, radius):
    """Float64 valid square mean, evaluated on the fixed common eight-pixel inset."""
    value = plane(value)
    if radius not in RADII: raise ValueError("unregistered diagnostic radius")
    size = 2 * radius + 1
    summed = np.pad(value, ((1, 0), (1, 0))).cumsum(axis=0).cumsum(axis=1)
    means = (summed[size:, size:] - summed[:-size, size:] - summed[size:, :-size]
             + summed[:-size, :-size]) / (size * size)
    start = MARGIN - radius
    h, w = value.shape
    return value[MARGIN:h - MARGIN, MARGIN:w - MARGIN] - means[start:h - MARGIN - radius, start:w - MARGIN - radius]


def correlation(a, b):
    a, b = a - a.mean(), b - b.mean()
    # PERF-004: use elementwise reductions here, avoiding BLAS dot-product teams
    # for small ROI statistics. Research timings are recorded separately.
    aa, bb = np.mean(a * a), np.mean(b * b)
    if math.sqrt(float(aa)) <= 1e-12 or math.sqrt(float(bb)) <= 1e-12: return None
    return float(np.clip(np.mean(a * b) / math.sqrt(float(aa * bb)), -1, 1))


def rms(value): return math.sqrt(float(np.mean(value * value)))


def plane_metrics(value):
    value = plane(value)
    core = value[MARGIN:-MARGIN, MARGIN:-MARGIN]
    result = {"dimensions": list(core.shape[::-1]), "count": int(core.size),
              "mean": float(core.mean()), "stddev_population": float(core.std()),
              "min": float(core.min()), "max": float(core.max()),
              "percentiles_1_50_99": np.percentile(core, [1, 50, 99]).tolist(),
              "negative_count": int(np.count_nonzero(core < 0)),
              "above_one_count": int(np.count_nonzero(core > 1)),
              "first_difference_rms": {"x": rms(np.diff(core, axis=1)), "y": rms(np.diff(core, axis=0))},
              "residuals": {}}
    for radius in RADII:
        high = residual(value, radius)
        lags = {}
        for lag in LAGS:
            lags[f"x{lag}"] = correlation(high[:, :-lag], high[:, lag:])
            lags[f"y{lag}"] = correlation(high[:-lag], high[lag:])
        result["residuals"][str(radius)] = {"mean": float(high.mean()), "rms": rms(high),
            "stddev_population": float(high.std()), "correlation": lags}
    return result


def rgb_metrics(value):
    value = np.asarray(value, dtype=np.float64)
    if value.ndim != 3 or value.shape[2] != 3: raise ValueError("diagnostic RGB must have three channels")
    r, g, b = (value[:, :, c] for c in range(3))
    proxies = {"L": (r + 2 * g + b) / 4, "Cr": r - g, "Cb": b - g}
    channels = {name: plane_metrics(value[:, :, c]) for c, name in enumerate(("R", "G", "B"))}
    measures = {name: plane_metrics(p) for name, p in proxies.items()}
    joint = {str(radius): math.sqrt(sum(measures[c]["residuals"][str(radius)]["rms"] ** 2
                                          for c in ("Cr", "Cb")) / 2) for radius in RADII}
    differences = measures["L"]["first_difference_rms"]
    return {"domain": "native scene-linear prophoto-d50; diagnostic camera matrix",
            "channels": channels, "proxies": measures, "joint_chroma_residual_rms": joint,
            "L_first_difference_joint_rms": math.sqrt((differences["x"] ** 2 + differences["y"] ** 2) / 2)}


def normalized_sites(codes, metadata, roi):
    x, y, w, h = roi
    pattern, px, py = metadata["pattern"], metadata.get("cfa_phase_x", 0), metadata.get("cfa_phase_y", 0)
    if (type(pattern) is not int or pattern not in range(4) or type(px) is not int or type(py) is not int
            or px not in (0, 1) or py not in (0, 1)):
        raise ValueError("invalid diagnostic Bayer pattern/phase")
    samples = np.asarray(codes[y:y + h, x:x + w])
    if samples.shape != (h, w) or samples.dtype != np.uint16: raise ValueError("invalid owned sensor ROI")
    yy, xx = np.mgrid[y:y + h, x:x + w]
    sites = ((yy + py) % 2) * 2 + ((xx + px) % 2)
    black, white = (np.asarray(metadata[k], dtype=np.float32) for k in ("black_levels", "white_levels"))
    if black.shape != (4,) or white.shape != (4,) or not np.isfinite(black).all() or not np.isfinite(white).all() or np.any(white <= black):
        raise ValueError("invalid diagnostic site levels")
    low, high = np.take(black, sites), np.take(white, sites)
    normalized = (samples.astype(np.float32) - low) / (high - low)
    colors = np.take(PATTERNS[pattern], sites)
    return normalized, sites, colors


def site_metrics(normalized, sites, colors):
    h, w = normalized.shape
    if h % 2 or w % 2: raise ValueError("site-grid statistics require even ROI dimensions")
    result = {}
    for row in (0, 1):
        for col in (0, 1):
            index = int(sites[row, col])
            result[str(index)] = {"color": ("R", "G", "B")[int(colors[row, col])],
                                 "metrics": plane_metrics(normalized[row::2, col::2])}
    return result


def verify_observed(output, normalized, colors):
    observed = np.take_along_axis(pixels(output), colors[:, :, None], axis=2)[:, :, 0]
    if observed.tobytes() != normalized.tobytes(): raise ValueError("independent observed-site normalization differs")
    return int(observed.size)


def ratios(bilinear, menon):
    def divide(a, b): return a / b if b != 0 else None
    return {"joint_chroma_residual_rms": {str(r): divide(menon["joint_chroma_residual_rms"][str(r)],
                 bilinear["joint_chroma_residual_rms"][str(r)]) for r in RADII},
            "L_first_difference_joint_rms": divide(menon["L_first_difference_joint_rms"], bilinear["L_first_difference_joint_rms"])}


def validate_plan(plan, records):
    if (type(plan.get("camera_noise_roi_plan_version")) is not int or plan.get("camera_noise_roi_plan_version") != 1 or plan.get("roi_extent") != [256, 256]
            or plan.get("metric_margin") != MARGIN or plan.get("residual_radii") != list(RADII)
            or plan.get("correlation_lags") != list(LAGS) or plan.get("residual_view_half_range") != .02):
        raise ValueError("unsupported noise diagnostic contract")
    captures = plan.get("captures")
    if not isinstance(captures, list) or not 1 <= len(captures) <= 4: raise ValueError("study budget is one to four captures")
    ids = set()
    for capture in captures:
        id_ = capture.get("id")
        if not isinstance(id_, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", id_) or id_ in ids or id_ not in records:
            raise ValueError("missing, duplicate or invalid capture ID")
        ids.add(id_)
        rois = capture.get("rois")
        if not isinstance(rois, list) or [r.get("role") for r in rois] != list(ROLES):
            raise ValueError("exactly four ordered, declared scene roles required")
        ax, ay, aw, ah = records[id_]["metadata"]["active_area"]
        rectangles = set()
        for roi in rois:
            rect = roi.get("rect")
            if (not isinstance(roi.get("description"), str) or not roi["description"]
                    or not isinstance(rect, list) or len(rect) != 4 or any(type(v) is not int for v in rect)
                    or rect[2:] != [256, 256] or rect[0] < ax or rect[1] < ay
                    or rect[0] + 256 > ax + aw or rect[1] + 256 > ay + ah
                    or (rect[0] - ax) % 4 or (rect[1] - ay) % 4):
                raise ValueError("ROI must be described, confined and aligned to active-relative four-pixel cells")
            if tuple(rect) in rectangles: raise ValueError("scene roles require distinct rectangles")
            rectangles.add(tuple(rect))


def fresh_output(folder, output):
    folder, output = folder.resolve(), output.resolve()
    output.relative_to(folder)
    if output.exists(): raise ValueError("use a new study directory; prior evidence is retained")
    output.mkdir(parents=True)
    return folder, output


def residual_view(rgb):
    rgb = np.asarray(rgb, dtype=np.float64)
    cr, cb = residual(rgb[:, :, 0] - rgb[:, :, 1], 4), residual(rgb[:, :, 2] - rgb[:, :, 1], 4)
    out = np.full((rgb.shape[0], rgb.shape[1], 3), .5)
    out[MARGIN:-MARGIN, MARGIN:-MARGIN, 0] += .5 * cr / .02
    out[MARGIN:-MARGIN, MARGIN:-MARGIN, 2] += .5 * cb / .02
    return np.rint(np.clip(out, 0, 1) * 255).astype(np.uint8)


def hardware():
    result = {"platform": platform.platform(), "machine": platform.machine(), "logical_cpus": os.cpu_count(),
              "thread_environment": {key: os.environ.get(key) for key in ("OMP_NUM_THREADS", "OMP_DYNAMIC", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS")}}
    if os.name == "nt":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            result["cpu_model"] = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    return result


def run(args):
    from PIL import Image, ImageDraw, ImageFont, __version__ as pillow_version
    sys.path.insert(0, str(args.module_dir.resolve()))
    import rawengine_native as raw
    extraction = json.loads(args.extraction.read_text(encoding="utf-8"))
    if extraction.get("local_extraction_schema_version") != 1: raise ValueError("unsupported extraction")
    records = {r["id"]: r for r in extraction["assets"]}
    if len(records) != len(extraction["assets"]): raise ValueError("duplicate extracted capture IDs")
    plan = json.loads(args.plan.read_text(encoding="utf-8")); validate_plan(plan, records)
    folder, output = fresh_output(args.folder, args.output)
    font_path = Path("C:/Windows/Fonts/consola.ttf")
    font = ImageFont.truetype(str(font_path), 13) if font_path.is_file() else ImageFont.load_default()
    report = {"raw_noise_characterization_version": 1, "complete": False,
              "plan_sha256": sha256_file(args.plan), "extraction_sha256": sha256_file(args.extraction),
              "native_module_sha256": sha256_file(Path(raw.__file__)), "engine_dll_sha256": sha256_file(args.module_dir / "RawEngine.dll"),
              "source_sha256_lf": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
                  for p in (Path(__file__), ROOT / "tools/raw_camera_engine_check.py", ROOT / "tools/raw_camera_extract.py",
                            ROOT / "bench/raw_session_benchmark.py", ROOT / "MenonDemosaic.cpp")},
              "runtime": {"python": sys.version, "numpy": np.__version__, "pillow": pillow_version,
                          "font_sha256": sha256_file(font_path) if font_path.is_file() else None},
              "hardware": hardware(),
              "settings": {"workers": 1, "cache_bytes": 0, "native_tile_sizes": [256, 64], "exposure_stops": 0,
                           "metric_margin": MARGIN, "residual_radii": list(RADII), "correlation_lags": list(LAGS)},
              "limits": "Single photographs contain noise/texture/artifacts together; unmatched ISO/exposure/light, diagnostic matrix. No noise ground truth, SNR/ISO curve, denoise, perceptual quality acceptance or cross-image amplitude comparison. Times are research workflow cost, not isolated render benchmarks; memory is process-wide.",
              "cases": []}
    started = time.perf_counter()
    for capture in plan["captures"]:
        record = records[capture["id"]]; meta = record["metadata"]
        hashes_started = time.perf_counter()
        for field in ("original", "decoded"):
            path = folder / record[field]["path"]; path.resolve().relative_to(folder)
            if sha256_file(path) != record[field]["sha256"]: raise ValueError("camera input hash mismatch")
        decoded = folder / record["decoded"]["path"]
        if decoded.stat().st_size != meta["height"] * meta["row_stride_samples"] * 2: raise ValueError("decoded byte count differs")
        codes = np.memmap(decoded, dtype="<u2", mode="r", shape=(meta["height"], meta["row_stride_samples"]))
        wb = record["camera_whitebalance"]
        if len(wb) != 4 or min(wb) <= 0 or wb[1] != wb[3]: raise ValueError("unsupported as-shot WB")
        recipe = dict(zip(("red_gain", "green_gain", "blue_gain"), (wb[i] / wb[1] for i in range(3))),
                      exposure_stops=0.0, camera_to_xyz_d50=camera_matrix(record), working_space="prophoto-d50", output_mode="srgb-preview")
        case = {"id": record["id"], "capture": record["capture"], "metadata": meta, "recipe": recipe,
                "original_sha256": record["original"]["sha256"], "decoded_sha256": record["decoded"]["sha256"],
                "hash_validation_ms": (time.perf_counter() - hashes_started) * 1000, "sessions": {}, "rois": []}
        normalized = []
        for selected in capture["rois"]:
            values, sites, colors = normalized_sites(codes, meta, selected["rect"])
            normalized.append((values, sites, colors))
            case["rois"].append({**selected, "raw_site_grid": site_metrics(values, sites, colors), "algorithms": {}})
        board = Image.new("RGB", (1080, 1244), "#202020"); draw = ImageDraw.Draw(board)
        draw.text((8, 8), f"{record['id']} ISO {record['capture']['iso']} | native 1:1 / same recipe / no denoise", font=font, fill="white")
        draw.text((8, 26), "Residual r4: red=Cr blue=Cb | fixed +/-0.02 working units | gray margin excluded", font=font, fill="white")
        for col, title in enumerate(("bilinear encoded", "Menon encoded", "bilinear residual r4", "Menon residual r4")):
            draw.text((8 + col * 268, 46), title, font=font, fill="white")
        for column, algorithm in enumerate(("rawengine.bilinear", "rawengine.menon_base")):
            init_started = time.perf_counter()
            session = raw.RawSession(codes, meta["width"], meta["height"], native_metadata(meta), cache_bytes=0, workers=1,
                                     demosaic={"algorithm": algorithm, "processing_version": 1})
            case["sessions"][algorithm] = {"initialization_ms": (time.perf_counter() - init_started) * 1000}
            try:
                document = json.loads(session.export_manifest(recipe))
                calibration_id = next(op["id"] for op in document["operations"] if op["type"] == "rawengine.camera_to_working")
                linear = json.dumps(stage_manifest(document, calibration_id))
                source = json.dumps(stage_manifest(document, document["sources"][0]["id"]))
                for row, roi in enumerate(case["rois"]):
                    timings = {}; rect = roi["rect"]
                    def timed(name, action):
                        begin = time.perf_counter(); value = action(); timings[name] = (time.perf_counter() - begin) * 1000; return value
                    observed = timed("source_render", lambda: session.render_manifest(source, request(rect)))
                    observed_count = verify_observed(observed, normalized[row][0], normalized[row][2])
                    calibrated = timed("calibrated_render", lambda: session.render_manifest(linear, request(rect)))
                    tiled = timed("calibrated_tile64_parity", lambda: session.render_manifest(linear, request(rect, 64)))
                    if tiled != calibrated: raise ValueError("native tile partition changed calibrated pixels")
                    metrics = timed("statistics", lambda: rgb_metrics(pixels(calibrated)))
                    encoded = timed("encoded_render", lambda: session.render({**recipe, **request(rect)}))
                    name = record["id"] + "-" + roi["role"] + "-" + algorithm.split(".")[-1] + "-linear-f32le.bin"
                    (output / name).write_bytes(calibrated[2])
                    roi["algorithms"][algorithm] = {"source_f32_sha256": hashlib.sha256(observed[2]).hexdigest(),
                        "observed_channels_exact": observed_count, "calibrated_tile64_exact": True,
                        "calibrated_f32_sha256": hashlib.sha256(calibrated[2]).hexdigest(), "calibrated_path": (output / name).relative_to(folder).as_posix(),
                        "encoded_f32_sha256": hashlib.sha256(encoded[2]).hexdigest(), "metrics": metrics, "timings_ms": timings}
                    top = 68 + row * 294
                    if column == 0: draw.text((8, top), f"{roi['role']} sensor={rect}", font=font, fill="white")
                    board.paste(Image.fromarray(np.rint(np.clip(pixels(encoded), 0, 1) * 255).astype(np.uint8)), (8 + column * 268, top + 20))
                    board.paste(Image.fromarray(residual_view(pixels(calibrated))), (8 + (column + 2) * 268, top + 20))
                    draw.text((8 + column * 268, top + 278), f"Cr/Cb joint r4={metrics['joint_chroma_residual_rms']['4']:.5f}", font=font, fill="white")
            finally: session.close()
        for roi in case["rois"]:
            roi["menon_over_bilinear"] = ratios(roi["algorithms"]["rawengine.bilinear"]["metrics"], roi["algorithms"]["rawengine.menon_base"]["metrics"])
        board_started = time.perf_counter(); board_path = output / (record["id"] + "-noise-detail.png"); board.save(board_path)
        case["board_export_ms"] = (time.perf_counter() - board_started) * 1000
        case["board"] = {"path": board_path.relative_to(folder).as_posix(), "sha256": sha256_file(board_path)}
        report["cases"].append(case)
        (output / "noise-characterization-v1.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print(record["id"] + ": " + json.dumps({r["role"]: r["menon_over_bilinear"]["joint_chroma_residual_rms"] for r in case["rois"]}), flush=True)
    report["complete"] = True; report["workflow_seconds"] = time.perf_counter() - started; report["memory_final"] = memory()
    (output / "noise-characterization-v1.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"workflow_seconds": report["workflow_seconds"], "memory_final": report["memory_final"]}), flush=True)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("module_dir", "folder", "extraction", "plan", "output"): parser.add_argument(name, type=Path)
    run(parser.parse_args())
