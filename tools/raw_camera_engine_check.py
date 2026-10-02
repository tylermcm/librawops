"""Local camera study against the built engine; no analytical RGB ground truth.

NumPy/Pillow are optional research dependencies. Decoder-derived color is only
a diagnostic view, not a measured profile or an Adobe/camera JPEG match.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

from raw_camera_extract import sha256_file


def camera_matrix(record):
    """Diagnostic D65-normalized camera basis, adapted to XYZ D50.

    Decoder cam_xyz is an XYZ-to-camera basis. Normalize its response to D65
    neutral before inverting, rather than applying it in the wrong direction.
    This is not an illuminant-dependent camera profile or calibration oracle.
    """
    import numpy as np
    xyz_d65 = np.array([[.4124564, .3575761, .1804375], [.2126729, .7151522, .0721750],
                        [.0193339, .1191920, .9503041]], dtype=np.float64)
    xyz_d50 = np.array([[.4360747, .3850649, .1430804], [.2225045, .7168786, .0606169],
                        [.0139322, .0971045, .7141733]], dtype=np.float64)
    basis = np.array(record["decoder_rgb_xyz_matrix_uninterpreted"], dtype=np.float64)
    if basis.shape != (4, 3) or not np.isfinite(basis).all() or np.any(basis[3]):
        raise ValueError("unsupported decoder camera basis")
    response = basis[:3] @ xyz_d65
    neutral = response.sum(axis=1)
    if np.any(neutral <= 0) or np.linalg.cond(response) > 100:
        raise ValueError("singular/ill-conditioned camera basis")
    return (xyz_d50 @ np.linalg.inv(response / neutral[:, None])).astype(np.float32).reshape(-1).tolist()


def sensor_rois(metadata, reviewed=None):
    x, y, w, h = metadata["active_area"]
    if min(w, h) < 512:
        raise ValueError("initial camera study needs at least a 512x512 active rectangle")
    if reviewed is not None:
        if not isinstance(reviewed, list) or len(reviewed) != 4:
            raise ValueError("four reviewed 256x256 sensor ROIs required")
        for roi in reviewed:
            if (not isinstance(roi, list) or len(roi) != 4 or any(type(v) is not int for v in roi)
                    or roi[2:] != [256, 256] or roi[0] < x or roi[1] < y
                    or roi[0] + 256 > x + w or roi[1] + 256 > y + h
                    or (roi[0] - x) % 4 or (roi[1] - y) % 4):
                raise ValueError("reviewed ROIs must be confined and align to native/mip2 cells")
        return reviewed
    # Active-relative multiples of four let native and mip-2 pixels cover exactly
    # the same photosites. First ROI includes actual top/left sensor boundaries.
    return [[x, y, 256, 256]] + [[x + int((w - 256) * fx) // 4 * 4,
                                 y + int((h - 256) * fy) // 4 * 4, 256, 256]
                                for fx, fy in ((.5, .5), (.75, .25), (.25, .75))]


def pixels(result):
    import numpy as np
    w, h, data = result
    return np.frombuffer(data, dtype="<f4").reshape(h, w, 3)


def native_metadata(meta):
    result = {k: v for k, v in meta.items() if k not in ("width", "height", "active_area")}
    result.update(zip(("active_x", "active_y", "active_width", "active_height"), meta["active_area"]))
    return result


def request(roi, tile=256):
    return dict(zip(("x", "y", "roi_width", "roi_height"), roi), tile_size=tile)


def observed_check(output, codes, meta, roi):
    import numpy as np
    x, y, w, h = roi
    yy, xx = np.mgrid[y:y + h, x:x + w]
    sites = (yy % 2) * 2 + xx % 2
    patterns = ((0, 1, 1, 2), (2, 1, 1, 0), (1, 0, 2, 1), (1, 2, 0, 1))
    colors = np.take(patterns[meta["pattern"]], sites)
    black = np.take(np.array(meta["black_levels"], dtype=np.float32), sites)
    white = np.take(np.array(meta["white_levels"], dtype=np.float32), sites)
    expected = (codes[y:y + h, x:x + w].astype(np.float32) - black) / (white - black)
    observed = np.take_along_axis(pixels(output), colors[:, :, None], axis=2)[:, :, 0]
    if observed.tobytes() != expected.tobytes():
        raise ValueError("camera observed photosites differ from independent normalization")
    return int(expected.size)


def check(module_dir, folder, extraction, output):
    import numpy as np
    from PIL import Image, ImageDraw
    sys.path.insert(0, str(module_dir.resolve()))
    import rawengine_native as raw
    folder, output = folder.resolve(), output.resolve()
    output.relative_to(folder)
    output.mkdir(parents=True, exist_ok=True)
    doc = json.loads(extraction.read_text(encoding="utf-8"))
    if doc["local_extraction_schema_version"] != 1:
        raise ValueError("unsupported extraction schema")
    cases = []
    for record in doc["assets"]:
        meta = record["metadata"]
        original, decoded = (folder / record[k]["path"] for k in ("original", "decoded"))
        for path, field in ((original, "original"), (decoded, "decoded")):
            path.resolve().relative_to(folder)
            if sha256_file(path) != record[field]["sha256"]:
                raise ValueError("camera input hash mismatch")
        if decoded.stat().st_size != meta["row_stride_samples"] * meta["height"] * 2:
            raise ValueError("decoded sensor size mismatch")
        codes = np.memmap(decoded, dtype="<u2", mode="r", shape=(meta["height"], meta["row_stride_samples"]))
        wb = record["camera_whitebalance"]
        if len(wb) != 4 or min(wb) <= 0 or wb[1] != wb[3]:
            raise ValueError("as-shot WB requires an unsupported dual-green treatment")
        gains = [wb[i] / wb[1] for i in range(3)]
        recipe = dict(zip(("red_gain", "green_gain", "blue_gain"), gains),
                      exposure_stops=0.0, camera_to_xyz_d50=camera_matrix(record),
                      working_space="prophoto-d50", output_mode="srgb-preview")
        rois = sensor_rois(meta, record.get("inspection_rois"))
        result = {"id": record["id"], "original_sha256": record["original"]["sha256"],
                  "decoded_sha256": record["decoded"]["sha256"], "metadata": meta,
                  "view_recipe": recipe, "rois": rois, "algorithms": {}}
        board = Image.new("RGB", (536, 4 * 286), "#202020")
        draw = ImageDraw.Draw(board)
        for column, algorithm in enumerate(("rawengine.bilinear", "rawengine.menon_base")):
            session = raw.RawSession(codes, meta["width"], meta["height"], native_metadata(meta),
                                     cache_bytes=0, workers=1,
                                     demosaic={"algorithm": algorithm, "processing_version": 1})
            try:
                source = json.loads(session.export_manifest({}))
                source["operations"] = []
                source["output"] = source["sources"][0]["id"]
                source_text = json.dumps(source)
                linear = json.loads(session.export_manifest(recipe))
                end = next(i for i, op in enumerate(linear["operations"]) if op["type"] == "rawengine.camera_to_working")
                linear["operations"] = linear["operations"][:end + 1]
                linear["output"] = linear["operations"][-1]["id"]
                linear_text = json.dumps(linear)
                checks = []
                for row, roi in enumerate(rois):
                    native = session.render_manifest(source_text, request(roi))
                    if session.render_manifest(source_text, request(roi, 37)) != native:
                        raise ValueError("camera tile partition changed pixels")
                    if session.submit_manifest(source_text, request(roi, 29)).result(timeout=60) != native:
                        raise ValueError("camera async ROI changed pixels")
                    observed_count = observed_check(native, codes, meta, roi)
                    x, y, w, h = roi
                    ax, ay, aw, ah = meta["active_area"]
                    halo = 6 if algorithm.endswith("menon_base") else 1
                    left, top = max(ax, x - halo), max(ay, y - halo)
                    footprint = (left, top, min(ax + aw, x + w + halo) - left,
                                 min(ay + ah, y + h + halo) - top)
                    if session.required_source_regions(source_text, request(roi)) != {session.source_info()["id"]: footprint}:
                        raise ValueError("camera footprint differs from declared halo")
                    calibrated = pixels(session.render_manifest(linear_text, request(roi)))
                    expected = calibrated.reshape(h // 4, 4, w // 4, 4, 3).mean(axis=(1, 3), dtype=np.float64).astype(np.float32)
                    reduced_roi = [(x - ax) // 4, (y - ay) // 4, w // 4, h // 4]
                    preview = pixels(session.render_manifest(linear_text, {**request(reduced_roi, 31), "mip": 2, "quality": "preview"}))
                    error = float(np.max(np.abs(expected - preview)))
                    if error > 1e-6:
                        raise ValueError("camera preview did not average native calibrated pixels")
                    display = pixels(session.render({**recipe, **request(roi)}))
                    image = Image.fromarray(np.rint(np.clip(display, 0, 1) * 255).astype(np.uint8))
                    board.paste(image, (column * 268, row * 286 + 25))
                    draw.text((column * 268, row * 286 + 5), algorithm.split(".")[-1] + f" ROI {row}", fill="white")
                    checks.append({"roi": roi, "source_rgb_f32_sha256": hashlib.sha256(native[2]).hexdigest(),
                                   "observed_channels_exact": observed_count, "tile_async_footprint_pass": True,
                                   "preview_max_abs_error": error,
                                   "signed_channel_count": int(np.count_nonzero(pixels(native) < 0)),
                                   "headroom_channel_count": int(np.count_nonzero(pixels(native) > 1))})
                start = time.perf_counter()
                full = session.render({**recipe, "mip": 2, "quality": "preview", "tile_size": 256})
                elapsed = time.perf_counter() - start
                image = Image.fromarray(np.rint(np.clip(pixels(full), 0, 1) * 255).astype(np.uint8))
                orientation = record["capture"]["orientation"]
                if orientation in (3, 6, 8):
                    image = image.rotate({3: 180, 6: 270, 8: 90}[orientation], expand=True)
                name = record["id"] + "-" + algorithm.split(".")[-1] + "-mip2.png"
                image.save(output / name)
                result["algorithms"][algorithm] = {"source_info": session.source_info(), "roi_checks": checks,
                    "full_mip2_dimensions": list(full[:2]), "full_mip2_seconds": elapsed,
                    "full_mip2_f32_sha256": hashlib.sha256(full[2]).hexdigest(),
                    "view_path": (output / name).relative_to(folder).as_posix()}
                print(f"{record['id']} {algorithm}: 4 ROI checks pass; full mip2 {elapsed:.3f}s", flush=True)
            finally:
                session.close()
        board_path = output / (record["id"] + "-native-crops.png")
        board.save(board_path)
        result["crop_board"] = {"path": board_path.relative_to(folder).as_posix(), "sha256": sha256_file(board_path)}
        cases.append(result)
        # Persist completed cases so an interrupted study remains reviewable.
        report = {"camera_engine_study_version": 1, "extraction_sha256": sha256_file(extraction),
                  "native_module_sha256": sha256_file(Path(raw.__file__)),
                  "view_limit": "decoder-derived diagnostic matrix and as-shot WB; no measured camera profile, no automatic exposure, no denoise/sharpening; original sensor coordinates; clipping only in encoded views",
                  "analytical_truth_available": False, "production_quality_accepted": False,
                  "cases": cases}
        (output / "engine-check-v1.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("module_dir", "folder", "extraction", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    check(args.module_dir, args.folder, args.extraction, args.output)
