"""Original scalar Menon research implementation from the frozen v1 equations.

Functional stage caches read unchanged earlier stages, never final-border
fallback. Bounded full-plane oracle storage is not a native tile design. No
third-party demosaicing code or scientific runtime is imported here.
"""
from __future__ import annotations

import argparse
import gzip
import json
import math
from pathlib import Path
import sys
from types import MappingProxyType

import raw_camera_corpus as corpus
import raw_ha_reference as metrics_helper
import raw_menon_support as support
import raw_quality_compare as comparison
import raw_quality_harness as h

REFERENCE_VERSION = 1
MAX_SAMPLES = 1_000_000
AXES = ((1, 0), (0, 1))
PRIMARY_PAPER_SHA256 = "4f207dffd4529b7df0453e71bbe958d311dc9a1619dfb8c6c2e200d3c6ecffbd"


def choose_direction(horizontal, vertical):
    return 1 if vertical < horizontal else 0


def green_predictor(center, far_a, far_b, near_a, near_b):
    return h.f32(0.5 * (near_a + near_b) + 0.25 * ((2.0 * center - far_a) - far_b))


def pair_difference(center, color_a, other_a, color_b, other_b):
    return h.f32(center + 0.5 * ((color_a - other_a) + (color_b - other_b)))


def refine_difference(center, delta_a, delta_center, delta_b, *, subtract=False):
    average = ((delta_a + delta_center) + delta_b) / 3.0
    return h.f32(center - average if subtract else center + average)


