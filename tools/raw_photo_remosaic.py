"""Frozen photographic RGB remosaicing study; optional isolated scientific runtime.

Rendered photos are simulated Bayer inputs, not camera-RAW ground truth.
No quality-policy revision, product dependency or native acceptance is implied.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import raw_colour_code_review as upstream
import raw_menon_reference as scalar
import raw_quality_harness as h

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "tests/reference/raw/photo_remosaic_assets_v1.json"
PROTOCOL = ROOT / "tests/reference/raw/photo_remosaic_protocol_v1.json"
ASSET_HASH = "caf67bad2e7bcd589fd2f0282256d1a1c177b81a07def42f72ed5ad95bf16990"
PROTOCOL_HASH = "179493d18051b3843fbab23e80e3f3bebbe904540b8a7628dad34024dcf2620b"


def bilinear(np, raw, channels):
    """Native-v1 neighbor order, float32 sums, valid samples and observed copy."""
    height, width = raw.shape
    result = np.zeros((height, width, 3), dtype=np.float32)
    for channel in range(3):
        sums = np.zeros_like(raw)
        counts = np.zeros(raw.shape, dtype=np.int32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                y0, y1 = max(0, -dy), min(height, height - dy)
                x0, x1 = max(0, -dx), min(width, width - dx)
                selected = channels[y0 + dy:y1 + dy, x0 + dx:x1 + dx] == channel
                sums[y0:y1, x0:x1] += np.where(selected, raw[y0 + dy:y1 + dy, x0 + dx:x1 + dx], 0)
                counts[y0:y1, x0:x1] += selected
        result[..., channel] = np.divide(sums, counts, out=np.zeros_like(raw), where=counts != 0)
        observed = channels == channel
        result[..., channel][observed] = raw[observed]
    return result


def decode_srgb(np, encoded):
    return np.where(encoded <= 0.04045, encoded / 12.92, ((encoded + 0.055) / 1.055) ** 2.4)


def encode_srgb(np, linear):
    return np.where(linear <= 0.0031308, 12.92 * linear, 1.055 * np.maximum(linear, 0) ** (1 / 2.4) - 0.055)


def scores(np, rgb, truth, mask):
    error = rgb.astype(np.float64)[mask] - truth.astype(np.float64)[mask]
    encoded_error = encode_srgb(np, rgb.astype(np.float64))[mask] - encode_srgb(np, truth.astype(np.float64))[mask]
    chroma = np.column_stack((error[:, 0] - error[:, 1], error[:, 2] - error[:, 1]))
    return {"pixel_count": int(error.shape[0]), "linear_rgb_rmse": float(np.sqrt(np.mean(error * error))),
            "linear_rgb_max_abs": float(np.max(np.abs(error))),
            "srgb_encoded_rmse_unclipped": float(np.sqrt(np.mean(encoded_error * encoded_error))),
            "color_difference_rmse": float(np.sqrt(np.mean(chroma * chroma)))}


def verify_bilinear(np):
    for pattern in range(4):
        fixture = h.generate(h.PROBES[-1], layout="shifted", pattern=pattern, phase=(1, 0), levels="per-site")
        ref = scalar.BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
        ox, oy, width, height = ref.bounds
        raw = np.asarray([[ref.observed[ox + x, oy + y] for x in range(width)] for y in range(height)], dtype=np.float32)
        channels = np.asarray([[ref.colors[ox + x, oy + y] for x in range(width)] for y in range(height)])
        actual = bilinear(np, raw, channels)
        expected = [ref.bilinear((ox + x, oy + y), c) for y in range(height) for x in range(width) for c in range(3)]
        if not np.array_equal(actual.ravel(), np.asarray(expected, dtype=np.float32)):
            raise ValueError("array baseline differs from native-v1 scalar order")


def evaluate(source, runtime, asset_root, reference_dir, board_path, progress=None):
    if h.digest(ASSETS.read_bytes()) != ASSET_HASH or h.digest(PROTOCOL.read_bytes()) != PROTOCOL_HASH:
        raise ValueError("frozen photo assets/protocol differ")
    manifest, protocol = json.loads(ASSETS.read_bytes()), json.loads(PROTOCOL.read_bytes())
    np, modules, helper_hash = upstream.load_upstream(source, runtime)
    from scipy.ndimage import gaussian_filter
    from PIL import Image, ImageDraw, __version__ as pillow_version
    verify_bilinear(np)
    reference_dir.mkdir(parents=True, exist_ok=True)
    reference_index, cases, visual_candidates = [], [], {}
    algorithms = protocol["algorithms"]
    for asset in manifest["assets"]:
        path = asset_root / asset["file"]
        if path.parent.resolve() != asset_root.resolve() or h.digest(path.read_bytes()) != asset["sha256"]:
            raise ValueError("photo path/hash differs")
        image = Image.open(path)
        embedded = image.info.get("icc_profile")
        unexpected_profile = embedded and h.digest(embedded) != protocol["allowed_embedded_srgb_profile_sha256"]
        if list(image.size) != asset["size_width_height"] or image.mode != "RGB" or unexpected_profile or image.getexif().get(274, 1) != 1:
            raise ValueError("unexpected photo shape/profile/orientation; specify conversion before scoring")
        width, height = image.size
        for qy in range(2):
            for qx in range(2):
                x, y = width * (1 + 2 * qx) // 4 - 64, height * (1 + 2 * qy) // 4 - 64
                encoded = np.asarray(image.crop((x, y, x + 128, y + 128)), dtype=np.float64) / 255
                original_linear = decode_srgb(np, encoded)
                for prefilter in protocol["prefilters"]:
                    truth = (gaussian_filter(original_linear, (prefilter["sigma"], prefilter["sigma"], 0), mode="mirror", truncate=3)
                             if prefilter["sigma"] else original_linear).astype(np.float32)
                    for pattern_index, pattern in enumerate(protocol["cfa_patterns"]):
                        yy, xx = np.indices(truth.shape[:2])
                        channels = np.asarray(h.CFA[pattern_index])[(yy % 2) * 2 + xx % 2]
                        observed_truth = np.take_along_axis(truth, channels[..., None], axis=2)[..., 0].astype(np.float64)
                        codes = np.floor(4000 + 15000 * observed_truth + 0.5).astype(np.uint16)
                        raw = ((codes.astype(np.float64) - 4000) / 15000).astype(np.float32)
                        arrays = {"engine_bilinear_v1": bilinear(np, raw, channels),
                                  "upstream_malvar": modules["malvar"].demosaicing_CFA_Bayer_Malvar2004(raw.astype(np.float64), pattern).astype(np.float32),
                                  "upstream_menon_base": modules["menon"].demosaicing_CFA_Bayer_Menon2007(raw.astype(np.float64), pattern, False).astype(np.float32),
                                  "upstream_menon_refined": modules["menon"].demosaicing_CFA_Bayer_Menon2007(raw.astype(np.float64), pattern, True).astype(np.float32)}
                        for name, halo in (("base", 6), ("refined", 8)):
                            adapted = arrays["engine_bilinear_v1"].copy()
                            adapted[halo:-halo, halo:-halo] = arrays["upstream_menon_" + name][halo:-halo, halo:-halo]
                            arrays["menon_" + name + "_engine_border"] = adapted
                        interior = np.zeros((128, 128), dtype=bool)
                        interior[8:-8, 8:-8] = True
                        masks = {"whole": np.ones_like(interior), "common_interior": interior, "border_band": ~interior}
                        case_id = f"{asset['id']}_q{qy}{qx}_{prefilter['label']}_{pattern}"
                        metrics = {}
                        for name in algorithms:
                            rgb = arrays[name]
                            if not np.isfinite(rgb).all():
                                raise ValueError("nonfinite reconstructed output")
                            fidelity = float(np.max(np.abs(np.take_along_axis(rgb, channels[..., None], axis=2)[..., 0] - raw)))
                            if fidelity != 0:
                                raise ValueError("observed sample changed: " + name)
                            metrics[name] = {scope: scores(np, rgb, truth, mask) for scope, mask in masks.items()}
                            metrics[name]["observed_fidelity_max_abs"] = fidelity
                            metrics[name]["output_sha256_f32le"] = h.digest(rgb.astype("<f4").tobytes())
                        cases.append({"case_id": case_id, "asset_id": asset["id"], "roi_xywh": [x, y, 128, 128],
                                      "prefilter": prefilter["label"], "pattern": pattern,
                                      "truth_sha256_f32le": h.digest(truth.astype("<f4").tobytes()),
                                      "codes_sha256_u16le": h.digest(codes.astype("<u2").tobytes()),
                                      "raw_sha256_f32le": h.digest(raw.astype("<f4").tobytes()), "metrics": metrics})
                        paths = {"raw": reference_dir / (case_id + ".raw.f32"),
                                 "base": reference_dir / (case_id + ".base.f32"),
                                 "refined": reference_dir / (case_id + ".refined.f32")}
                        for name, destination in paths.items():
                            pixels = raw if name == "raw" else arrays["upstream_menon_" + name]
                            data = pixels.astype("<f4").tobytes()
                            if destination.exists() and destination.read_bytes() != data:
                                raise ValueError("refusing to replace different reference output")
                            destination.write_bytes(data)
                        reference_index.append({"case_id": case_id, "width": 128, "height": 128, "pattern": pattern_index,
                                                "files": {name: path.name for name, path in paths.items()}})
                        # Visualization selection depends only on baseline difficulty.
                        if prefilter["label"] == "none" and pattern == "RGGB":
                            difficulty = metrics["engine_bilinear_v1"]["common_interior"]["linear_rgb_rmse"]
                            if asset["id"] not in visual_candidates or difficulty > visual_candidates[asset["id"]][0]:
                                visual_candidates[asset["id"]] = (difficulty, case_id, truth.copy(), {k: v.copy() for k, v in arrays.items()})
                        if progress and len(cases) % 32 == 0:
                            progress(len(cases))
        image.close()
    if len(cases) != 160:
        raise ValueError("frozen study requires five photos x four ROIs x two prefilters x four patterns")
    summaries = {}
    for prefilter in protocol["prefilters"]:
        selected = [c for c in cases if c["prefilter"] == prefilter["label"]]
        by_image = {}
        for asset in manifest["assets"]:
            group = [c for c in selected if c["asset_id"] == asset["id"]]
            by_image[asset["id"]] = {name: {scope: float(np.mean([c["metrics"][name][scope]["linear_rgb_rmse"] for c in group])) for scope in masks} for name in algorithms}
        means = {name: {scope: float(np.mean([v[name][scope] for v in by_image.values()])) for scope in masks} for name in algorithms}
        screens = {}
        for name in algorithms:
            ratios = {asset: values[name]["common_interior"] / values["engine_bilinear_v1"]["common_interior"] for asset, values in by_image.items()}
            improvement = 1 - means[name]["common_interior"] / means["engine_bilinear_v1"]["common_interior"]
            improving = sum(ratio < 1 for ratio in ratios.values())
            cap_ok = all(ratio <= 1.05 for ratio in ratios.values())
            screens[name] = {"mean_rmse_improvement_fraction": improvement, "source_images_improving": improving,
                             "source_image_regression_cap_passed": cap_ok, "per_image_rmse_ratio": ratios,
                             "research_continuation_supported": improvement >= 0.10 and improving >= 4 and cap_ok}
        summaries[prefilter["label"]] = {"case_count": len(selected), "per_image_mean_rmse": by_image, "equal_image_mean_rmse": means, "research_screen": screens}
    columns = ["reference", "engine_bilinear_v1", "upstream_malvar", "upstream_menon_base", "upstream_menon_refined"]
    board = Image.new("RGB", (len(columns) * 270, len(manifest["assets"]) * 300 + 45), "white")
    draw = ImageDraw.Draw(board)
    for i, name in enumerate(columns):
        draw.text((i * 270 + 5, 8), name, fill="black")
    for row, asset in enumerate(manifest["assets"]):
        _, case_id, truth, arrays = visual_candidates[asset["id"]]
        for col, name in enumerate(columns):
            rgb = truth if name == "reference" else arrays[name]
            visible = np.floor(np.clip(encode_srgb(np, rgb.astype(np.float64)), 0, 1) * 255 + 0.5).astype(np.uint8)
            tile = Image.fromarray(visible).resize((256, 256), Image.Resampling.NEAREST)
            board.paste(tile, (col * 270, row * 300 + 45))
        draw.text((5, row * 300 + 307), case_id + " | " + asset["author"], fill="black")
    board_path.parent.mkdir(parents=True, exist_ok=True)
    board.save(board_path)
    scalar.write_report(reference_dir / "index.json", {"reference_schema_version": 1, "source_commit": upstream.COMMIT, "cases": reference_index})
    return {"photo_remosaic_report_version": 1, "case_count": len(cases), "asset_count": 5,
            "protocol_sha256": PROTOCOL_HASH, "assets_sha256": ASSET_HASH, "source_commit": upstream.COMMIT,
            "upstream_files_sha256": upstream.FILES, "dependencies": {**upstream.crosscheck.VERSIONS, "Pillow": pillow_version},
            "helper_source_sha256_lf": {Path(module.__file__).name: scalar.source_digest(module) for module in (upstream, scalar, h)},
            "tool_source_sha256_lf": scalar.source_digest(sys.modules[__name__]), "colour_array_helper_sha256": helper_hash,
            "scope": "Rendered sRGB photographs, synthetic Bayer sampling and optional blur; exploratory research only, not representative-camera RAW or production acceptance.",
            "replacement_policy_v1_changed": False, "native_replacement_accepted": False,
            "cases": cases, "summaries": summaries,
            "visualization_selection": {asset: value[1] for asset, value in visual_candidates.items()},
            "contact_sheet_sha256": h.digest(board_path.read_bytes())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "runtime", "assets", "references", "board", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    result = evaluate(args.source, args.runtime, args.assets, args.references, args.board, progress=lambda n: print(f"{n}/160 photographic remosaicing cases completed", flush=True))
    scalar.write_report(args.output, result)
    print(json.dumps({label: {name: screen["research_continuation_supported"] for name, screen in summary["research_screen"].items()} for label, summary in result["summaries"].items()}, sort_keys=True))


if __name__ == "__main__":
    main()
