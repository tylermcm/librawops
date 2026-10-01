"""Pair the pinned synthetic corpus; enforce versioned replacement or exact parity.

No pixel data is present in these reports: this validates native harness evidence,
not an independently rerun renderer or an authentication of its measured scores.
"""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import raw_quality_harness as h

POLICY_PATH = Path(__file__).resolve().parents[1] / "tests/reference/raw/replacement_policy_v1.json"
LEGACY = {"algorithm": "rawengine.bilinear", "processing_version": 1}


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def case_key(case):
    f = case["fixture"]
    m = f["metadata"]
    return (f["name"], f["layout"], m["pattern"], m["cfa_phase_x"], m["cfa_phase_y"], f["level_policy"])


def source_policy(case):
    manifest = case["saved_manifest"]
    if (manifest["format_version"] not in (2, 3) or len(manifest["sources"]) != 1 or
            manifest["operations"] or manifest["output"] != manifest["sources"][0]["id"]):
        raise ValueError("measurement requires a source-only format-2/3 manifest")
    source = manifest["sources"][0]
    if source["kind"] != "decoded_bayer_u16":
        raise ValueError("measurement requires decoded Bayer source")
    policy = source.get("demosaic", LEGACY)
    if (manifest["format_version"] == 3 and "demosaic" not in source or
            manifest["format_version"] == 2 and "demosaic" in source or
            set(policy) != {"algorithm", "processing_version"} or
            not isinstance(policy["algorithm"], str) or not policy["algorithm"] or
            type(policy["processing_version"]) is not int or policy["processing_version"] < 1 or
            case["source_info"].get("demosaic", LEGACY) != policy):
        raise ValueError("invalid or inconsistent demosaic policy")
    return policy


def input_identity(case):
    source_policy(case)
    result = copy.deepcopy({k: case[k] for k in
                           ("fixture", "source_info", "saved_manifest", "render_request", "measurement_scopes")})
    result["source_info"].pop("demosaic", None)
    result["saved_manifest"]["sources"][0].pop("demosaic", None)
    result["saved_manifest"]["format_version"] = 2
    return canonical(result)


def metric_shape_and_values(base, current, path="metrics"):
    """Denominators/structure must match; values must be finite nonnegative numbers."""
    if isinstance(base, dict):
        if not isinstance(current, dict) or set(base) != set(current):
            raise ValueError(f"metric structure differs: {path}")
        for key in base:
            if key in ("pixel_count", "sample_count"):
                if type(current[key]) is not int or current[key] != base[key]:
                    raise ValueError(f"metric denominator differs: {path}.{key}")
            else:
                metric_shape_and_values(base[key], current[key], path + "." + key)
        if "rmse" in current and current["rmse"] is not None:
            if (current["rmse"] > current["max_abs"] + 1e-12 or
                    current.get("mae", 0) > current["rmse"] + 1e-12):
                raise ValueError(f"inconsistent metric scores: {path}")
    elif base is None:
        if current is not None:
            raise ValueError(f"metric null applicability differs: {path}")
    elif type(current) not in (int, float) or not math.isfinite(current) or current < 0:
        raise ValueError(f"invalid metric value: {path}")


def score(metric, name):
    block, field = name.split(".")
    return metric[block][field] if metric[block] is not None else None


def correctness(case, policy):
    m = case["fixture"]["metadata"]
    bound = max(0.5 / (w - b) for b, w in zip(m["black_levels"], m["white_levels"]))
    metrics = case["metrics"]
    scope = {"flat": "whole", "affine": "interior"}.get(case["fixture"]["family"])
    gates = {"quantization_bound": metrics["quantization"]["observed"]["max_abs"] <= bound + policy["quantization_roundoff_allowance"],
             "observed_sample_fidelity": metrics["observed_fidelity"]["observed"]["max_abs"] <= policy["observed_fidelity_max_limit"],
             "analytical_reconstruction": metrics[scope]["rgb"]["max_abs"] <= bound + policy["analytical_roundoff_allowance"] if scope else None,
             "tile_exact": set(case["tile_exact"]) == {str(v) for v in h.TILE_SIZES} and all(v is True for v in case["tile_exact"].values()),
             "roi_exact": all(r["exact_crop"] is True for r in case["roi_checks"])}
    acceptance = case["acceptance"]
    if (canonical(acceptance["gates"]) != canonical(gates) or acceptance["passed"] is not all(v for v in gates.values() if v is not None) or
            acceptance["analytical_scope"] != scope or acceptance["quantization_max_bound"] != bound or
            acceptance["reconstruction_max_limit"] != (bound + policy["analytical_roundoff_allowance"] if scope else None) or
            acceptance["observed_fidelity_max_limit"] != policy["observed_fidelity_max_limit"]):
        raise ValueError("reported correctness does not agree with measured metrics/policy")
    return gates


