"""Independent Hamilton-Adams scalar research reference; not a native backend.

Original implementation from the equation figures in US5629734, printed pages
5-7. No third-party implementation code is used. Full-active-plane storage is
intentional for an oracle, not the proposed native tile scratch contract.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys

import raw_camera_corpus as corpus
import raw_quality_harness as h
import raw_quality_compare as comparison

REFERENCE_VERSION = 1
MAX_SAMPLES = 1_000_000


def select_direction(first, second):
    """Each argument is (classifier, predictor); exact ties average predictors."""
    if first[0] < second[0]:
        return first[1]
    if second[0] < first[0]:
        return second[1]
    return (first[1] + second[1]) * 0.5


def green_direction(center, far_a, far_b, near_a, near_b):
    laplacian = (2.0 * center - far_a) - far_b
    return abs(laplacian) + abs(near_a - near_b), (near_a + near_b) * 0.5 + laplacian * 0.25


def chroma_direction(center_green, green_a, green_b, color_a, color_b):
    laplacian = (2.0 * center_green - green_a) - green_b
    return abs(laplacian) + abs(color_a - color_b), (color_a + color_b) * 0.5 + laplacian * 0.5


class BayerReference:
    """Copy a bounded uint16 sensor fixture and reconstruct without native code.

    Strict metadata uses the harness's integer per-site levels and coordinates.
    Every render request is independent of tile boundaries. The green plane is
    prepared from observations once; final border fallback does not replace it.
    """
    def __init__(self, samples, width, height, metadata):
        required = {"row_stride_samples", "pattern", "cfa_phase_x", "cfa_phase_y",
                    "active_x", "active_y", "active_width", "active_height", "black_levels", "white_levels"}
        if not isinstance(metadata, dict) or set(metadata) != required:
            raise ValueError("missing/unknown scalar sensor metadata")
        meta = dict(metadata)
        for name in ("black_levels", "white_levels"):
            if not isinstance(meta[name], (tuple, list)):
                raise ValueError("four integer site levels required")
            meta[name] = tuple(meta[name])
        area = [meta.pop("active_" + name) for name in ("x", "y", "width", "height")]
        check = dict(meta, width=width, height=height, active_area=area)
        check["black_levels"], check["white_levels"] = list(meta["black_levels"]), list(meta["white_levels"])
        count = corpus.sensor_metadata(check) // 2
        if count > MAX_SAMPLES:
            raise ValueError("scalar research fixture exceeds sample budget")
        if len(samples) != count:
            raise ValueError("sensor buffer size mismatch")
        owned = tuple(samples)
        if any(type(v) is not int or not 0 <= v <= 65535 for v in owned):
            raise ValueError("uint16 integer sensor samples required")
        self.metadata = dict(metadata, black_levels=meta["black_levels"], white_levels=meta["white_levels"])
        self.bounds = tuple(area)
        self.width, self.height = area[2:]
        ox, oy = area[:2]
        self.observed = {}
        self.colors = {}
        for y in range(oy, oy + self.height):
            for x in range(ox, ox + self.width):
                site, channel = h.site_channel(self.metadata, x, y)
                self.observed[x, y] = h.f32((owned[y * meta["row_stride_samples"] + x] - meta["black_levels"][site]) /
                                          (meta["white_levels"][site] - meta["black_levels"][site]))
                self.colors[x, y] = channel
        self.green = {}
        for x, y in self.observed:
            if self.colors[x, y] == 1:
                value = self.observed[x, y]
            elif self.margin(x, y) >= 2:
                s = self.observed
                value = h.f32(select_direction(
                    green_direction(s[x, y], s[x - 2, y], s[x + 2, y], s[x - 1, y], s[x + 1, y]),
                    green_direction(s[x, y], s[x, y - 2], s[x, y + 2], s[x, y - 1], s[x, y + 1])))
            else:
                value = self.bilinear(x, y, 1)
            self.green[x, y] = value

    def margin(self, x, y):
        ox, oy, w, height = self.bounds
        return min(x - ox, y - oy, ox + w - 1 - x, oy + height - 1 - y)

    def bilinear(self, x, y, channel):
        if self.colors[x, y] == channel:
            return self.observed[x, y]
        total, count = 0.0, 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                p = (x + dx, y + dy)
                if self.colors.get(p) == channel:
                    total = h.f32(total + self.observed[p])
                    count += 1
        return h.f32(total / count) if count else 0.0

    def pixel(self, x, y):
        if self.margin(x, y) < 3:
            return tuple(self.bilinear(x, y, ch) for ch in range(3))
        result = []
        for channel in range(3):
            if self.colors[x, y] == channel:
                value = self.observed[x, y]
            elif channel == 1:
                value = self.green[x, y]
            elif self.colors[x, y] == 1:
                dx, dy = (1, 0) if self.colors[x + 1, y] == channel else (0, 1)
                a, b = (x - dx, y - dy), (x + dx, y + dy)
                value = h.f32(chroma_direction(self.green[x, y], self.green[a], self.green[b],
                                               self.observed[a], self.observed[b])[1])
            else:
                a, b, c, d = (x - 1, y - 1), (x + 1, y + 1), (x + 1, y - 1), (x - 1, y + 1)
                value = h.f32(select_direction(
                    chroma_direction(self.green[x, y], self.green[a], self.green[b], self.observed[a], self.observed[b]),
                    chroma_direction(self.green[x, y], self.green[c], self.green[d], self.observed[c], self.observed[d])))
            result.append(value)
        return tuple(result)

    def render(self, bounds=None):
        bounds = self.bounds if bounds is None else tuple(bounds)
        if len(bounds) != 4 or any(type(v) is not int or v < 0 for v in bounds):
            raise ValueError("invalid scalar ROI")
        x, y, w, height = bounds
        ox, oy, aw, ah = self.bounds
        if x < ox or y < oy or x + w > ox + aw or y + height > oy + ah:
            raise ValueError("scalar ROI outside active area")
        return h.RgbImage(w, height, tuple(v for yy in range(y, y + height) for xx in range(x, x + w)
                                         for v in self.pixel(xx, yy)), (x, y))


def case_metrics(fixture, rendered):
    x, y, w, height = fixture.truth.bounds
    metrics = {"whole": h.measure(rendered, fixture.truth, neutral=fixture.probe.neutral),
               "interior": h.measure(rendered, fixture.truth, (x + 1, y + 1, w - 2, height - 2), neutral=fixture.probe.neutral),
               **h.observed_metrics(fixture, rendered)}
    if isinstance(fixture.probe, h.EdgeProbe):
        metrics["edge_band"] = h.measure(rendered, fixture.truth, neutral=fixture.probe.neutral,
                                         selection=h.edge_band_selection(fixture.truth, fixture.probe))
    if isinstance(fixture.probe, h.ImpulseProbe):
        px, py = fixture.probe.position
        neighborhood = (x + px - 2, y + py - 2, 5, 5)
        surround = tuple((xx, yy) != (px, py) for yy in range(height) for xx in range(w))
        metrics["impulse_center"] = h.measure(rendered, fixture.truth, (x + px, y + py, 1, 1), neutral=fixture.probe.neutral)
        metrics["impulse_neighborhood"] = h.measure(rendered, fixture.truth, neighborhood, neutral=fixture.probe.neutral)
        metrics["impulse_surround"] = h.measure(rendered, fixture.truth, neighborhood, neutral=fixture.probe.neutral, selection=surround)
    return metrics


def evaluate(baseline, *, quick=False):
    """Pair research scores with immutable evidence without forging native reports."""
    h.validate_baseline(baseline)
    policy = json.loads(comparison.POLICY_PATH.read_text(encoding="utf-8"))
    probes = {p.name: p for p in h.ALL_PROBES}
    cases, regressions, failures, groups = [], [], [], {}
    for old in baseline["cases"]:
        info = old["fixture"]
        meta = info["metadata"]
        if quick and (meta["pattern"] or meta["cfa_phase_x"] or meta["cfa_phase_y"]):
            continue
        fixture = h.generate(probes[info["name"]], layout=info["layout"], pattern=meta["pattern"],
                             phase=(meta["cfa_phase_x"], meta["cfa_phase_y"]), levels=info["level_policy"])
        if (h.digest(fixture.bayer_le) != info["bayer"]["sha256"] or
                h.digest(h.little_bytes("f", fixture.truth.pixels)) != info["ground_truth"]["sha256"] or
                comparison.canonical(fixture.metadata) != comparison.canonical(meta)):
            raise ValueError("generated research fixture differs from preserved evidence")
        rendered = BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata).render()
        metrics = case_metrics(fixture, rendered)
        comparison.metric_shape_and_values(old["metrics"], metrics)
        key = comparison.case_key(old)
        bound = old["acceptance"]["quantization_max_bound"]
        scope = old["acceptance"]["analytical_scope"]
        gates = {"quantization": metrics["quantization"]["observed"]["max_abs"] <= bound + policy["quantization_roundoff_allowance"],
                 "observed_fidelity": metrics["observed_fidelity"]["observed"]["max_abs"] <= policy["observed_fidelity_max_limit"],
                 "analytical": metrics[scope]["rgb"]["max_abs"] <= bound + policy["analytical_roundoff_allowance"] if scope else None}
        failures.extend({"case": key, "gate": g} for g, value in gates.items() if value is False)
        if metrics["quantization"] != old["metrics"]["quantization"]:
            raise ValueError("observed quantization differs from preserved evidence")
        if not scope:
            for name, metric in old["metrics"].items():
                if "rgb" not in metric:
                    continue
                for score in policy["regression_scores"]:
                    before, after = comparison.score(metric, score), comparison.score(metrics[name], score)
                    if before is None:
                        continue
                    limit = before * (1 + policy["per_case_regression_fraction"]) + policy["absolute_regression_allowance"]
                    if after > limit:
                        regressions.append({"case": key, "scope": name, "score": score, "baseline": before, "reference": after, "limit": limit})
                    if name == policy["improvement_scopes"].get(info["family"]) and score in policy["improvement_scores"]:
                        group = groups.setdefault((info["name"], name, score), [[], []])
                        group[0].append(before); group[1].append(after)
        cases.append({"case": key, "rendered_float32_le_sha256": h.digest(h.little_bytes("f", rendered.pixels)),
                      "metrics": metrics, "measured_correctness": gates})
    improvements = []
    for (probe, scope, score), (old, new) in sorted(groups.items()):
        before, after = math.fsum(old) / len(old), math.fsum(new) / len(new)
        limit = before * (1 - policy["improvement_fraction"])
        improvements.append({"probe": probe, "scope": scope, "score": score, "pair_count": len(old),
                             "baseline_mean": before, "reference_mean": after, "limit": limit, "passed": after <= limit})
    return {"research_report_schema_version": 1, "reference_version": REFERENCE_VERSION,
            "reference_source_sha256": h.digest(Path(__file__).read_bytes()),
            "helper_source_sha256": {Path(m.__file__).name: h.digest(Path(m.__file__).read_bytes()) for m in (h, corpus, comparison)},
            "generator_version": h.GENERATOR_VERSION, "metric_formula_version": h.METRIC_VERSION,
            "measurement_domain": h.DOMAIN,
            "baseline_report_sha256": h.digest(h.report_json(baseline).encode()), "policy": policy,
            "scope": "scalar research only; no native integration, cache/history, preview, performance, camera or shipment acceptance",
            "metadata_sweep": "quick" if quick else "full", "case_count": len(cases), "cases": cases,
            "measured_gates": {"analytical_and_observed": not failures, "per_case_regression": not regressions,
                               "edge_detail_improvement": bool(improvements) and all(g["passed"] for g in improvements)},
            "native_replacement_accepted": False, "correctness_failures": failures,
            "regressions": regressions, "improvements": improvements}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--quick", action="store_true", help="pattern 0 / phase 0 only; not full coverage")
    args = parser.parse_args()
    result = evaluate(h.read_baseline(args.baseline), quick=args.quick)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(h.report_json(result), encoding="utf-8")
    print(f"{result['case_count']} scalar research cases; measured gates={result['measured_gates']}")
    return 0  # A successfully measured rejected candidate is still valid research evidence.


if __name__ == "__main__":
    sys.exit(main())
