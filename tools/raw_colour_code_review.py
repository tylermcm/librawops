"""Optional pinned Colour Science comparison, using an isolated research runtime.

This measures a declared 164-case subset, not native or replacement acceptance.
Upstream sources stay in ignored research storage; no engine dependency is added.
"""
from __future__ import annotations

import argparse
import gzip
import importlib.metadata
import importlib.util
import inspect
import json
from pathlib import Path
import sys

import raw_menon_reference as m
import raw_menon_upstream_check as crosscheck
import raw_quality_compare as comparison
import raw_quality_harness as h

COMMIT = "7bff324983fb77b41444fda3bf922e354d386d1c"
FILES = {
    "LICENSE": crosscheck.LICENSE_SHA256,
    "colour_demosaicing/bayer/demosaicing/menon2007.py": crosscheck.SOURCE_SHA256,
    "colour_demosaicing/bayer/demosaicing/malvar2004.py": "4e1b2e70841864bc91c3b1eab129d4303eb89ee7eb71140954f56e80dc01c020",
    "colour_demosaicing/bayer/demosaicing/bilinear.py": "b3c59f6c2f614dfc0be55214510d551d4dd5ed9cbce04e4e16701425562b0b03",
    "colour_demosaicing/bayer/masks.py": "b14808eff640a77fffa1d9dc5a0c6896cc7d7ad9807a56daa32dd04187ac26d2",
    "colour_demosaicing/bayer/mosaicing.py": "b9ae917de419c7ba01f85b4eae0d328148d63c18051d71801c7154ca45d02221",
    "colour_demosaicing/bayer/demosaicing/tests/test_menon2007.py": "3ff152959bae02eca4d916ec04f7fc2ef0edb1584b1b284c4f6c437d4eeb768b",
    "colour_demosaicing/bayer/demosaicing/tests/test_malvar2004.py": "734c736101d85d0882eac95089cd6d1068d892724101b32294fd6a2db929a06f",
    "colour_demosaicing/bayer/demosaicing/tests/test_bilinear.py": "4c65940de3da660e2254fb454cfe33e4942ed5e37f9fac31422245d9c1136e61",
    "colour_demosaicing/examples/examples_bayer.ipynb": "9faf3595acb29a42802dc2f5b049fc1617b74496dd9e81531e7b3b4b21d280a0",
}
EXTRA_PROBES = {"neutral_mid", "neutral_affine", "chromatic_affine", "chromatic_sine_1_8_x"}


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_upstream(source, runtime):
    for relative, expected in FILES.items():
        if h.digest((source / relative).read_bytes()) != expected:
            raise ValueError("unreviewed upstream source/license: " + relative)
    sys.path.insert(0, str(runtime.resolve()))
    for name, version in crosscheck.VERSIONS.items():
        if importlib.metadata.version(name) != version:
            raise ValueError("requires pinned isolated runtime: " + name)
    import numpy as np
    import colour.utilities as utilities
    import colour_demosaicing.bayer as bayer
    masks = load_module("review_masks", source / "colour_demosaicing/bayer/masks.py")
    bayer.masks_CFA_Bayer = masks.masks_CFA_Bayer
    modules = {name: load_module("review_" + name, source / ("colour_demosaicing/bayer/demosaicing/" + filename))
               for name, filename in (("bilinear", "bilinear.py"), ("malvar", "malvar2004.py"), ("menon", "menon2007.py"))}
    if utilities.as_float_array([0.0]).dtype != np.float64:
        raise ValueError("review requires upstream default float64")
    return np, modules, h.digest(Path(utilities.as_float_array.__code__.co_filename).read_bytes())


def selected(case):
    info, meta = case["fixture"], case["fixture"]["metadata"]
    ordinary = info["layout"] == "packed" and info["level_policy"] == "uniform" and not meta["cfa_phase_x"] and not meta["cfa_phase_y"]
    extra = info["name"] in EXTRA_PROBES and info["layout"] == "shifted" and info["level_policy"] == "per-site" and meta["cfa_phase_x"] == 1 and meta["cfa_phase_y"] == 0
    return ordinary or extra


def traced_base(function, raw, pattern):
    # Observe the exact original code after its mask assignment; no equations change.
    lines, first = inspect.getsourcelines(function)
    target = first + next(i for i, line in enumerate(lines) if line.strip() == "mask = d_V >= d_H") + 1
    captured = {}

    def trace(frame, event, arg):
        if frame.f_code is function.__code__ and event == "line" and frame.f_lineno == target:
            captured.update({key: frame.f_locals[key].copy() for key in ("mask", "d_H", "d_V")})
        return trace

    previous = sys.gettrace()
    try:
        sys.settrace(trace)
        pixels = function(raw, pattern, refining_step=False)
    finally:
        sys.settrace(previous)
    if set(captured) != {"mask", "d_H", "d_V"}:
        raise ValueError("upstream classifier trace was not captured")
    return pixels, captured