def compare(base, candidate, policy=None):
    policy = json.loads(POLICY_PATH.read_text(encoding="utf-8")) if policy is None else policy
    pinned_policy = json.loads(POLICY_PATH.read_text(encoding="utf-8"))
    if policy != pinned_policy:
        raise ValueError("unsupported replacement policy; introduce a reviewed new policy version")
    h.validate_baseline(base)
    h.validate_baseline(candidate, require_passed=False)
    for field in ("report_schema_version", "generator_version", "metric_formula_version", "seed", "randomness", "provenance", "scope"):
        if base[field] != candidate[field]:
            raise ValueError(f"report provenance differs: {field}")
    for field in ("measurement_domain", "stage"):
        if base["algorithm"][field] != candidate["algorithm"][field]:
            raise ValueError(f"algorithm measurement differs: {field}")
    paired = {case_key(c): c for c in candidate["cases"]}
    failures, regressions, groups = [], [], {}
    parity = True
    identities = set()
    characterization = {}
    for old in sorted(base["cases"], key=case_key):
        new = paired[case_key(old)]
        key = case_key(old)
        if source_policy(old) != LEGACY:
            raise ValueError("replacement reference must be pinned bilinear version 1")
        if candidate["algorithm"].get("demosaic", LEGACY) != source_policy(new):
            raise ValueError("candidate header disagrees with case demosaic policy")
        if input_identity(old) != input_identity(new):
            raise ValueError(f"paired fixture/source/request/scope differs: {key}")
        if ([r["bounds"] for r in old["roi_checks"]] != [r["bounds"] for r in new["roi_checks"]] or
                old["metrics"]["quantization"] != new["metrics"]["quantization"]):
            raise ValueError(f"paired ROI/observed quantization differs: {key}")
        metric_shape_and_values(old["metrics"], new["metrics"])
        gates = correctness(new, policy)
        failures.extend({"case": key, "gate": gate} for gate, value in gates.items() if value is False)
        identities.add(canonical(source_policy(new)))
        parity = parity and source_policy(old) == source_policy(new) and all(
            old[k] == new[k] for k in ("metrics", "tile_exact", "roi_checks", "acceptance", "rendered_float32_le_sha256"))
        family = old["fixture"]["family"]
        if family in ("flat", "affine"):
            continue
        for scope, metric in old["metrics"].items():
            if "rgb" not in metric:
                continue
            for name in policy["regression_scores"]:
                before, after = score(metric, name), score(new["metrics"][scope], name)
                if before is None:
                    continue
                limit = before * (1 + policy["per_case_regression_fraction"]) + policy["absolute_regression_allowance"]
                if after > limit:
                    regressions.append({"case": key, "scope": scope, "score": name, "baseline": before,
                                        "candidate": after, "limit": limit})
                if family in ("impulse", "aliasing"):
                    summary = characterization.setdefault((family, scope, name), [0.0, 0.0])
                    summary[0] = max(summary[0], before); summary[1] = max(summary[1], after)
                if scope == policy["improvement_scopes"].get(family) and name in policy["improvement_scores"]:
                    group = groups.setdefault((old["fixture"]["name"], scope, name), [[], []])
                    group[0].append(before); group[1].append(after)
    if len(identities) != 1:
        raise ValueError("candidate mixes demosaic algorithms/versions")
    improvements = []
    for (probe, scope, name), (old, new) in sorted(groups.items()):
        before, after = math.fsum(old) / len(old), math.fsum(new) / len(new)
        limit = before * (1 - policy["improvement_fraction"])
        improvements.append({"probe": probe, "scope": scope, "score": name, "pair_count": len(old),
                             "baseline_mean": before, "candidate_mean": after, "limit": limit, "passed": after <= limit})
    distinct = identities != {canonical(LEGACY)}
    gates = {"correctness": not failures, "per_case_regression": not regressions,
             "edge_detail_improvement": bool(improvements) and all(g["passed"] for g in improvements),
             "distinct_algorithm_version": distinct}
    return {"comparison_schema_version": 1, "policy": policy,
            "policy_sha256": hashlib.sha256(canonical(policy).encode()).hexdigest(),
            "baseline_report_sha256": h.digest(h.report_json(base).encode()),
            "candidate_report_sha256": h.digest(h.report_json(candidate).encode()),
            "candidate_demosaic": json.loads(next(iter(identities))), "pair_count": len(paired),
            "parity_passed": parity and not failures, "replacement_passed": all(gates.values()), "gates": gates,
            "correctness_failures": failures, "regressions": regressions, "improvements": improvements,
            "characterization_maxima": [{"family": f, "scope": s, "score": n, "baseline": v[0], "candidate": v[1]}
                                         for (f, s, n), v in sorted(characterization.items())]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="preserved .json.gz with verified index")
    parser.add_argument("candidate", type=Path, help="full native-harness JSON")
    parser.add_argument("output", type=Path)
    parser.add_argument("--require", choices=("replacement", "parity"), default="replacement")
    args = parser.parse_args()
    try:
        result = compare(h.read_baseline(args.baseline), json.loads(args.candidate.read_text(encoding="utf-8")))
    except (ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(h.report_json(result), encoding="utf-8")
    print(f"{result['pair_count']} pairs; parity={result['parity_passed']}; replacement={result['replacement_passed']}")
    return 0 if result[args.require + "_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