class BayerReference:
    """Own validated observations; evaluate an acyclic, immutable-stage oracle."""
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
        area = [meta["active_" + name] for name in ("x", "y", "width", "height")]
        check = {k: meta[k] for k in ("row_stride_samples", "pattern", "cfa_phase_x", "cfa_phase_y")}
        check.update(width=width, height=height, active_area=area,
                     black_levels=list(meta["black_levels"]), white_levels=list(meta["white_levels"]))
        count = corpus.sensor_metadata(check) // 2
        if count > MAX_SAMPLES:
            raise ValueError("scalar research fixture exceeds sample budget")
        if len(samples) != count:
            raise ValueError("sensor buffer size mismatch")
        owned = tuple(samples)
        if any(type(v) is not int or not 0 <= v <= 65535 for v in owned):
            raise ValueError("uint16 integer sensor samples required")
        self.metadata = MappingProxyType(meta)
        self.bounds = tuple(area)
        self.width, self.height = area[2:]
        ox, oy = area[:2]
        observed, colors = {}, {}
        for y in range(oy, oy + self.height):
            for x in range(ox, ox + self.width):
                site, channel = h.site_channel(meta, x, y)
                observed[x, y] = h.f32((owned[y * meta["row_stride_samples"] + x] - meta["black_levels"][site]) /
                                       (meta["white_levels"][site] - meta["black_levels"][site]))
                colors[x, y] = channel
        self.observed, self.colors = MappingProxyType(observed), MappingProxyType(colors)
        self._cache = {}

    def margin(self, p):
        ox, oy, w, height = self.bounds
        return min(p[0] - ox, p[1] - oy, ox + w - 1 - p[0], oy + height - 1 - p[1])

    def _require(self, p, radius):
        if p not in self.observed or self.margin(p) < radius:
            raise ValueError("internal stage requires complete active-area support")

    def _memo(self, key, evaluate):
        if key not in self._cache:
            self._cache[key] = evaluate()
        return self._cache[key]

    def directional_green(self, p, axis):
        if type(axis) is not int or axis not in (0, 1):
            raise ValueError("unknown directional axis")
        self._require(p, 0)
        if self.colors[p] == 1:
            return self.observed[p]
        self._require(p, 2)
        e, s = AXES[axis], self.observed
        return self._memo(("directional_green", p, axis), lambda: green_predictor(
            s[p], s[support.shifted(p, e, -2)], s[support.shifted(p, e, 2)],
            s[support.shifted(p, e, -1)], s[support.shifted(p, e, 1)]))

    def _gradient(self, p, axis):
        def evaluate():
            q = support.shifted(p, AXES[axis], 2)
            a = self.observed[p] - self.directional_green(p, axis)
            b = self.observed[q] - self.directional_green(q, axis)
            return abs(a - b)
        return self._memo(("gradient", p, axis), evaluate)

    def classifiers(self, p):
        self._require(p, 4)
        if self.colors[p] == 1:
            raise ValueError("classifiers require a red/blue site")
        def evaluate():
            values = []
            for axis, edges in enumerate((support.H_EDGES, support.V_EDGES)):
                total = 0.0
                for dx, dy, weight in edges:
                    total += weight * self._gradient((p[0] + dx, p[1] + dy), axis)
                values.append(total)
            return tuple(values)
        return self._memo(("classifiers", p), evaluate)

    def direction(self, p):
        return choose_direction(*self.classifiers(p))

    def green(self, p):
        self._require(p, 0)
        if self.colors[p] == 1:
            return self.observed[p]
        return self._memo(("green", p), lambda: self.directional_green(p, self.direction(p)))

    def _axis_at_green(self, p, c):
        return AXES[0] if self.colors[p[0] + 1, p[1]] == c else AXES[1]

    def color_at_green(self, p, c):
        self._require(p, 5)
        if self.colors[p] != 1 or type(c) is not int or c not in (0, 2):
            raise ValueError("green-site reconstruction requires missing red/blue")
        def evaluate():
            e = self._axis_at_green(p, c)
            a, b = support.shifted(p, e, -1), support.shifted(p, e, 1)
            return pair_difference(self.green(p), self.observed[a], self.green(a), self.observed[b], self.green(b))
        return self._memo(("color_at_green", p, c), evaluate)

    def base(self, p, c):
        self._require(p, 0)
        own = self.colors[p]
        if c == own:
            return self.observed[p]
        if c == 1:
            return self.green(p)
        if own == 1:
            return self.color_at_green(p, c)
        self._require(p, 6)
        def evaluate():
            e = AXES[self.direction(p)]
            a, b = support.shifted(p, e, -1), support.shifted(p, e, 1)
            return pair_difference(self.observed[p], self.color_at_green(a, c), self.color_at_green(a, own),
                                   self.color_at_green(b, c), self.color_at_green(b, own))
        return self._memo(("base", p, c), evaluate)

    def refined_green(self, p):
        self._require(p, 0)
        own = self.colors[p]
        if own == 1:
            return self.observed[p]
        self._require(p, 6)
        def evaluate():
            e = AXES[self.direction(p)]
            delta = [self.base(q, own) - self.green(q) for q in
                     (support.shifted(p, e, -1), p, support.shifted(p, e, 1))]
            return refine_difference(self.observed[p], *delta, subtract=True)
        return self._memo(("refined_green", p), evaluate)

    def after_green_locations(self, p, c):
        self._require(p, 0)
        own = self.colors[p]
        if c == own:
            return self.observed[p]
        if c == 1:
            return self.refined_green(p)
        if own != 1:
            return self.base(p, c)
        self._require(p, 7)
        def evaluate():
            e = self._axis_at_green(p, c)
            a, b = support.shifted(p, e, -1), support.shifted(p, e, 1)
            return pair_difference(self.refined_green(p), self.observed[a], self.refined_green(a),
                                   self.observed[b], self.refined_green(b))
        return self._memo(("refined_color_at_green", p, c), evaluate)

    def refined(self, p, c):
        self._require(p, 0)
        own = self.colors[p]
        if c == own or c == 1 or own == 1:
            return self.after_green_locations(p, c)
        self._require(p, 8)
        def evaluate():
            e = AXES[self.direction(p)]
            delta = [self.after_green_locations(q, c) - self.after_green_locations(q, own) for q in
                     (support.shifted(p, e, -1), p, support.shifted(p, e, 1))]
            return refine_difference(self.observed[p], *delta)
        return self._memo(("refined", p, c), evaluate)

    def bilinear(self, p, c):
        if self.colors[p] == c:
            return self.observed[p]
        total, count = 0.0, 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                q = (p[0] + dx, p[1] + dy)
                if self.colors.get(q) == c:
                    total = h.f32(total + self.observed[q])
                    count += 1
        return h.f32(total / count) if count else 0.0

    def pixel(self, x, y, *, refine=True):
        if type(refine) is not bool or type(x) is not int or type(y) is not int:
            raise ValueError("integer sensor coordinates and boolean refinement required")
        p = (x, y)
        self._require(p, 0)
        if self.margin(p) < (8 if refine else 6):
            return tuple(self.bilinear(p, c) for c in range(3))
        stage = self.refined if refine else self.base
        return tuple(stage(p, c) for c in range(3))

    def render(self, bounds=None, *, refine=True):
        if type(refine) is not bool:
            raise ValueError("refine must be boolean")
        bounds = self.bounds if bounds is None else tuple(bounds)
        if len(bounds) != 4 or any(type(v) is not int or v < 0 for v in bounds):
            raise ValueError("invalid scalar ROI")
        x, y, w, height = bounds
        ox, oy, aw, ah = self.bounds
        if x < ox or y < oy or x + w > ox + aw or y + height > oy + ah:
            raise ValueError("scalar ROI outside active area")
        return h.RgbImage(w, height, tuple(v for yy in range(y, y + height) for xx in range(x, x + w)
                                         for v in self.pixel(xx, yy, refine=refine)), (x, y))