def image(np, array, origin):
    rounded = np.asarray(array, dtype=np.float32)
    return h.RgbImage(rounded.shape[1], rounded.shape[0], tuple(float(v) for v in rounded.ravel()), origin)


def difference(np, first, second, halo):
    delta = np.abs(first[halo:-halo, halo:-halo].astype(np.float64) - second[halo:-halo, halo:-halo].astype(np.float64))
    yy, xx, channel = (int(v) for v in np.unravel_index(np.argmax(delta), delta.shape))
    return {"halo": halo, "channel_count": int(delta.size), "max_abs": float(delta.max()),
            "rmse": float(np.sqrt(np.mean(delta * delta))), "exact_channel_count": int(np.count_nonzero(delta == 0)),
            "worst_active_local_xy_channel": [xx + halo, yy + halo, channel]}


def review(source, runtime, baseline_path, scalar_path, progress=None):
    np, modules, helper_digest = load_upstream(source, runtime)
    baseline, scalar = h.read_baseline(baseline_path), json.loads(gzip.decompress(scalar_path.read_bytes()))
    h.validate_baseline(baseline)
    if scalar["reference_source_sha256_lf"] != m.source_digest(m) or scalar["baseline_report_sha256"] != h.digest(h.report_json(baseline).encode()):
        raise ValueError("scalar/baseline provenance differs")
    policy = json.loads(comparison.POLICY_PATH.read_text())
    old_scalar = {name: {tuple(c["case"]): c for c in variant["cases"]} for name, variant in scalar["variants"].items()}
    scorers = {name: m.Scores(name.startswith("menon_refined"), policy) for name in
               ("bilinear", "malvar", "menon_base", "menon_refined", "menon_base_engine_border", "menon_refined_engine_border")}
    probes = {p.name: p for p in h.ALL_PROBES}
    cases = []
    for old in filter(selected, baseline["cases"]):
        info, meta, key = old["fixture"], old["fixture"]["metadata"], comparison.case_key(old)
        fixture = h.generate(probes[info["name"]], layout=info["layout"], pattern=meta["pattern"],
                             phase=(meta["cfa_phase_x"], meta["cfa_phase_y"]), levels=info["level_policy"])
        if (h.digest(fixture.bayer_le) != info["bayer"]["sha256"] or
                h.digest(h.little_bytes("f", fixture.truth.pixels)) != info["ground_truth"]["sha256"] or
                comparison.canonical(fixture.metadata) != comparison.canonical(meta)):
            raise ValueError("fixture differs from preserved evidence")
        reference = m.BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
        ox, oy, w, height = reference.bounds
        raw = np.array([[reference.observed[ox + x, oy + y] for x in range(w)] for y in range(height)], dtype=np.float64)
        pattern = "".join("RGB"[reference.colors[ox + x, oy + y]] for y in range(2) for x in range(2))
        native = np.array([reference.bilinear((ox + x, oy + y), c) for y in range(height) for x in range(w) for c in range(3)], dtype=np.float32).reshape(height, w, 3)
        if h.digest(h.little_bytes("f", native.ravel())) != old["rendered_float32_le_sha256"]:
            raise ValueError("local bilinear differs from preserved native baseline")
        menon_base, trace = traced_base(modules["menon"].demosaicing_CFA_Bayer_Menon2007, raw, pattern)
        arrays = {"bilinear": modules["bilinear"].demosaicing_CFA_Bayer_bilinear(raw, pattern),
                  "malvar": modules["malvar"].demosaicing_CFA_Bayer_Malvar2004(raw, pattern),
                  "menon_base": menon_base,
                  "menon_refined": modules["menon"].demosaicing_CFA_Bayer_Menon2007(raw, pattern)}
        paired = {}
        for name, refine, halo in (("base", False, 6), ("refined", True, 8)):
            ours = reference.render(refine=refine)
            if h.digest(h.little_bytes("f", ours.pixels)) != old_scalar[name][key]["rendered_float32_le_sha256"]:
                raise ValueError("regenerated scalar differs from preserved study")
            ours_array = np.asarray(ours.pixels, dtype=np.float32).reshape(height, w, 3)
            upstream = arrays["menon_" + name].astype(np.float32)
            aligned = native.copy()
            aligned[halo:-halo, halo:-halo] = upstream[halo:-halo, halo:-halo]
            arrays["menon_" + name + "_engine_border"] = aligned
            common = (ox + 8, oy + 8, w - 16, height - 16)
            paired[name] = {"interior_difference": difference(np, upstream, ours_array, halo),
                            "ours_common_interior": h.measure(ours, fixture.truth, common, neutral=fixture.probe.neutral),
                            "upstream_common_interior": h.measure(image(np, upstream, (ox, oy)), fixture.truth, common, neutral=fixture.probe.neutral)}
        disagreements = []
        direction_count = 0
        for y in range(8, height - 8):
            for x in range(8, w - 8):
                p = (ox + x, oy + y)
                if reference.colors[p] == 1:
                    continue
                direction_count += 1
                theirs, ours = 0 if trace["mask"][y, x] else 1, reference.direction(p)
                if theirs != ours:
                    disagreements.append({"active_local_xy": [x, y], "ours_direction": ours, "upstream_direction": theirs,
                                          "ours_classifiers": reference.classifiers(p),
                                          "upstream_classifiers": [float(trace["d_H"][y, x]), float(trace["d_V"][y, x])]})
        for name, array in arrays.items():
            scorers[name].add(old, fixture, image(np, array, (ox, oy)))
        cases.append({"case": key, "bayer_sha256": info["bayer"]["sha256"], "truth_sha256": info["ground_truth"]["sha256"],
                      "effective_upstream_pattern": pattern, "paired_menon": paired,
                      "classified_rb_count_common_interior": direction_count,
                      "direction_disagreement_count": len(disagreements), "first_direction_disagreements": disagreements[:4]})
        if progress and len(cases) % 16 == 0:
            progress(len(cases))
    if len(cases) != 164:
        raise ValueError("expected declared 148 ordinary plus 16 metadata controls")
    variants = {}
    for name, scorer in scorers.items():
        report = scorer.report()
        report["research_identity"] = {"algorithm": "colour-science." + name, "source_commit": COMMIT,
                                       "upstream_float64_stages": True, "output_cast": "float32",
                                       "engine_border_adapter": name.endswith("_engine_border")}
        variants[name] = report
    controls = []
    for shape in ((5, 5), (1, 5), (5, 1), (1, 1)):
        raw = np.ones(shape, dtype=np.float64)
        record = {"shape_height_width": shape, "results": {}}
        for name in ("bilinear", "malvar", "menon_base", "menon_refined"):
            function = {"bilinear": modules["bilinear"].demosaicing_CFA_Bayer_bilinear,
                        "malvar": modules["malvar"].demosaicing_CFA_Bayer_Malvar2004}.get(name)
            try:
                rgb = function(raw, "RGGB") if function else modules["menon"].demosaicing_CFA_Bayer_Menon2007(raw, "RGGB", refining_step=name == "menon_refined")
                record["results"][name] = {"returned_shape": list(rgb.shape), "top_left_rgb": [float(v) for v in rgb[0, 0]], "max_abs_constant_error": float(np.max(np.abs(rgb - 1)))}
            except (ValueError, IndexError, TypeError) as error:
                record["results"][name] = {"error_type": type(error).__name__, "message": str(error)}
        controls.append(record)
    return {"code_review_schema_version": 1, "source_commit": COMMIT, "upstream_files_sha256": FILES,
            "dependencies": crosscheck.VERSIONS, "colour_array_helper_sha256": helper_digest,
            "tool_source_sha256_lf": m.source_digest(sys.modules[__name__]),
            "local_reference_sha256_lf": m.source_digest(m),
            "helper_source_sha256_lf": {Path(module.__file__).name: m.source_digest(module) for module in (h, comparison, crosscheck)},
            "baseline_json_sha256": h.digest(h.report_json(baseline).encode()), "scalar_archive_sha256": h.digest(scalar_path.read_bytes()),
            "policy_sha256": h.digest(comparison.POLICY_PATH.read_bytes()), "case_count": len(cases),
            "selection": "37 probes x four CFA patterns, packed/uniform/phase00; four declared probes x four CFA patterns, shifted/per-site/phase10",
            "scope": "Code comparison on 164 selected synthetic pairs. Original upstream borders and separate engine-border adapters; no full-policy, camera, performance, native or shipment acceptance.",
            "native_replacement_accepted": False, "cases": cases, "variants": variants, "shape_and_constant_controls": controls}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "runtime", "baseline", "scalar", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    result = review(args.source, args.runtime, args.baseline, args.scalar, progress=lambda n: print(f"{n}/164 code-comparison fixtures completed", flush=True))
    m.write_report(args.output, result)
    print(json.dumps({"cases": result["case_count"], "direction_disagreements": sum(c["direction_disagreement_count"] for c in result["cases"]),
                      "max_interior_difference": {name: max(c["paired_menon"][name]["interior_difference"]["max_abs"] for c in result["cases"]) for name in ("base", "refined")}}, sort_keys=True))


if __name__ == "__main__":
    main()
