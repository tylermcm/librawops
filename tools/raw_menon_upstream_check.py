"""Optional isolated BSD upstream cross-check; never imported by the engine/tests.

Requires an explicitly supplied ignored research runtime and exact pinned source
and license bytes. Native acceptance and visual-quality gates are separate.
"""
import argparse
import importlib.metadata
import importlib.util
import itertools
import json
from pathlib import Path
import sys

import raw_menon_reference as m
import raw_quality_harness as h

SOURCE_SHA256 = "5621bde89d89f85962ac19afc47ad0dc63d962d7f11a3895d9a60d3aa0f8fbf2"
LICENSE_SHA256 = "62f431963cc2e7387c118c0798bcb323a1e0c0129b28265099ab74bf2a0c04af"
VERSIONS = {"numpy": "2.3.5", "scipy": "1.16.3", "colour-science": "0.4.6", "colour-demosaicing": "0.2.6"}


def fixture(pattern, phase, origin):
    ox, oy = origin
    w, height = 33, 31
    sw, sh, stride = ox + w + 2, oy + height + 2, ox + w + 5
    black = (1024, 1536, 2048, 2560)
    metadata = {"row_stride_samples": stride, "pattern": pattern, "cfa_phase_x": phase[0], "cfa_phase_y": phase[1],
                "active_x": ox, "active_y": oy, "active_width": w, "active_height": height,
                "black_levels": black, "white_levels": tuple(b + 4096 for b in black)}
    samples = [65535] * (stride * sh)
    for y in range(height):
        for x in range(w):
            site, _ = h.site_channel(metadata, ox + x, oy + y)
            # Original signed/headroom dyadic data: no initial stage rounding or
            # classifier ambiguity can be explained by input conversion here.
            code = ((17 * x + 29 * y + 3 * x * y + 11) % 449 - 64) * 16
            samples[(oy + y) * stride + ox + x] = black[site] + code
    return samples, sw, sh, metadata


def check(source, license_path, runtime):
    if h.digest(source.read_bytes()) != SOURCE_SHA256 or h.digest(license_path.read_bytes()) != LICENSE_SHA256:
        raise ValueError("upstream source/license differ from reviewed snapshot")
    sys.path.insert(0, str(runtime.resolve()))
    for name, version in VERSIONS.items():
        if importlib.metadata.version(name) != version:
            raise ValueError("cross-check requires exact pinned research dependencies")
    import numpy as np
    import colour_demosaicing.bayer as bayer_helpers
    import colour.utilities as colour_helpers
    spec = importlib.util.spec_from_file_location("menon_pinned_bsd_upstream", source)
    upstream = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(upstream)
    cases = []
    for pattern, px, py, origin in itertools.product(range(4), (0, 1), (0, 1), ((0, 0), (3, 5))):
        args = fixture(pattern, (px, py), origin)
        ref = m.BayerReference(*args)
        ox, oy, w, height = ref.bounds
        raw = np.array([[ref.observed[ox + x, oy + y] for x in range(w)] for y in range(height)], dtype=np.float64)
        effective_pattern = "".join("RGB"[ref.colors[ox + x, oy + y]] for y in range(2) for x in range(2))
        magnitude = float(np.max(np.abs(raw)))
        # Dyadic observations and base expressions are exactly representable.
        # For refinement, G1 <= 6M, color-at-green <= 8M, final opposite <= 15M.
        # Its stage-rounding propagation is <34M*u32; 64M*u32 plus binary64
        # slack is an independent conservative cross-precision envelope.
        bound = 64 * magnitude * 2**-24 + 256 * magnitude * 2**-53 + 8 * 2**-150
        results = {}
        for name, refine, halo in (("base", False, 6), ("refined", True, 8)):
            theirs = upstream.demosaicing_CFA_Bayer_Menon2007(raw, pattern=effective_pattern, refining_step=refine)
            ours = ref.render((ox + halo, oy + halo, w - 2 * halo, height - 2 * halo), refine=refine)
            cropped = np.asarray(theirs[halo:height - halo, halo:w - halo], dtype=np.float32)
            difference = np.abs(cropped.astype(np.float64).ravel() - np.asarray(ours.pixels, dtype=np.float64))
            error = float(np.max(difference))
            if error > (bound if refine else 0):
                raise ValueError(f"upstream interior disagreement: {pattern}/{px}/{py}/{origin}/{name}: {error}")
            for p, c in ref.colors.items():
                if float(theirs[p[1] - oy, p[0] - ox, c]) != ref.observed[p]:
                    raise ValueError("upstream changed an observed sample")
            results[name] = {"compared_channel_count": len(ours.pixels), "max_abs": error,
                             "rounding_bound": bound if refine else 0,
                             "ours_float32_sha256": h.digest(h.little_bytes("f", ours.pixels)),
                             "upstream_float32_sha256": h.digest(h.little_bytes("f", cropped.ravel().tolist())),
                             "passed": True}
        cases.append({"pattern": pattern, "phase": [px, py], "active_origin": list(origin),
                      "effective_upstream_pattern": effective_pattern,
                      "bayer_sha256": h.digest(h.little_bytes("H", args[0])), "variants": results})
    helper_files = {"colour_masks": Path(bayer_helpers.masks_CFA_Bayer.__code__.co_filename),
                    "colour_utilities": Path(colour_helpers.as_float_array.__code__.co_filename)}
    return {"upstream_check_schema_version": 1, "kind": "external-interior-cross-check",
            "source_commit": "7bff324983fb77b41444fda3bf922e354d386d1c",
            "upstream_source_sha256": SOURCE_SHA256, "upstream_license_sha256": LICENSE_SHA256,
            "reference_source_sha256_lf": m.source_digest(m),
            "check_source_sha256_lf": h.digest(Path(__file__).read_bytes().replace(b"\r\n", b"\n")),
            "dependencies": VERSIONS, "helper_source_sha256": {k: h.digest(v.read_bytes()) for k, v in helper_files.items()},
            "scope": "Exact dyadic base and bounded float32 refinement interior; upstream borders differ. No camera/quality/native/shipment admission.",
            "case_count": len(cases), "cases": cases, "passed": True, "native_replacement_accepted": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("license", type=Path)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = check(args.source, args.license, args.runtime)
    m.write_report(args.output, result)
    print(f"{result['case_count']} upstream metadata cases passed; both variants")


if __name__ == "__main__":
    main()
