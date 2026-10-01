"""Analytical HA error-budget research; never changes replacement policy.

Exact rational, branch-independent absolute-error propagation for the frozen
flat/affine fixtures. This is an audit of scalar v1, not a native backend.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from fractions import Fraction as F
import gzip
import json
from pathlib import Path
import sys

import raw_ha_reference as ha
import raw_quality_compare as comparison
import raw_quality_harness as h

AUDIT_VERSION = 1
U32, U64, TINY32 = F(1, 2**24), F(1, 2**53), F(1, 2**150)
ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "tests/reference/raw"


def source_digest(path):
    # Git's Windows checkout may use CRLF. Preserve the historical byte hashes
    # and permit only this explicitly recorded newline normalization.
    return h.digest(path.read_bytes().replace(b"\r\n", b"\n"))


@dataclass(frozen=True)
class Budget:
    quantization: F = F(0)
    roundoff: F = F(0)

    @property
    def total(self):
        return self.quantization + self.roundoff

    def __add__(self, other):
        return Budget(self.quantization + other.quantization, self.roundoff + other.roundoff)

    def __mul__(self, weight):
        if weight < 0:
            raise ValueError("error weights must be nonnegative")
        return Budget(self.quantization * weight, self.roundoff * weight)


def envelope(*choices):
    # Taking each component's maximum is conservative even if maxima occur
    # in different branches. An exact tie is a convex mean of the branches.
    return Budget(max(b.quantization for b in choices), max(b.roundoff for b in choices))


def green_budget(center, far_a, far_b, near_a, near_b):
    return center * F(1, 2) + (far_a + far_b) * F(1, 4) + (near_a + near_b) * F(1, 2)


def chroma_budget(center_green, green_a, green_b, color_a, color_b):
    return center_green + (green_a + green_b + color_a + color_b) * F(1, 2)


def finish_stage(budget, ideal_magnitude, input_magnitude):
    # At most eight rounded binary64 operations, including the tie mean;
    # intermediates are bounded by 8*input_magnitude. Power-of-two scaling
    # is exact here. 64*M*u64 covers the absolute arithmetic error, without
    # assuming a well-conditioned classifier or stable branch selection.
    arithmetic = 64 * input_magnitude * U64
    pre_cast = budget.total + arithmetic
    return Budget(budget.quantization,
                  budget.roundoff + arithmetic + U32 * (ideal_magnitude + pre_cast) + TINY32)


def ideal_rgb(probe, x, y):
    return tuple(F(c) + F(dx) * x + F(dy) * y
                 for c, dx, dy in zip(probe.constant, probe.dx, probe.dy))


def truth_profile(fixture):
    """Independent exact affine plane, plus generated float32 truth deviation."""
    magnitude, storage = [F(0)] * 3, [F(0)] * 3
    for y in range(fixture.truth.height):
        for x in range(fixture.truth.width):
            for c, ideal in enumerate(ideal_rgb(fixture.probe, x, y)):
                value = fixture.truth.pixels[(y * fixture.truth.width + x) * 3 + c]
                magnitude[c] = max(magnitude[c], abs(ideal))
                storage[c] = max(storage[c], abs(F(value) - ideal))
    return tuple(magnitude), tuple(storage)


def bilinear_budget(input_budget, magnitude):
    """Bound up to four float32 additions, division and final float32 cast."""
    choices = []
    for n in range(1, 5):
        accumulated = F(0)
        for k in range(1, n + 1):
            accumulated += U32 * (k * (magnitude + input_budget.total) + accumulated) + TINY32
        mean_error = accumulated / n
        division = U64 * (magnitude + input_budget.total + mean_error)
        cast = U32 * (magnitude + input_budget.total + mean_error + division) + TINY32
        choices.append(Budget(input_budget.quantization,
                              input_budget.roundoff + mean_error + division + cast))
    return envelope(*choices)


def fixture_budgets(fixture, profile=None):
    """Conservative channel bounds over every branch and active-border fallback.

    Every predictor reproduces an ideal affine channel away from borders;
    hence classifier flips/ties cannot introduce an ideal-field error. A flat
    is also reproduced by every clipped bilinear border stencil. Affine
    acceptance uses the unchanged one-pixel interior, where bilinear stencils
    are symmetric. The frozen fixtures are 65x49, not thin-image cases.
    """
    if fixture.probe.family not in ("flat", "affine") or (fixture.truth.width, fixture.truth.height) != (65, 49):
        raise ValueError("audit requires the frozen 65x49 flat/affine fixtures")
    magnitude, storage = truth_profile(fixture) if profile is None else profile
    meta = fixture.metadata
    inputs = []
    for site, channel in enumerate(h.CFA[meta["pattern"]]):
        span = meta["white_levels"][site] - meta["black_levels"][site]
        if span <= 0:
            raise ValueError("invalid site span")
        q = F(1, 2 * span)
        # Encoding has three binary64 rounding steps with uint16-scale
        # intermediates; this deliberately conservative allowance also covers
        # half-step boundary crossings caused by their rounding.
        encoding = F(64 * 65536, span) * U64
        normalization64 = U64 * (magnitude[channel] + storage[channel] + q + encoding)
        normalization32 = U32 * (magnitude[channel] + storage[channel] + q + encoding + normalization64) + TINY32
        inputs.append(Budget(q, storage[channel] + encoding + normalization64 + normalization32))

    def sample(x, y):
        return inputs[h.site_channel(meta, x, y)[0]]

    def color(x, y):
        return h.site_channel(meta, x, y)[1]

    sample_magnitude = max(magnitude[c] + inputs[s].total for s, c in enumerate(h.CFA[meta["pattern"]]))
    green = {}
    for y in range(2):
        for x in range(2):
            if color(x, y) == 1:
                green[x, y] = sample(x, y)
            else:
                choices = [green_budget(sample(x, y), sample(x - 2 * dx, y - 2 * dy),
                                        sample(x + 2 * dx, y + 2 * dy), sample(x - dx, y - dy),
                                        sample(x + dx, y + dy)) for dx, dy in ((1, 0), (0, 1))]
                green[x, y] = finish_stage(envelope(*choices), magnitude[1], sample_magnitude)

    def g(x, y):
        return green[x % 2, y % 2]

    green_magnitude = magnitude[1] + max(b.total for b in green.values())
    result = []
    for channel in range(3):
        candidates = [bilinear_budget(envelope(*(inputs[s] for s, c in enumerate(h.CFA[meta["pattern"]])
                                                if c == channel)), magnitude[channel])]
        for y in range(2):
            for x in range(2):
                if color(x, y) == channel:
                    candidates.append(sample(x, y))
                elif channel == 1:
                    candidates.append(g(x, y))
                else:
                    directions = (((1, 0),) if color(x + 1, y) == channel else ((0, 1),)) if color(x, y) == 1 else ((1, 1), (1, -1))
                    choices = [chroma_budget(g(x, y), g(x - dx, y - dy), g(x + dx, y + dy),
                                              sample(x - dx, y - dy), sample(x + dx, y + dy))
                               for dx, dy in directions]
                    candidates.append(finish_stage(envelope(*choices), magnitude[channel],
                                                   max(sample_magnitude, green_magnitude)))
        final = envelope(*candidates)
        # Metrics compare to stored float32 truth, rather than the exact plane.
        result.append(Budget(final.quantization, final.roundoff + storage[channel]))
    return tuple(result)


def counterexample():
    """Exact decomposition of the retained neutral_mid red counterexample."""
    fixture = h.generate(next(p for p in h.PROBES if p.name == "neutral_mid"), levels="per-site")
    ref = ha.BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
    weights = {}

    def add(point, weight):
        weights[point] = weights.get(point, F(0)) + weight

    def green_weights(x, y, weight):
        s = ref.observed
        predictors = []
        for dx, dy in ((1, 0), (0, 1)):
            lap = (2 * s[x, y] - s[x - 2 * dx, y - 2 * dy]) - s[x + 2 * dx, y + 2 * dy]
            classifier = abs(lap) + abs(s[x - dx, y - dy] - s[x + dx, y + dy])
            predictors.append((classifier, dx, dy))
        lowest = min(p[0] for p in predictors)
        selected = [p for p in predictors if p[0] == lowest]
        for _, dx, dy in selected:
            scale = weight / len(selected)
            add((x, y), scale / 2)
            for sign in (-1, 1):
                add((x + sign * dx, y + sign * dy), scale / 2)
                add((x + sign * 2 * dx, y + sign * 2 * dy), -scale / 4)

    x, y, channel = 30, 25, 0
    add((x, y), F(1))
    for yy in (y - 1, y + 1):
        add((x, yy), F(1, 2))
        green_weights(x, yy, F(-1, 2))
    weights = {p: w for p, w in weights.items() if w}
    samples, meta = fixture.native_samples(), fixture.metadata
    quantization, normalization, stored_truth, linear, q_bound = [F(0)] * 5
    for (xx, yy), weight in weights.items():
        site, c = h.site_channel(meta, xx, yy)
        span = meta["white_levels"][site] - meta["black_levels"][site]
        code = samples[yy * meta["row_stride_samples"] + xx]
        normalized = F(code - meta["black_levels"][site], span)
        truth = F(fixture.truth.pixels[(yy * fixture.truth.width + xx) * 3 + c])
        observed = F(ref.observed[xx, yy])
        quantization += weight * (normalized - truth)
        normalization += weight * (observed - normalized)
        stored_truth += weight * (truth - ideal_rgb(fixture.probe, xx, yy)[c])
        linear += weight * observed
        q_bound += abs(weight) * F(1, 2 * span)
    rendered = F(ref.pixel(x, y)[channel])
    truth = F(fixture.truth.pixels[(y * fixture.truth.width + x) * 3 + channel])
    stored_truth += ideal_rgb(fixture.probe, x, y)[channel] - truth
    stage_roundoff = rendered - linear
    signed_error = rendered - truth
    if quantization + normalization + stored_truth + stage_roundoff != signed_error:
        raise ValueError("counterexample decomposition does not close exactly")
    return {"pixel": [x, y], "channel": "R", "rendered": float(rendered), "truth": float(truth),
            "signed_error": float(signed_error), "sensor_quantization_contribution": float(quantization),
            "normalization_roundoff_contribution": float(normalization),
            "truth_storage_contribution": float(stored_truth), "stage_roundoff_contribution": float(stage_roundoff),
            "selected_stencil_quantization_bound": float(q_bound), "exact_rational_decomposition_closed": True,
            "stencil": [{"sensor": list(p), "weight": str(w)} for p, w in sorted(weights.items())]}


def read_research(baseline):
    index = json.loads((EVIDENCE / "ha_scalar_research_v1.index.json").read_text(encoding="utf-8"))
    compressed = (EVIDENCE / "ha_scalar_research_v1.json.gz").read_bytes()
    data = gzip.decompress(compressed)
    if h.digest(compressed) != index["compressed_sha256"] or h.digest(data) != index["report_json_sha256"]:
        raise ValueError("preserved research integrity mismatch")
    report = json.loads(data)
    if (report["reference_version"] != 1 or report["case_count"] != len(baseline["cases"]) or
            report["baseline_report_sha256"] != h.digest(h.report_json(baseline).encode()) or
            report["reference_source_sha256"] != source_digest(Path(ha.__file__)) or
            report["policy"] != json.loads(comparison.POLICY_PATH.read_text(encoding="utf-8")) or
            any(source_digest(ROOT / "tools" / name) != value
                for name, value in report["helper_source_sha256"].items())):
        raise ValueError("preserved research provenance mismatch")
    rows = {tuple(row["case"]): row for row in report["cases"]}
    if len(rows) != report["case_count"] or set(rows) != {comparison.case_key(c) for c in baseline["cases"]}:
        raise ValueError("preserved research case pairing mismatch")
    return report, rows, index


def evaluate(baseline):
    h.validate_baseline(baseline)
    research, paired, index = read_research(baseline)
    probes = {p.name: p for p in h.PROBES}
    profiles, cases = {}, []
    for old in baseline["cases"]:
        info = old["fixture"]
        if info["family"] not in ("flat", "affine"):
            continue
        meta = info["metadata"]
        fixture = h.generate(probes[info["name"]], layout=info["layout"], pattern=meta["pattern"],
                             phase=(meta["cfa_phase_x"], meta["cfa_phase_y"]), levels=info["level_policy"])
        if (h.digest(fixture.bayer_le) != info["bayer"]["sha256"] or
                h.digest(h.little_bytes("f", fixture.truth.pixels)) != info["ground_truth"]["sha256"] or
                comparison.canonical(fixture.metadata) != comparison.canonical(meta)):
            raise ValueError("audit fixture differs from preserved evidence")
        if info["name"] not in profiles:
            profiles[info["name"]] = truth_profile(fixture)
        budgets = fixture_budgets(fixture, profiles[info["name"]])
        row = paired[comparison.case_key(old)]
        scope = old["acceptance"]["analytical_scope"]
        channels = {c: {"quantization_bound": float(b.quantization),
                        "roundoff_and_truth_storage_bound": float(b.roundoff), "total_bound": float(b.total),
                        "measured_max_abs": row["metrics"][scope]["channels"][c]["max_abs"],
                        "within_derived_budget": F(row["metrics"][scope]["channels"][c]["max_abs"]) <= b.total}
                    for c, b in zip(h.CHANNELS, budgets)}
        cases.append({"case": row["case"], "scope": scope, "channels": channels,
                      "frozen_analytical_passed": row["measured_correctness"]["analytical"],
                      "observed_fidelity_passed": row["measured_correctness"]["observed_fidelity"],
                      "quantization_passed": row["measured_correctness"]["quantization"]})
    return {"error_budget_audit_version": AUDIT_VERSION, "measurement_domain": h.DOMAIN,
            "scope": "scalar v1 analytical audit only; no replacement-policy revision or native acceptance",
            "audit_source_sha256": source_digest(Path(__file__)),
            "source_hash_rule": "CRLF normalized to LF; historical evidence byte hashes retained",
            "baseline_report_sha256": research["baseline_report_sha256"],
            "research_report_sha256": index["report_json_sha256"],
            "reference_source_sha256": research["reference_source_sha256"],
            "helper_source_sha256": research["helper_source_sha256"],
            "policy_sha256": index["policy_sha256"],
            "policy_content_sha256": h.digest(h.report_json(research["policy"]).encode()),
            "uniform_quantization_multipliers": {"observed": 1, "missing_green": 2,
                                                 "axial_missing_color": 4, "diagonal_missing_color": 5},
            "counterexample": counterexample(), "cases": cases,
            "summary": {"analytical_cases": len(cases),
                        "frozen_analytical_failures": sum(not c["frozen_analytical_passed"] for c in cases),
                        "derived_budget_failures": sum(not all(ch["within_derived_budget"] for ch in c["channels"].values()) for c in cases),
                        "observed_fidelity_failures": sum(not c["observed_fidelity_passed"] for c in cases),
                        "quantization_failures": sum(not c["quantization_passed"] for c in cases),
                        "frozen_regression_checks": len(research["regressions"]),
                        "frozen_improvement_gates_passed": sum(g["passed"] for g in research["improvements"]),
                        "frozen_improvement_gates_total": len(research["improvements"])},
            "native_replacement_accepted": False}


def preserve(result, directory):
    data = h.report_json(result).encode()
    compressed = gzip.compress(data, mtime=0)
    name = "ha_error_budget_audit_v1.json.gz"
    index = h.report_json({"error_budget_audit_version": AUDIT_VERSION, "filename": name,
                           "compressed_sha256": h.digest(compressed), "report_json_sha256": h.digest(data),
                           "audit_source_sha256": result["audit_source_sha256"],
                           "source_hash_rule": result["source_hash_rule"],
                           "baseline_report_sha256": result["baseline_report_sha256"],
                           "research_report_sha256": result["research_report_sha256"],
                           "policy_sha256": result["policy_sha256"],
                           "policy_content_sha256": result["policy_content_sha256"], "summary": result["summary"],
                           "native_replacement_accepted": False}).encode()
    files = [(directory / name, compressed), (directory / "ha_error_budget_audit_v1.index.json", index)]
    for path, content in files:
        if path.exists() and path.read_bytes() != content:
            raise ValueError("refusing to replace different audit evidence")
    directory.mkdir(parents=True, exist_ok=True)
    for path, content in files:
        if not path.exists():
            path.write_bytes(content)
        if path.read_bytes() != content:
            raise ValueError("audit evidence read-back mismatch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--preserve", type=Path, help="preserve separate immutable audit gzip/index")
    args = parser.parse_args()
    result = evaluate(h.read_baseline(args.baseline))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(h.report_json(result), encoding="utf-8")
    if args.preserve:
        preserve(result, args.preserve)
    print(json.dumps(result["summary"], sort_keys=True))
    return int(bool(result["summary"]["derived_budget_failures"]))


if __name__ == "__main__":
    sys.exit(main())