class Scores:
    def __init__(self, refine, policy):
        self.refine, self.policy = refine, policy
        self.cases, self.failures, self.regressions, self.groups = [], [], [], {}

    def add(self, old, fixture, rendered):
        policy, info = self.policy, old["fixture"]
        metrics = metrics_helper.case_metrics(fixture, rendered)
        comparison.metric_shape_and_values(old["metrics"], metrics)
        key, bound, scope = comparison.case_key(old), old["acceptance"]["quantization_max_bound"], old["acceptance"]["analytical_scope"]
        gates = {"quantization": metrics["quantization"]["observed"]["max_abs"] <= bound + policy["quantization_roundoff_allowance"],
                 "observed_fidelity": metrics["observed_fidelity"]["observed"]["max_abs"] <= policy["observed_fidelity_max_limit"],
                 "analytical": metrics[scope]["rgb"]["max_abs"] <= bound + policy["analytical_roundoff_allowance"] if scope else None}
        self.failures.extend({"case": key, "gate": g} for g, v in gates.items() if v is False)
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
                        self.regressions.append({"case": key, "scope": name, "score": score,
                                                 "baseline": before, "reference": after, "limit": limit})
                    if name == policy["improvement_scopes"].get(info["family"]) and score in policy["improvement_scores"]:
                        group = self.groups.setdefault((info["name"], name, score), [[], []])
                        group[0].append(before)
                        group[1].append(after)
        self.cases.append({"case": key, "rendered_float32_le_sha256": h.digest(h.little_bytes("f", rendered.pixels)),
                           "metrics": metrics, "measured_correctness": gates})

    def report(self):
        improvements = []
        for (probe, scope, score), (old, new) in sorted(self.groups.items()):
            before, after = math.fsum(old) / len(old), math.fsum(new) / len(new)
            limit = before * (1 - self.policy["improvement_fraction"])
            improvements.append({"probe": probe, "scope": scope, "score": score, "pair_count": len(old),
                                 "baseline_mean": before, "reference_mean": after, "limit": limit, "passed": after <= limit})
        return {"research_identity": {"algorithm": "librawops.menon.scalar-research", "version": REFERENCE_VERSION,
                                      "refinement": self.refine, "float32_stages": True, "bilinear_border": 8 if self.refine else 6},
                "case_count": len(self.cases), "cases": self.cases,
                "measured_gates": {"analytical_and_observed": not self.failures, "per_case_regression": not self.regressions,
                                   "edge_detail_improvement": bool(improvements) and all(g["passed"] for g in improvements)},
                "native_replacement_accepted": False, "correctness_failures": self.failures,
                "regressions": self.regressions, "improvements": improvements}


def source_digest(module):
    return h.digest(Path(module.__file__).read_bytes().replace(b"\r\n", b"\n"))


