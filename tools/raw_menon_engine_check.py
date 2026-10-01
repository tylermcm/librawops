"""Check the engine Menon base against all frozen photographic upstream planes.

Uses standard Python only plus the built native extension. Original photos and
scientific packages are unnecessary once the verified reference planes exist.
"""
import argparse
import array
import hashlib
import json
from pathlib import Path
import platform
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(data): return hashlib.sha256(data).hexdigest()


def check(module_dir, references, photo_report):
    sys.path.insert(0, str(module_dir.resolve()))
    import rawengine_native as raw
    photo = json.loads(photo_report.read_bytes())
    index = json.loads((references / "index.json").read_bytes())
    old = {c["case_id"]: c for c in photo["cases"]}
    if len(index["cases"]) != 160 or len(old) != 160:
        raise ValueError("unexpected photographic reference count")
    cases = []
    policy = {"algorithm": "rawengine.menon_base", "processing_version": 1}
    started = time.perf_counter()
    for case in index["cases"]:
        saved = old[case["case_id"]]
        data = (references / case["files"]["raw"]).read_bytes()
        expected = (references / case["files"]["base"]).read_bytes()
        if digest(data) != saved["raw_sha256_f32le"] or digest(expected) != saved["metrics"]["upstream_menon_base"]["output_sha256_f32le"]:
            raise ValueError("reference hash differs")
        observations = array.array("f"); observations.frombytes(data)
        codes = array.array("H", (int(v * 15000 + 4000 + 0.5) for v in observations))
        if digest(codes.tobytes()) != saved["codes_sha256_u16le"]:
            raise ValueError("recovered sensor codes differ")
        w, h = case["width"], case["height"]
        stride, sw, sh, ox, oy = w + 11, w + 6, h + 6, 3, 3
        sensor = array.array("H", [65535]) * (stride * sh)
        for row in range(h): sensor[(row + oy) * stride + ox:(row + oy) * stride + ox + w] = codes[row * w:(row + 1) * w]
        metadata = {"row_stride_samples": stride, "pattern": case["pattern"], "cfa_phase_x": 1, "cfa_phase_y": 1,
                    "active_x": ox, "active_y": oy, "active_width": w, "active_height": h,
                    "black_levels": (4000,) * 4, "white_levels": (19000,) * 4}
        session = raw.RawSession(sensor, sw, sh, metadata, cache_bytes=0, workers=2, demosaic=policy)
        manifest = json.loads(session.export_manifest({})); source_id = manifest["sources"][0]["id"]
        manifest["operations"] = []; manifest["output"] = source_id; text = json.dumps(manifest)
        for size in (512, 29, 7):
            output = session.render_manifest(text, {"tile_size": size})
            if output != (w, h, expected): raise ValueError(f"engine parity failed: {case['case_id']}/tile{size}")
        for x, y, rw, rh in ((0, 0, 11, 9), (17, 13, 15, 11)):
            request = {"x": ox + x, "y": oy + y, "roi_width": rw, "roi_height": rh, "tile_size": 3}
            crop = b"".join(expected[((y + row) * w + x) * 12:((y + row) * w + x + rw) * 12] for row in range(rh))
            if session.render_manifest(text, request) != (rw, rh, crop): raise ValueError("engine ROI differs")
            left, top = max(0, x - 6), max(0, y - 6)
            footprint = (ox + left, oy + top, min(w, x + rw + 6) - left, min(h, y + rh + 6) - top)
            if session.required_source_regions(text, request) != {source_id: footprint}: raise ValueError("engine footprint differs")
        session.close()
        cases.append({"case_id": case["case_id"], "channel_count": w * h * 3, "full_tile_sizes": [512, 29, 7],
                      "roi_checks": 2, "source_footprint_checks": 2, "mean_squared_error": 0.0, "max_abs": 0.0,
                      "all_outputs_bit_exact": True, "expected_sha256_f32le": digest(expected),
                      "sensor_sha256_u16le": digest(sensor.tobytes())})
        if len(cases) % 32 == 0: print(f"{len(cases)}/160 engine photo cases passed", flush=True)
    bound_files = ("MenonDemosaic.cpp", "MenonDemosaic.hpp", "RawEngine.cpp", "RawEngine.hpp", "CMakeLists.txt", "RawEnginePython.cpp",
                   "tests/raw_menon_native_tests.cpp", "tests/python_raw_menon_tests.py", "third_party/colour-demosaicing/LICENSE")
    return {"engine_check_version": 1, "demosaic": policy, "photo_report_sha256": digest(photo_report.read_bytes()),
            "photo_source_commit": photo["source_commit"], "protocol_sha256": photo["protocol_sha256"],
            "fixture_count": len(cases), "full_image_checks": len(cases) * 3, "roi_checks": len(cases) * 2,
            "source_footprint_checks": len(cases) * 2, "upstream_channel_count_per_full_pass": sum(c["channel_count"] for c in cases),
            "mean_squared_error": 0.0, "rmse": 0.0, "max_abs": 0.0, "cases": cases,
            "engine_files_sha256": {f: digest((ROOT / f).read_bytes()) for f in bound_files},
            "checker_sha256_lf": digest(Path(__file__).read_bytes().replace(b"\r\n", b"\n")),
            "engine_dll_sha256": digest((module_dir / "RawEngine.dll").read_bytes()),
            "python_extension_sha256": digest(Path(raw.__file__).read_bytes()),
            "python": platform.python_version(), "duration_seconds": time.perf_counter() - started,
            "all_checks_passed": True, "camera_quality_certified": False,
            "scope": "Native Menon base, padded uint16 sensors/shifted phase, all 160 photographic goldens; full/tile/ROI/halo exactness. Photographic remosaicing, not camera RAW acceptance."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("module_dir", "references", "photo_report", "output"): parser.add_argument(name, type=Path)
    args = parser.parse_args()
    report = check(args.module_dir, args.references, args.photo_report)
    data = (json.dumps(report,sort_keys=True,indent=2,allow_nan=False) + "\n").encode()
    if args.output.exists() and args.output.read_bytes() != data: raise ValueError("refusing to overwrite different engine evidence")
    args.output.write_bytes(data)
    print(json.dumps({k: v for k, v in report.items() if k not in ("cases", "engine_files_sha256")}, indent=2))


if __name__ == "__main__": main()
