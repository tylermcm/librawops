"""Opt-in Windows guided-filter resource studies against an installed engine."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_report(path):
    def unique_keys(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key: " + key)
            result[key] = value
        return result

    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_keys)


def require(value, message):
    if not value:
        raise ValueError(message)


def expanded_pixels(x, y, width, height, image_width, image_height, halo):
    return (min(image_width, x + width + halo) - max(0, x - halo)) * (
        min(image_height, y + height + halo) - max(0, y - halo)
    )


def verify_payload(case, source_shape):
    scale = 1 << case["mip"]
    width = (source_shape[0] + scale - 1) // scale
    height = (source_shape[1] + scale - 1) // scale
    mode = case["mode"]
    tile_size = 64 if mode == "image64" else 256
    rects = [(0, 0, width, height)] if mode == "direct" else [
        (x, y, min(tile_size, width - x), min(tile_size, height - y))
        for y in range(0, height, tile_size)
        for x in range(0, width, tile_size)
    ]
    source_bytes = coefficients = max_payload = 0
    is_source = case.get("preset") == "source"
    for x, y, rw, rh in rects:
        inputs = expanded_pixels(x, y, rw, rh, width, height, 2 * case["radius"])
        centers = expanded_pixels(x, y, rw, rh, width, height, case["radius"]) if case["radius"] else 0
        source_bytes += inputs * 12
        coefficients += centers
        max_payload = max(max_payload, inputs * 12 + centers * 56 + (0 if is_source else rw * rh * 12))
    is_frame = source_shape == (1025, 769)
    expected = {
        "output_pixels": width * height,
        "source_bytes" if is_frame else "derived_source_returned_bytes": source_bytes,
        "coefficient_centers" if is_frame else "derived_coefficient_centers": coefficients,
        "derived_node_logical_bytes" if is_frame else "max_node_logical_bytes": max_payload,
        "derived_renderer_retained_bytes" if is_frame else "renderer_retained_bytes": (
            width * height * 12 if mode in ("image", "image256", "image64") else 0
        ),
    }
    for key, value in expected.items():
        require(case[key] == value, "independent payload mismatch: " + key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--engine-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="fresh evidence directory")
    parser.add_argument("--study", choices=("frame", "threads", "45mp"), required=True)
    args = parser.parse_args()
    require(os.name == "nt", "this harness uses Windows process counters")
    build = args.build_dir.resolve()
    engine = args.engine_dir.resolve()
    output = args.output_dir.resolve()
    dll = engine / "bin/RawEngine.dll"
    study = {"frame": "Frame", "threads": "Thread", "45mp": "45MP"}[args.study]
    exe = build / ("GuidedFilter" + study + ".exe")
    require(dll.is_file() and exe.is_file(), "installed engine or built benchmark is missing")
    source = Path(__file__).resolve().parent
    files = [dll, exe, Path(__file__).resolve(), source / "CMakeLists.txt", source / (study + ".cpp")]
    bindings = {str(path): sha256(path) for path in files}
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env["PATH"] = str(dll.parent) + os.pathsep + env.get("PATH", "")
    started = time.perf_counter()
    reports = []
    output_hashes = {}

    def run(arguments, report_path):
        subprocess.run([str(exe), *map(str, arguments)], env=env, check=True)
        report = read_report(report_path)
        require(report["complete"], "native study did not complete")
        loaded = report.get("loaded_dll")
        require(loaded and Path(loaded).resolve() == dll, "unexpected native DLL was loaded")
        reports.append((report_path, report))
        return report

    if args.study == "frame":
        native_output = output / "native"
        report = run([native_output], native_output / "measurements.json")
        expected = {"fixtures": 96, "exact_repeated_outputs": 288, "cancellation_checks": 48,
                    "helper_node_comparisons": 24, "independent_support_checks": 2904}
        for key, value in expected.items():
            require(report[key] == value, "unexpected frame count: " + key)
        require(len(report["cases"]) == 96, "frame case count")
        for case in report["cases"]:
            verify_payload(case, (1025, 769))
            path = native_output / case["output"]
            require(path.stat().st_size == case["output_pixels"] * 12, "frame output size")
            output_hashes[str(path.relative_to(output))] = sha256(path)
        require(len(output_hashes) == 24, "frame output file count")
    elif args.study == "threads":
        report = run([output / "measurements.json"], output / "measurements.json")
        require(report["case_count"] == len(report["cases"]) == 54, "thread case count")
        require(report["measured_batches"] == 378 and report["exact_outputs_including_warmup"] == 3456,
                "thread exact batch/output counts")
        for case in report["cases"]:
            require(case["fixed_tasks"] == 8 and len(case["samples_ms"]) == 7, "fixed thread workload")
            require(all(1 <= n <= case["workers"] for n in case["observed_peak_calls"]), "thread overlap bound")
            require(case["upper_concurrent_node_logical_bytes"] ==
                    case["max_single_node_logical_bytes"] * case["workers"], "thread logical bound")
    else:
        for space in (0, 1):
            for preset in ("source", "r0", "r3", "r8-mip2", "r8"):
                for mode in ("stream", "image"):
                    path = output / (str(space) + "-" + preset + "-" + mode + ".json")
                    report = run([space, preset, mode, path], path)
                    require((report["space"], report["preset"], report["mode"]) ==
                            (space, preset, mode), "45MP case identity")
                    require(report["source_shape"] == [7200, 6250] and
                            report["selected_corner_tiles_exact"] == 4, "45MP shape/corners")
                    verify_payload(report, (7200, 6250))
        for space in (0, 1):
            def digest(preset, mode):
                return next(report["tile_order_output_sha256"] for _, report in reports
                            if (report["space"], report["preset"], report["mode"]) == (space, preset, mode))
            for preset in ("source", "r0", "r3", "r8-mip2", "r8"):
                require(digest(preset, "stream") == digest(preset, "image"), "45MP stream/image hash")
            require(digest("source", "stream") == digest("r0", "stream"), "45MP identity hash")

    for path, expected_hash in bindings.items():
        require(sha256(Path(path)) == expected_hash, "source/native binding changed: " + path)
    result = {
        "complete": True, "study": args.study, "files_sha256": bindings,
        "native_reports_sha256": {str(path.relative_to(output)): sha256(path) for path, _ in reports},
        "output_files_sha256": output_hashes, "elapsed_workflow_seconds": time.perf_counter() - started,
        "limits": "Read README.md for timing, logical/process memory, hashing, cancellation and qualification scope.",
    }
    with (output / "verification.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print("Verified " + args.study + " study: " + str(output / "verification.json"), flush=True)


if __name__ == "__main__":
    main()