def evaluate(baseline, *, quick=False, progress=None):
    h.validate_baseline(baseline)
    policy = json.loads(comparison.POLICY_PATH.read_text(encoding="utf-8"))
    probes = {p.name: p for p in h.ALL_PROBES}
    scores = [Scores(refine, policy) for refine in (False, True)]
    count = 0
    for old in baseline["cases"]:
        info, meta = old["fixture"], old["fixture"]["metadata"]
        if quick and (meta["pattern"] or meta["cfa_phase_x"] or meta["cfa_phase_y"]):
            continue
        fixture = h.generate(probes[info["name"]], layout=info["layout"], pattern=meta["pattern"],
                             phase=(meta["cfa_phase_x"], meta["cfa_phase_y"]), levels=info["level_policy"])
        if (h.digest(fixture.bayer_le) != info["bayer"]["sha256"] or
                h.digest(h.little_bytes("f", fixture.truth.pixels)) != info["ground_truth"]["sha256"] or
                comparison.canonical(fixture.metadata) != comparison.canonical(meta)):
            raise ValueError("generated research fixture differs from preserved evidence")
        reference = BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
        for scorer in scores:
            scorer.add(old, fixture, reference.render(refine=scorer.refine))
        count += 1
        if progress and count % 64 == 0:
            progress(count)
    return {"research_report_schema_version": 2, "reference_version": REFERENCE_VERSION,
            "reference_source_sha256_lf": source_digest(sys.modules[__name__]),
            "helper_source_sha256_lf": {Path(m.__file__).name: source_digest(m) for m in
                                         (h, corpus, comparison, support, metrics_helper)},
            "primary_paper_sha256": PRIMARY_PAPER_SHA256,
            "generator_version": h.GENERATOR_VERSION, "metric_formula_version": h.METRIC_VERSION,
            "measurement_domain": h.DOMAIN, "baseline_report_sha256": h.digest(h.report_json(baseline).encode()),
            "policy": policy, "policy_sha256": h.digest(comparison.POLICY_PATH.read_bytes()),
            "scope": "scalar research only; no native tile/cache/history/preview/performance/camera/shipment acceptance",
            "metadata_sweep": "quick" if quick else "full", "paired_case_count": count,
            "native_replacement_accepted": False,
            "variants": {name: score.report() for name, score in zip(("base", "refined"), scores)}}


def write_report(path, record):
    payload = h.report_json(record).encode("utf-8")
    path = Path(path)
    if path.exists() and path.read_bytes() != payload:
        raise ValueError("refusing to overwrite different research evidence")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return payload


def preserve(path, record):
    if record["metadata_sweep"] != "full" or record["paired_case_count"] != 2368:
        raise ValueError("preservation requires all 2368 paired fixtures")
    payload = h.report_json(record).encode("utf-8")
    packed = gzip.compress(payload, compresslevel=9, mtime=0)
    index = {"index_version": 1, "research_file": Path(path).name,
             "compressed_sha256": h.digest(packed), "report_json_sha256": h.digest(payload),
             **{k: record[k] for k in ("research_report_schema_version", "reference_version", "reference_source_sha256_lf",
                                      "helper_source_sha256_lf", "primary_paper_sha256", "baseline_report_sha256",
                                      "policy_sha256", "metadata_sweep", "paired_case_count", "native_replacement_accepted")},
             "variant_summary": {name: {"identity": v["research_identity"], "case_count": v["case_count"],
                                         "measured_gates": v["measured_gates"],
                                         "correctness_failure_count": len(v["correctness_failures"]),
                                         "regression_check_count": len(v["regressions"]),
                                         "improvement_pass_count": sum(g["passed"] for g in v["improvements"]),
                                         "improvement_gate_count": len(v["improvements"])}
                                 for name, v in record["variants"].items()}}
    target, index_path = Path(path), Path(path).with_suffix("").with_suffix(".index.json")
    encoded_index = (json.dumps(index, indent=2, sort_keys=True) + "\n").encode()
    for file, data in ((target, packed), (index_path, encoded_index)):
        if file.exists() and file.read_bytes() != data:
            raise ValueError("refusing to replace different preserved research evidence")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(packed)
    index_path.write_bytes(encoded_index)
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--quick", action="store_true", help="148 pairs; not full coverage")
    parser.add_argument("--preserve", type=Path, help="separate full research .json.gz evidence")
    args = parser.parse_args()
    if args.quick and args.preserve:
        parser.error("quick evidence cannot be preserved as the full study")
    result = evaluate(h.read_baseline(args.baseline), quick=args.quick,
                      progress=lambda n: print(f"{n} paired fixtures completed", flush=True))
    write_report(args.output, result)
    if args.preserve:
        preserve(args.preserve, result)
    print(json.dumps({name: {"cases": v["case_count"], "measured_gates": v["measured_gates"],
                            "correctness_failures": len(v["correctness_failures"]),
                            "regression_checks": len(v["regressions"]),
                            "improvement_passes": sum(g["passed"] for g in v["improvements"])}
                      for name, v in result["variants"].items()}, sort_keys=True))
    return 0  # Completing a rejected evaluation is valid research evidence.


if __name__ == "__main__":
    sys.exit(main())
