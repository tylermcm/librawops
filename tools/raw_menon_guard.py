"""Original bounded Menon base experiment with a frozen curvature-sign guard.

Only missing red/blue at green sites changes. Original v1 sources, scores,
policy and evidence are reused read-only. This is scalar research, never
native replacement, refinement, camera, tile/cache or performance admission.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import gzip
import itertools
import json
import math
from pathlib import Path
import sys

import raw_menon_diagnostics as diagnostics
import raw_menon_reference as m
import raw_menon_support as support
import raw_quality_compare as comparison
import raw_quality_harness as h

IDENTITY = {"algorithm": "librawops.menon.curvature-sign-guard.scalar-research", "version": 1,
            "refinement": False, "float32_stages": True, "bilinear_border": 6,
            "guard": "binary64 DG*DC > 0; zero/opposing signs suppress; far target samples +/-3"}
RADII = {"directional_green": 2, "decision": 4, "selected_green": 4,
         "guarded_color_at_green": 5, "guarded_opposite_color": 6}
EXAMPLES = {"chromatic_hard_edge_vertical": ((32, 24), 2),
            "chromatic_linear_edge_vertical": ((34, 22), 2),
            "chromatic_sine_1_32_x": ((45, 24), 0),
            "chromatic_sine_1_8_x": ((35, 24), 0),
            "neutral_hard_edge_vertical": ((33, 24), 2)}
ORIGINAL_REPORT_SHA256 = "7240a36171b1cfd819baa9af75b8ed6b95baacf3549ecd3dccd04d25ebef3270"


@dataclass(frozen=True)
class Decision:
    near_mean: float
    green_curvature: float
    color_curvature: float
    product: float
    kept: bool
    value: float


def interpolate(center_green, near_a, near_b, green_a, green_b, far_a, far_b):
    if any(not math.isfinite(v) for v in (center_green, near_a, near_b, green_a, green_b, far_a, far_b)):
        raise ValueError("finite guard inputs required")
    near_sum, far_sum = near_a + near_b, far_a + far_b
    mean = .5 * near_sum
    dg = center_green - .5 * (green_a + green_b)
    dc = (near_sum - far_sum) / 16.0
    product = dg * dc
    kept = product > 0.0
    value = mean + (dg if kept else 0.0)
    if any(not math.isfinite(v) for v in (mean, dg, dc, product, value)):
        raise ValueError("guard arithmetic exceeds finite research domain")
    return Decision(mean, dg, dc, product, kept, h.f32(value))


class BayerReference(m.BayerReference):
    """Reuse immutable original green/classifiers; expose base-only output."""
    def guard_details(self, p, c):
        self._require(p, 5)
        if self.colors[p] != 1 or type(c) is not int or c not in (0, 2):
            raise ValueError("guard requires green site and missing red/blue")
        def evaluate():
            e = self._axis_at_green(p, c)
            a, b = support.shifted(p, e, -1), support.shifted(p, e, 1)
            far_a, far_b = support.shifted(p, e, -3), support.shifted(p, e, 3)
            return interpolate(self.observed[p], self.observed[a], self.observed[b],
                               self.green(a), self.green(b), self.observed[far_a], self.observed[far_b])
        return self._memo(("guard_decision", p, c), evaluate)

    def color_at_green(self, p, c):
        return self.guard_details(p, c).value

    @staticmethod
    def _base_only(refine):
        if refine is not False:
            raise ValueError("guarded research contract permits base only")

    def pixel(self, x, y, *, refine=False):
        self._base_only(refine)
        return super().pixel(x, y, refine=False)

    def render(self, bounds=None, *, refine=False):
        self._base_only(refine)
        return super().render(bounds, refine=False)

    def refined_green(self, p):
        raise ValueError("guarded research contract permits base only")

    def after_green_locations(self, p, c):
        raise ValueError("guarded research contract permits base only")

    def refined(self, p, c):
        raise ValueError("guarded research contract permits base only")


class Support(support.Support):
    def color_at_green(self, p, c):
        e = self.axis_for_color(p, c)
        return super().color_at_green(p, c) | {support.shifted(p, e, -3), support.shifted(p, e, 3)}

    def output(self, bounds, p, c, refine=False):
        BayerReference._base_only(refine)
        return super().output(bounds, p, c, refine=False)


def support_report():
    measured, witnesses = {name: set() for name in RADII}, {}
    parity_cases = 0
    for pattern, px, py in itertools.product(support.PATTERNS, (0, 1), (0, 1)):
        geometry = Support(pattern, (px, py))
        for p in itertools.product((0, 1), repeat=2):
            own = geometry.channel(p)
            if own == 1:
                stages = {"guarded_color_at_green": geometry.color_at_green(p, 0) | geometry.color_at_green(p, 2)}
            else:
                stages = {"directional_green": geometry.directional_green(p, support.AXES[0]),
                          "decision": geometry.decision(p), "selected_green": geometry.green(p),
                          "guarded_opposite_color": geometry.base(p, 2 if own == 0 else 0)}
            for name, leaves in stages.items():
                radius = support.radius(leaves, p)
                measured[name].add(radius)
                witnesses.setdefault(name, {"pattern": pattern, "phase": [px, py], "site": list(p),
                    "radius": radius, "outer_offsets": [list((x - p[0], y - p[1])) for x, y in sorted(leaves)
                        if max(abs(x - p[0]), abs(y - p[1])) == radius]})
            parity_cases += 1
    if any(measured[name] != {value} for name, value in RADII.items()):
        raise ValueError("guarded staged support disagrees with frozen contract")
    outputs, interiors, layouts = 0, 0, 0
    for pattern, px, py, origin, extent in itertools.product(support.PATTERNS, (0, 1), (0, 1),
            ((0, 0), (3, 5)), ((1, 1), (1, 19), (19, 1), (12, 19), (13, 13), (16, 19), (17, 17), (21, 19))):
        bounds, geometry = support.Bounds(*origin, *extent), Support(pattern, (px, py))
        for p in bounds.points():
            interiors += bounds.margin(p) >= 6
            for c in range(3):
                leaves = geometry.output(bounds, p, c)
                if not all(bounds.contains(q) for q in leaves):
                    raise ValueError("guarded output reads inactive samples")
                if c == geometry.channel(p) and leaves != {p}:
                    raise ValueError("guard changed observed support")
                outputs += 1
        layouts += 1
    return {"kind": "symbolic-dependency-proof", "evaluates_pixels": False, "stage_radii": RADII,
            "outer_witnesses": witnesses, "cfa_phase_site_cases": parity_cases,
            "active_layout_cases": layouts, "channel_output_checks": outputs,
            "interior_point_checks": interiors, "active_confinement_passed": True, "halo": 6,
            "scope": "Both guard outcomes, all period-two CFA sites/green directions/classifiers; bounded active layouts; no native proof"}


def guard_summary(reference):
    ox, oy, width, height = reference.bounds
    counts = {"kept": 0, "opposing_signs": 0, "zero_product": 0, "total": 0}
    for y in range(oy + 6, oy + height - 6):
        for x in range(ox + 6, ox + width - 6):
            if reference.colors[x, y] != 1:
                continue
            for c in (0, 2):
                decision = reference.guard_details((x, y), c)
                counts["kept" if decision.kept else "opposing_signs" if decision.product < 0 else "zero_product"] += 1
                counts["total"] += 1
    return counts


def evaluate(baseline, original, *, quick=False, progress=None):
    h.validate_baseline(baseline)
    diagnostics.validate_study(original)
    original_digest = h.digest(h.report_json(original).encode())
    if original_digest != ORIGINAL_REPORT_SHA256:
        raise ValueError("original study identity differs from frozen v1")
    if type(quick) is not bool:
        raise ValueError("quick must be boolean")
    if original["baseline_report_sha256"] != h.digest(h.report_json(baseline).encode()):
        raise ValueError("guard/original/baseline pairing mismatch")
    old_cases = {tuple(case["case"]): case for case in original["variants"]["base"]["cases"]}
    if len(old_cases) != 2368:
        raise ValueError("original base contains duplicate cases")
    proof = support_report()
    policy, probes = original["policy"], {probe.name: probe for probe in h.ALL_PROBES}
    scorer, groups, examples, changed = m.Scores(False, policy), {}, [], 0
    for old in baseline["cases"]:
        info, meta, key = old["fixture"], old["fixture"]["metadata"], comparison.case_key(old)
        if quick and (meta["pattern"] or meta["cfa_phase_x"] or meta["cfa_phase_y"]):
            continue
        fixture = h.generate(probes[info["name"]], layout=info["layout"], pattern=meta["pattern"],
                             phase=(meta["cfa_phase_x"], meta["cfa_phase_y"]), levels=info["level_policy"])
        if (h.digest(fixture.bayer_le) != info["bayer"]["sha256"]
                or h.digest(h.little_bytes("f", fixture.truth.pixels)) != info["ground_truth"]["sha256"]
                or comparison.canonical(fixture.metadata) != comparison.canonical(meta)):
            raise ValueError("guard fixture differs from preserved evidence")
        prior = old_cases[key]
        reference = BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
        rendered = reference.render()
        scorer.add(old, fixture, rendered)
        current = scorer.cases[-1]
        current["guard_decisions"] = guard_summary(reference)
        changed += current["rendered_float32_le_sha256"] != prior["rendered_float32_le_sha256"]
        comparison.metric_shape_and_values(prior["metrics"], current["metrics"])
        scope = policy["improvement_scopes"].get(info["family"])
        if scope:
            for score in policy["improvement_scores"]:
                before = comparison.score(prior["metrics"][scope], score)
                if before is not None:
                    groups.setdefault((info["name"], scope, score), [[], []])[0].append(before)
                    groups[info["name"], scope, score][1].append(comparison.score(current["metrics"][scope], score))
        if info["name"] in EXAMPLES and key[1:] == ("packed", 0, 0, 0, "uniform"):
            p, c = EXAMPLES[info["name"]]
            unchanged = m.BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
            prior_image = unchanged.render(refine=False)
            if h.digest(h.little_bytes("f", prior_image.pixels)) != prior["rendered_float32_le_sha256"]:
                raise ValueError("original example render identity differs")
            i = (p[1] * rendered.width + p[0]) * 3 + c
            row = {"case": list(key), "sensor": list(p), "channel": h.CHANNELS[c],
                   "observed_channel": h.CHANNELS[reference.colors[p]], "truth": fixture.truth.pixels[i],
                   "bilinear": reference.bilinear(p, c), "original_base": prior_image.pixels[i],
                   "guarded_base": rendered.pixels[i]}
            if reference.colors[p] == 1:
                decision = reference.guard_details(p, c)
                row["decision"] = {name: getattr(decision, name) for name in decision.__dataclass_fields__}
            examples.append(row)
        if progress and len(scorer.cases) % 64 == 0:
            progress(len(scorer.cases))
    result = scorer.report()
    result["research_identity"] = dict(IDENTITY)
    totals = {name: sum(case["guard_decisions"][name] for case in scorer.cases)
              for name in ("kept", "opposing_signs", "zero_product", "total")}
    paired = [{"probe": probe, "scope": scope, "score": score, "pair_count": len(before),
               "original_base_mean": math.fsum(before) / len(before), "guarded_base_mean": math.fsum(after) / len(after)}
              for (probe, scope, score), (before, after) in sorted(groups.items())]
    return {"research_report_schema_version": 3, "guard_contract_version": 1, "research_identity": dict(IDENTITY),
            "reference_source_sha256_lf": m.source_digest(sys.modules[__name__]),
            "helper_source_sha256_lf": {Path(module.__file__).name: m.source_digest(module) for module in
                (m, support, diagnostics, h, comparison, m.corpus, m.metrics_helper)},
            "baseline_report_sha256": original["baseline_report_sha256"],
            "original_scalar_report_sha256": original_digest,
            "generator_version": h.GENERATOR_VERSION, "metric_formula_version": h.METRIC_VERSION,
            "measurement_domain": h.DOMAIN, "policy": policy, "policy_sha256": original["policy_sha256"],
            "metadata_sweep": "quick" if quick else "full", "paired_case_count": len(scorer.cases),
            "native_replacement_accepted": False, "support_proof": proof,
            "scope": "Separate base-only scalar experiment; no refinement, native/tile/cache/history/camera/performance/shipment acceptance",
            "guard_decision_scope": "Both missing colors at green sites in final nonfallback radius-six interior only; excludes internal-only stage calls",
            "guard_decision_totals": totals, "comparison_to_original_base": {"changed_render_count": changed, "paired_probe_scores": paired},
            "selected_examples": examples, "variant": result}


def preserve(path, record):
    if record["metadata_sweep"] != "full" or record["paired_case_count"] != 2368:
        raise ValueError("preservation requires all 2368 paired fixtures")
    payload = h.report_json(record).encode()
    packed = gzip.compress(payload, compresslevel=9, mtime=0)
    variant = record["variant"]
    index = {"index_version": 1, "research_file": Path(path).name, "compressed_sha256": h.digest(packed),
             "report_json_sha256": h.digest(payload), **{name: record[name] for name in
                ("research_report_schema_version", "guard_contract_version", "research_identity", "reference_source_sha256_lf",
                 "helper_source_sha256_lf", "baseline_report_sha256", "original_scalar_report_sha256", "policy_sha256",
                 "metadata_sweep", "paired_case_count", "native_replacement_accepted", "guard_decision_totals")},
             "variant_summary": {"measured_gates": variant["measured_gates"], "correctness_failure_count": len(variant["correctness_failures"]),
                 "regression_check_count": len(variant["regressions"]), "regressed_case_count": len({tuple(row["case"]) for row in variant["regressions"]}),
                 "improvement_pass_count": sum(row["passed"] for row in variant["improvements"]), "improvement_gate_count": len(variant["improvements"]),
                 "changed_render_count_vs_original_base": record["comparison_to_original_base"]["changed_render_count"]}}
    targets = ((Path(path), packed), (Path(path).with_suffix("").with_suffix(".index.json"),
                (json.dumps(index, sort_keys=True, indent=2) + "\n").encode()))
    for target, data in targets:
        if target.exists() and target.read_bytes() != data:
            raise ValueError("refusing to replace different guarded research evidence")
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    for target, data in targets:
        target.write_bytes(data)
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("original", type=Path, help="preserved original Menon schema-2 study")
    parser.add_argument("output", type=Path)
    parser.add_argument("--quick", action="store_true", help="148 paired cases only; not full coverage")
    parser.add_argument("--preserve", type=Path, help="separate full-study .json.gz evidence")
    args = parser.parse_args()
    if args.quick and args.preserve:
        parser.error("quick evidence cannot be preserved as a full study")
    record = evaluate(h.read_baseline(args.baseline), diagnostics.read_study(args.original), quick=args.quick,
                      progress=lambda n: print(f"{n} guarded paired fixtures completed", flush=True))
    m.write_report(args.output, record)
    if args.preserve:
        preserve(args.preserve, record)
    v = record["variant"]
    print(json.dumps({"case_count": v["case_count"], "measured_gates": v["measured_gates"],
                      "correctness_failures": len(v["correctness_failures"]), "regression_checks": len(v["regressions"]),
                      "regressed_cases": len({tuple(row["case"]) for row in v["regressions"]}),
                      "improvement_passes": sum(row["passed"] for row in v["improvements"]),
                      "native_replacement_accepted": False}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
