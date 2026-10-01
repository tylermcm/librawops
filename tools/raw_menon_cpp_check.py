"""Validate the isolated C++ Menon prototype against pinned upstream research.

This checks implementation parity, not quality-policy or product admission.
Requires the optional, isolated scientific runtime used by the photo study.
"""
from __future__ import annotations

import argparse
import itertools
import json
from pathlib import Path
import platform
import subprocess
import sys

import raw_colour_code_review as upstream
import raw_menon_reference as scalar
import raw_photo_remosaic as photo
import raw_quality_harness as h

ROOT = Path(__file__).resolve().parents[1]
PATTERNS = ("RGGB", "BGGR", "GRBG", "GBRG")


def check(executable, source, runtime, references, photo_report, output_dir):
    np, modules, helper_hash = upstream.load_upstream(source, runtime)
    output_dir.mkdir(parents=True, exist_ok=True)
    oracle = modules["menon"].demosaicing_CFA_Bayer_Menon2007
    cases = []

    def channels(shape, pattern):
        grid = np.asarray(["RGB".index(c) for c in PATTERNS[pattern]]).reshape(2, 2)
        yy, xx = np.indices(shape)
        return grid[yy % 2, xx % 2]

    def invoke(raw, pattern, mode, roi=None):
        path, destination = output_dir / "input.f32", output_dir / "output.f32"
        raw.astype("<f4").tofile(path)
        command = [str(executable.resolve()), str(path), str(raw.shape[1]), str(raw.shape[0]),
                   str(pattern), mode, str(destination)]
        if roi is not None:
            command.extend(map(str, roi))
        completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if completed.returncode != 0:
            raise ValueError("C++ execution failed: " + completed.stderr)
        width, height = (raw.shape[1], raw.shape[0]) if roi is None else roi[2:]
        data = destination.read_bytes()
        if len(data) != width * height * 3 * 4:
            raise ValueError("wrong C++ output byte count")
        return np.frombuffer(data, dtype="<f4").reshape(height, width, 3), json.loads(completed.stdout)

    def compare(case_id, group, raw, pattern, mode, expected):
        actual, timing = invoke(raw, pattern, mode)
        if not np.isfinite(actual).all() or not np.isfinite(expected).all():
            raise ValueError("nonfinite comparison output")
        error = np.abs(actual.astype(np.float64) - expected.astype(np.float64))
        # Two adjacent float32 steps, with the predeclared float64 operation slack.
        magnitude = np.abs(expected)
        ulp = np.nextafter(magnitude, np.float32(np.inf)).astype(np.float64) - magnitude.astype(np.float64)
        slack = 64 * max(1.0, float(np.max(np.abs(raw.astype(np.float64))))) * np.finfo(np.float64).eps
        limit = 2 * ulp + slack
        selected = np.take_along_axis(actual, channels(raw.shape, pattern)[..., None], axis=2)[..., 0]
        observed_exact = bool(np.array_equal(selected.view(np.uint32), raw.view(np.uint32)))
        passed = bool(np.all(error <= limit)) and observed_exact
        exact = actual.view(np.uint32) == expected.view(np.uint32)
        cases.append({"case_id": case_id, "group": group, "mode": mode, "pattern": PATTERNS[pattern],
                      "shape_hw": list(raw.shape), "channel_count": int(actual.size),
                      "bit_exact_channel_count": int(np.count_nonzero(exact)), "max_abs": float(error.max()),
                      "channels_outside_tolerance": int(np.count_nonzero(error > limit)),
                      "observed_samples_bit_exact": observed_exact, "passed": passed,
                      "raw_sha256_f32le": h.digest(raw.astype("<f4").tobytes()),
                      "reference_sha256_f32le": h.digest(expected.astype("<f4").tobytes()),
                      "cpp_sha256_f32le": h.digest(actual.tobytes()), "reconstruction_ms": timing["reconstruction_ms"]})
        if not passed:
            where = np.unravel_index(np.argmax(error - limit), error.shape)
            raise ValueError(f"parity failure {case_id}/{mode} at {where}: {actual[where]} vs {expected[where]}")
        if len(cases) % 64 == 0:
            print(f"{len(cases)} C++ whole-plane parity comparisons passed", flush=True)
        return actual

    report = json.loads(photo_report.read_bytes())
    index_path = references / "index.json"
    index = json.loads(index_path.read_bytes())
    if (report["case_count"] != 160 or len(index["cases"]) != 160 or
            report["source_commit"] != upstream.COMMIT or index["source_commit"] != upstream.COMMIT or
            report["protocol_sha256"] != photo.PROTOCOL_HASH or report["assets_sha256"] != photo.ASSET_HASH or
            report["tool_source_sha256_lf"] != scalar.source_digest(photo)):
        raise ValueError("photographic reference provenance differs")
    recorded = {c["case_id"]: c for c in report["cases"]}
    for case in index["cases"]:
        old = recorded[case["case_id"]]
        raw_data = (references / case["files"]["raw"]).read_bytes()
        if h.digest(raw_data) != old["raw_sha256_f32le"]:
            raise ValueError("photo Bayer reference differs")
        raw = np.frombuffer(raw_data, dtype="<f4").reshape(case["height"], case["width"])
        for mode in ("base", "refined"):
            data = (references / case["files"][mode]).read_bytes()
            if h.digest(data) != old["metrics"]["upstream_menon_" + mode]["output_sha256_f32le"]:
                raise ValueError("photo RGB reference differs")
            expected = np.frombuffer(data, dtype="<f4").reshape(case["height"], case["width"], 3)
            compare(case["case_id"], "photographic", raw, case["pattern"], mode, expected)

    # Reuse exactly the earlier 164-case audit selection, including rejected probes.
    audit_path = ROOT / "tests/reference/raw/colour_code_review_v1.json.gz"
    import gzip
    audit = json.loads(gzip.decompress(audit_path.read_bytes()))
    probes = {p.name: p for p in h.ALL_PROBES}
    if len(audit["cases"]) != 164 or audit["source_commit"] != upstream.COMMIT:
        raise ValueError("unexpected code-audit selection")

    def normalized(samples, width, height, metadata):
        ref = scalar.BayerReference(samples, width, height, metadata)
        ox, oy, width, height = ref.bounds
        raw = np.asarray([[ref.observed[ox + x, oy + y] for x in range(width)] for y in range(height)], dtype=np.float32)
        pattern = "".join("RGB"[ref.colors[ox + x, oy + y]] for y in range(2) for x in range(2))
        return raw, PATTERNS.index(pattern)

    for old in audit["cases"]:
        name, layout, pattern, px, py, levels = old["case"]
        fixture = h.generate(probes[name], layout=layout, pattern=pattern, phase=(px, py), levels=levels)
        if h.digest(fixture.bayer_le) != old["bayer_sha256"]:
            raise ValueError("synthetic fixture differs from earlier audit")
        raw, effective = normalized(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
        if PATTERNS[effective] != old["effective_upstream_pattern"]:
            raise ValueError("effective CFA differs")
        for mode in ("base", "refined"):
            expected = oracle(raw, PATTERNS[effective], refining_step=mode == "refined").astype(np.float32)
            compare("/".join(map(str, old["case"])), "synthetic_audit", raw, effective, mode, expected)

    for pattern, phase, origin in itertools.product(range(4), ((0, 0), (1, 0), (0, 1), (1, 1)), ((0, 0), (3, 2))):
        args = upstream.crosscheck.fixture(pattern, phase, origin)
        raw, effective = normalized(*args)
        for mode in ("base", "refined"):
            expected = oracle(raw, PATTERNS[effective], refining_step=mode == "refined").astype(np.float32)
            compare(f"signed/{pattern}/{phase}/{origin}", "signed_headroom", raw, effective, mode, expected)

    for shape, pattern in itertools.product(((1, 1), (1, 7), (7, 1), (2, 2), (3, 5), (5, 3), (5, 5), (9, 11)), range(4)):
        yy, xx = np.indices(shape)
        fields = {"constant": np.full(shape, 0.375, dtype=np.float32),
                  "signed": (((17 * xx + 29 * yy + 3 * xx * yy) % 47 - 16) / 16).astype(np.float32)}
        for label, raw in fields.items():
            for mode in ("base", "refined"):
                thin = min(shape) == 1
                expected = photo.bilinear(np, raw, channels(shape, pattern)) if thin else oracle(raw, PATTERNS[pattern], refining_step=mode == "refined").astype(np.float32)
                compare(f"{shape}/{pattern}/{label}", "thin_fallback" if thin else "small_border", raw, pattern, mode, expected)

    roi_checks = []
    for pattern in range(4):
        case = next(c for c in index["cases"] if c["pattern"] == pattern)
        raw = np.fromfile(references / case["files"]["raw"], dtype="<f4").reshape(case["height"], case["width"])
        for mode in ("base", "refined"):
            full, _ = invoke(raw, pattern, mode)
            for roi in ((0, 0, 11, 9), (17, 13, 15, 11)):
                cropped, _ = invoke(raw, pattern, mode, roi)
                x, y, width, height = roi
                exact = cropped.tobytes() == full[y:y + height, x:x + width].tobytes()
                if not exact:
                    raise ValueError("ROI output differs from complete reconstruction")
                roi_checks.append({"pattern": PATTERNS[pattern], "mode": mode, "roi_xywh": roi, "bit_exact": exact})

    malformed = []
    valid = np.arange(25, dtype=np.float32).reshape(5, 5) / 16
    path, destination = output_dir / "invalid.f32", output_dir / "invalid-output.f32"
    controls = [
        ("short", valid.tobytes()[:-4], ["5", "5", "0", "base"], []),
        ("long", valid.tobytes() + b"0000", ["5", "5", "0", "base"], []),
        ("zero_width", valid.tobytes(), ["0", "5", "0", "base"], []),
        ("negative_width", valid.tobytes(), ["-5", "5", "0", "base"], []),
        ("fractional_width", valid.tobytes(), ["5.0", "5", "0", "base"], []),
        ("unknown_pattern", valid.tobytes(), ["5", "5", "4", "base"], []),
        ("unknown_mode", valid.tobytes(), ["5", "5", "0", "other"], []),
        ("sample_budget", valid.tobytes(), ["1001", "1000", "0", "base"], []),
        ("integer_overflow", valid.tobytes(), ["999999999999999999999999", "5", "0", "base"], []),
        ("roi_outside", valid.tobytes(), ["5", "5", "0", "base"], ["4", "4", "2", "2"]),
        ("roi_zero", valid.tobytes(), ["5", "5", "0", "base"], ["0", "0", "0", "2"]),
    ]
    for value, label in ((np.nan, "nan"), (np.inf, "positive_inf"), (-np.inf, "negative_inf")):
        raw = valid.copy(); raw[0, 0] = value
        controls.append((label, raw.tobytes(), ["5", "5", "0", "base"], []))
    for label, data, args, roi in controls:
        path.write_bytes(data)
        if destination.exists():
            destination.unlink()
        result = subprocess.run([str(executable.resolve()), str(path), *args, str(destination), *roi], capture_output=True, text=True, timeout=30)
        passed = result.returncode != 0 and not destination.exists()
        if not passed:
            raise ValueError("malformed input accepted: " + label)
        malformed.append({"case": label, "rejected_before_output": passed, "diagnostic": result.stderr.strip()})

    # A bounded throughput indication, not native tile performance or 45 MP timing.
    first = index["cases"][0]
    raw = np.fromfile(references / first["files"]["raw"], dtype="<f4").reshape(128, 128)
    large = np.tile(raw, (8, 8))[:1000, :1000].copy()
    timings = {}
    for mode in ("base", "refined"):
        expected = oracle(large, PATTERNS[first["pattern"]], refining_step=mode == "refined").astype(np.float32)
        actual = compare("1000x1000_photo_repeat", "one_megapixel", large, first["pattern"], mode, expected)
        milliseconds = [cases[-1]["reconstruction_ms"]]
        for _ in range(2):
            repeated, timing = invoke(large, first["pattern"], mode)
            if repeated.tobytes() != actual.tobytes():
                raise ValueError("repeated C++ result differs")
            milliseconds.append(timing["reconstruction_ms"])
        timings[mode] = {"reconstruction_ms": milliseconds, "median_ms": float(np.median(milliseconds)),
                         "repeat_output_bit_exact": True}

    groups = {}
    for group in sorted({c["group"] for c in cases}):
        selected = [c for c in cases if c["group"] == group]
        groups[group] = {"comparison_count": len(selected), "channel_count": sum(c["channel_count"] for c in selected),
                         "bit_exact_channel_count": sum(c["bit_exact_channel_count"] for c in selected),
                         "max_abs": max(c["max_abs"] for c in selected), "all_passed": all(c["passed"] for c in selected)}
    files = ("menon_research.cpp", "CMakeLists.txt", "README.md", "LICENSE.colour-demosaicing")
    return {"menon_cpp_check_version": 1, "source_commit": upstream.COMMIT,
            "upstream_files_sha256": upstream.FILES, "dependencies": upstream.crosscheck.VERSIONS,
            "colour_array_helper_sha256": helper_hash, "platform": platform.platform(), "processor": platform.processor(),
            "python": platform.python_version(), "executable_sha256": h.digest(executable.read_bytes()),
            "prototype_files_sha256": {f: h.digest((ROOT / "research/menon" / f).read_bytes()) for f in files},
            "tool_source_sha256_lf": scalar.source_digest(sys.modules[__name__]),
            "helper_source_sha256_lf": {Path(m.__file__).name: scalar.source_digest(m) for m in (upstream, scalar, photo, h)},
            "photo_report_sha256": h.digest(photo_report.read_bytes()), "photo_reference_index_sha256": h.digest(index_path.read_bytes()),
            "code_audit_archive_sha256": h.digest(audit_path.read_bytes()),
            "tolerance": "2 float32 ULPs + 64*max(1,max_abs_input)*epsilon64; observed channels bit exact",
            "scope": "Whole active planes including original borders; singleton fallback; crop equivalence only. No native engine/tile integration or quality admission.",
            "native_replacement_accepted": False, "comparison_count": len(cases), "groups": groups, "cases": cases,
            "roi_checks": roi_checks, "malformed_input_checks": malformed, "one_megapixel_research_timings": timings,
            "all_checks_passed": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("executable", "source", "runtime", "references", "photo_report", "scratch", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    report = check(args.executable, args.source, args.runtime, args.references, args.photo_report, args.scratch)
    scalar.write_report(args.output, report)
    print(json.dumps({"comparisons": report["comparison_count"], "groups": report["groups"],
                      "roi_checks": len(report["roi_checks"]), "malformed_inputs": len(report["malformed_input_checks"]),
                      "timings": report["one_megapixel_research_timings"], "all_checks_passed": True}, indent=2))


if __name__ == "__main__":
    main()
