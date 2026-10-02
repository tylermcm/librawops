"""Original optional smoothing control, not adaptive NR or an engine operation.

Frozen protocol: docs/research/RAW_NOISE_CONTROL_V1.md. NumPy/Pillow required;
native module is used only for the existing display transform, not filtering.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
import time

import numpy as np

import raw_noise_characterize as diagnostics
from raw_camera_extract import sha256_file

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "tests/reference/raw/noise_control_v1.json"
DISPLAY = {"working_space": "prophoto-d50", "exposure_stops": 0.0,
           "output_mode": "srgb-preview", "tile_size": 256}


def rgb(value):
    value = np.asarray(value)
    if (value.dtype != np.float32 or value.ndim != 3 or value.shape[2] != 3
            or not 1 <= min(value.shape[:2]) <= max(value.shape[:2]) <= 1024
            or not np.isfinite(value).all() or np.max(np.abs(value)) > 8):
        raise ValueError("control needs finite float32 RGB, axes 1..1024, absolute values <=8")
    return value


def parameters(radius, intensity, color):
    if type(radius) is not int or radius not in (1, 4):
        raise ValueError("control radius must be 1 or 4 native pixels")
    for strength in (intensity, color):
        if type(strength) not in (int, float) or not math.isfinite(strength) or not 0 <= strength <= 1:
            raise ValueError("control strengths must be finite numbers in [0,1]")


def proxies(value):
    value = np.asarray(value, dtype=np.float64)
    r, g, b = (value[..., c] for c in range(3))
    return np.stack(((r + 2*g + b)/4, r-g, b-g), axis=-1)


def reconstruct(value):
    l, cr, cb = (value[..., c] for c in range(3))
    g = l - (cr+cb)/4
    return np.stack((g+cr, g, g+cb), axis=-1)


def box(value, radius):
    """Clipped real-sample square mean with float64 summed-area reductions."""
    h, w = value.shape[:2]
    summed = np.pad(value, ((1, 0), (1, 0), (0, 0))).cumsum(axis=0).cumsum(axis=1)
    y, x = np.arange(h), np.arange(w)
    top, bottom = np.maximum(0, y-radius), np.minimum(h, y+radius+1)
    left, right = np.maximum(0, x-radius), np.minimum(w, x+radius+1)
    sums = (summed[bottom[:, None], right] - summed[top[:, None], right]
            - summed[bottom[:, None], left] + summed[top[:, None], left])
    count = (bottom-top)[:, None] * (right-left)
    return sums / count[..., None]


def control(value, radius=1, intensity_strength=0, color_strength=0):
    value = rgb(value)
    parameters(radius, intensity_strength, color_strength)
    if intensity_strength == color_strength == 0:
        return value.copy()
    channels = proxies(value)
    strengths = np.array([intensity_strength, color_strength, color_strength], dtype=np.float64)
    filtered = channels + strengths * (box(channels, radius) - channels)
    result = reconstruct(filtered).astype(np.float32)
    if not np.isfinite(result).all(): raise ValueError("nonfinite control output")
    return result


def tile(value, rect, **preset):
    """Fetch a radius halo in global input coordinates; retain just the request."""
    value = rgb(value)
    h, w = value.shape[:2]
    if (not isinstance(rect, (list, tuple)) or len(rect) != 4 or any(type(v) is not int for v in rect)):
        raise ValueError("tile needs four integer coordinates")
    x, y, rw, rh = rect
    if min(x, y) < 0 or min(rw, rh) < 1 or x+rw > w or y+rh > h:
        raise ValueError("tile outside input")
    parameters(preset.get("radius", 1), preset.get("intensity_strength", 0), preset.get("color_strength", 0))
    radius = preset.get("radius", 1)
    left, top = max(0, x-radius), max(0, y-radius)
    right, bottom = min(w, x+rw+radius), min(h, y+rh+radius)
    filtered = control(value[top:bottom, left:right], **preset)
    return filtered[y-top:y-top+rh, x-left:x-left+rw].copy()


def variance_factor(radius, strength):
    parameters(radius, strength, 0)
    n = (2*radius+1)**2
    return (1-strength)**2 + (2*strength-strength*strength)/n


def reduce_native(value, scale=4):
    h, w = value.shape[:2]
    if scale not in (2, 4) or h % scale or w % scale:
        raise ValueError("research reduction needs complete 2x2 or 4x4 cells")
    return value.astype(np.float64).reshape(h//scale, scale, w//scale, scale, 3).mean(axis=(1, 3)).astype(np.float32)


def truth_fixtures(config):
    n = config["synthetic_extent"]
    y, x = np.mgrid[:n, :n]
    constant = np.empty((n, n, 3), dtype=np.float32); constant[:] = [-.05, .2, 1.2]
    affine = np.stack((-.03+x/256, .1+y/256, .9+(x-y)/512), axis=-1).astype(np.float32)
    edges = reconstruct(np.stack((.15+.3*(x >= n//2), -.08+.16*(y >= n//2),
                                   .06-.12*(x+y >= n)), axis=-1)).astype(np.float32)
    texture = reconstruct(np.stack((.3+.05*np.sin(2*np.pi*x/16)+.04*((x//4+y//4) % 2),
                                     .025*np.sin(2*np.pi*y/8), .025*np.sin(2*np.pi*(x+y)/12)), axis=-1)).astype(np.float32)
    rng = np.random.Generator(np.random.PCG64(config["seed"]))
    result = []
    for name, truth in zip(("flat", "affine", "edges", "texture"), (constant, affine, edges, texture)):
        perturbation = rng.normal(size=truth.shape) * config["proxy_noise_stddev"]
        noisy = reconstruct(proxies(truth)+perturbation).astype(np.float32)
        result.append((name, truth, noisy))
    return result


def error_metrics(candidate, reference):
    def errors(delta):
        core = delta[8:-8, 8:-8]
        return {"rms": np.sqrt(np.mean(core*core, axis=(0, 1))).tolist(),
                "joint_rms": diagnostics.rms(core), "maximum_absolute": float(np.max(np.abs(core)))}
    return {"RGB": errors(candidate.astype(np.float64)-reference),
            "L_Cr_Cb": errors(proxies(candidate)-proxies(reference))}


def load_config(path):
    config = json.loads(path.read_text(encoding="utf-8"))
    expected = {"noise_control_version": 1, "domain": "native-scene-linear-prophoto-d50",
                "maximum_absolute_input": 8, "camera_crop_inset": 4, "synthetic_extent": 96,
                "seed": 20261001, "proxy_noise_stddev": [.02, .015, .015],
                "presets": [{"id": name, "radius": radius, "intensity_strength": intensity, "color_strength": color}
                    for name, radius, intensity, color in (("bypass", 1, 0, 0), ("intensity_half_r1", 1, .5, 0),
                        ("color_half_r1", 1, 0, .5), ("both_half_r1", 1, .5, .5), ("both_full_r4", 4, 1, 1))]}
    # Canonical JSON comparison also rejects booleans masquerading as integers.
    if json.dumps(config, sort_keys=True) != json.dumps(expected, sort_keys=True):
        raise ValueError("configuration differs from the frozen v1 control")
    return config


def input_buffers(folder, report_path):
    folder = folder.resolve(); report_path.resolve().relative_to(folder)
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if type(report.get("raw_noise_characterization_version")) is not int or report.get("raw_noise_characterization_version") != 1 or report.get("complete") is not True:
        raise ValueError("complete frozen noise study required")
    cases = report.get("cases", [])
    if len(cases) != 3 or [case["id"] for case in cases] != ["_DSC1788", "_DSC1790", "_DSC1793"]:
        raise ValueError("three frozen high-ISO captures required")
    plan = ROOT / "tests/reference/raw/camera_noise_rois_v1.json"
    if report.get("plan_sha256") != sha256_file(plan): raise ValueError("ROI plan provenance differs")
    frozen = json.loads(plan.read_text(encoding="utf-8"))
    result = []
    for case, capture in zip(cases, frozen["captures"]):
        if len(case["rois"]) != 4: raise ValueError("four frozen scene ROIs required")
        for roi, selected in zip(case["rois"], capture["rois"]):
            if roi["role"] != selected["role"] or roi["rect"] != selected["rect"]:
                raise ValueError("ROI selection differs from frozen study")
            for algorithm in ("rawengine.bilinear", "rawengine.menon_base"):
                entry = roi["algorithms"][algorithm]
                if entry["metrics"]["domain"] != "native scene-linear prophoto-d50; diagnostic camera matrix":
                    raise ValueError("calibrated input working domain differs")
                path = folder / entry["calibrated_path"]; path.resolve().relative_to(folder)
                if path.stat().st_size != 256*256*12 or sha256_file(path) != entry["calibrated_f32_sha256"]:
                    raise ValueError("calibrated input bytes/hash differs")
                value = rgb(np.frombuffer(path.read_bytes(), dtype="<f4").reshape(256, 256, 3))
                result.append((case, roi, algorithm, path, value))
    return result, report


def run(args):
    preparation_started = time.perf_counter()
    from PIL import Image, ImageDraw, ImageFont, __version__ as pillow_version
    config = load_config(args.config)
    inputs, original = input_buffers(args.folder, args.input_report)
    folder, output = diagnostics.fresh_output(args.folder, args.output)
    sys.path.insert(0, str(args.module_dir.resolve()))
    import rawengine_native as raw
    font_path = Path("C:/Windows/Fonts/consola.ttf")
    font = ImageFont.truetype(str(font_path), 13) if font_path.is_file() else ImageFont.load_default()
    protocol = (ROOT / "docs/research/RAW_NOISE_CONTROL_V1.md").read_text(encoding="utf-8").split("\n## Results", 1)[0]
    (output / "protocol-frozen.md").write_bytes(protocol.encode("utf-8"))
    report = {"raw_noise_control_version": 1, "complete": False, "config": config,
              "config_sha256": sha256_file(args.config), "input_report_sha256": sha256_file(args.input_report),
              "protocol_sha256": sha256_file(output / "protocol-frozen.md"),
              "upstream_source_sha256_lf": original["source_sha256_lf"],
              "source_sha256_lf": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
                  for p in (Path(__file__), ROOT / "tests/raw_noise_control_tests.py", ROOT / "tools/raw_noise_characterize.py",
                            ROOT / "tools/raw_camera_extract.py", ROOT / "tools/raw_camera_engine_check.py",
                            ROOT / "bench/raw_session_benchmark.py")},
              "display": {"options": DISPLAY, "module_sha256": sha256_file(Path(raw.__file__)),
                          "engine_dll_sha256": sha256_file(args.module_dir / "RawEngine.dll")},
              "runtime": {"python": sys.version, "numpy": np.__version__, "pillow": pillow_version,
                          "font_sha256": sha256_file(font_path) if font_path.is_file() else None},
              "hardware": diagnostics.hardware(), "synthetic": [], "camera": [], "figures": [],
              "limits": "Original linear smoothing control, not adaptive NR, sensor noise model, trained model or native operation. Camera metrics mix texture/noise; 232-square centers differ from earlier study. Synthetic Gaussian proxies are not calibrated RAW noise. Timing/memory describe this research process only."}
    started = time.perf_counter()
    report["preparation_seconds"] = started-preparation_started

    def save(name, value):
        path = output / (name + ".bin"); path.write_bytes(value.astype("<f4").tobytes())
        return {"path": path.relative_to(folder).as_posix(), "sha256": sha256_file(path), "dimensions": list(value.shape[:2][::-1])}

    def display(value):
        w, h, data = raw.render_raster(np.ascontiguousarray(value), value.shape[1], value.shape[0], DISPLAY)
        if (h, w) != value.shape[:2]: raise ValueError("display changed dimensions")
        encoded = np.frombuffer(data, dtype="<f4").reshape(h, w, 3)
        if not np.isfinite(encoded).all(): raise ValueError("nonfinite display output")
        return Image.fromarray(np.rint(np.clip(encoded, 0, 1)*255).astype(np.uint8)), hashlib.sha256(data).hexdigest()

    def figure(name, image):
        path = output / (name + ".png"); image.save(path)
        report["figures"].append({"path": path.relative_to(folder).as_posix(), "sha256": sha256_file(path)})

    synthetic_board = Image.new("RGB", (736, 564), "#202020")
    sd = ImageDraw.Draw(synthetic_board)
    sd.text((8, 8), "Known truth / injected proxy noise / linear smoothing controls | native 1:1", font=font, fill="white")
    for col, label in enumerate(("truth", "bypass", "L half r1", "C half r1", "L+C half r1", "L+C full r4")):
        sd.text((8+col*120, 32), label, font=font, fill="white")
    for row, (name, truth, noisy) in enumerate(truth_fixtures(config)):
        entry = {"id": name, "truth": save("synthetic-"+name+"-truth", truth),
                 "noisy": save("synthetic-"+name+"-noisy", noisy), "controls": {}}
        top = 58+row*124
        sd.text((8, top), name, font=font, fill="white")
        image, _ = display(truth); synthetic_board.paste(image, (8, top+20))
        for col, preset in enumerate(config["presets"]):
            settings = {k: v for k, v in preset.items() if k != "id"}
            clean = control(truth, **settings); filtered = control(noisy, **settings)
            filtered_then_reduced = reduce_native(filtered)
            reduced_then_filtered = control(reduce_native(noisy), **settings)
            image, encoded_hash = display(filtered); synthetic_board.paste(image, (8+(col+1)*120, top+20))
            entry["controls"][preset["id"]] = {"clean": save("synthetic-"+name+"-"+preset["id"]+"-clean", clean),
                "noisy": save("synthetic-"+name+"-"+preset["id"]+"-noisy", filtered),
                "error_to_truth": error_metrics(filtered, truth), "clean_filter_bias": error_metrics(clean, truth),
                "injected_noise_response": error_metrics(filtered, clean), "encoded_sha256": encoded_hash,
                "native_vs_reduced_first_max_error": float(np.max(np.abs(filtered_then_reduced.astype(np.float64)-reduced_then_filtered))),
                "flat_proxy_predicted_variance_factors": [variance_factor(preset["radius"], s)
                    for s in (preset["intensity_strength"], preset["color_strength"], preset["color_strength"])]}
        report["synthetic"].append(entry)
    figure("synthetic-truth-control", synthetic_board)

    boards = {}
    for case, roi, algorithm, path, value in inputs:
        key = case["id"] + "-" + algorithm.split(".")[-1]
        if key not in boards:
            board = Image.new("RGB", (1284, 1176), "#202020"); draw = ImageDraw.Draw(board)
            draw.text((8, 8), f"{key} ISO {case['capture']['iso']} | 248-square valid crops / native 1:1 / shared display", font=font, fill="white")
            for col, preset in enumerate(config["presets"]):
                draw.text((8+col*256, 32), preset["id"].replace("_", " "), font=font, fill="white")
            boards[key] = board
        board = boards[key]; draw = ImageDraw.Draw(board)
        row = diagnostics.ROLES.index(roi["role"]); top = 58+row*278
        draw.text((8, top), f"{roi['role']} | original sensor ROI={roi['rect']} | discard 4px crop border", font=font, fill="white")
        entry = {"id": case["id"], "role": roi["role"], "algorithm": algorithm, "original_roi": roi["rect"],
                 "input_sha256": sha256_file(path), "controls": {}}
        for col, preset in enumerate(config["presets"]):
            settings = {k: v for k, v in preset.items() if k != "id"}
            begin = time.perf_counter(); filtered = control(value, **settings)[4:-4, 4:-4].copy()
            filter_ms = (time.perf_counter()-begin)*1000
            begin = time.perf_counter(); metrics = diagnostics.rgb_metrics(filtered)
            statistics_ms = (time.perf_counter()-begin)*1000
            begin = time.perf_counter(); image, encoded_hash = display(filtered)
            display_ms = (time.perf_counter()-begin)*1000
            board.paste(image, (8+col*256, top+20))
            output_name = key+"-"+roi["role"]+"-"+preset["id"]
            entry["controls"][preset["id"]] = {"output": save(output_name, filtered), "metrics": metrics,
                "encoded_sha256": encoded_hash, "timings_ms": {"filter": filter_ms, "statistics": statistics_ms, "display": display_ms}}
        report["camera"].append(entry)
    for name, board in boards.items(): figure(name+"-smoothing-control", board)
    report["workflow_seconds"] = time.perf_counter()-started
    report["total_seconds"] = report["workflow_seconds"]+report["preparation_seconds"]
    report["process_memory"] = diagnostics.memory()
    report["complete"] = True
    (output / "noise-control-v1.json").write_text(json.dumps(report, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    print(json.dumps({"report": str(output / "noise-control-v1.json"), "camera_controls": len(inputs)*len(config["presets"]),
                      "synthetic_controls": len(report["synthetic"])*len(config["presets"]), "seconds": report["workflow_seconds"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("module_dir", "folder", "input_report", "output"): parser.add_argument(name, type=Path)
    parser.add_argument("--config", type=Path, default=CONFIG)
    run(parser.parse_args())
