"""Paired policy arithmetic and refusal of mismatched evidence.

Altered metric records below exercise the comparator only, not candidate quality.
"""
import copy
import json
import math
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_quality_harness as h
import raw_quality_compare as c


class ComparisonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.baseline = h.read_baseline(Path(__file__).resolve().parent / "reference/raw/bilinear_baseline_v3.json.gz")

    def candidate(self):
        record = copy.deepcopy(self.baseline)
        policy = {"algorithm": "test.synthetic-only", "processing_version": 1}
        record["algorithm"]["demosaic"] = policy
        for case in record["cases"]:
            case["saved_manifest"]["format_version"] = 3
            case["saved_manifest"]["sources"][0]["demosaic"] = policy
            case["source_info"]["demosaic"] = policy
        return record

    def test_self_parity_cannot_pass_replacement_and_order_is_irrelevant(self):
        candidate = {**self.baseline, "cases": list(reversed(self.baseline["cases"]))}
        result = c.compare(self.baseline, candidate)
        self.assertEqual(result["pair_count"], 2368)
        self.assertTrue(result["parity_passed"])
        self.assertFalse(result["replacement_passed"])
        self.assertFalse(result["gates"]["edge_detail_improvement"])
        self.assertFalse(result["gates"]["distinct_algorithm_version"])
        self.assertEqual(len(result["improvements"]), 24)
        self.assertTrue(all(g["pair_count"] == 64 for g in result["improvements"]))

    def test_legacy_and_explicit_identity_have_exact_parity(self):
        candidate = self.candidate()
        candidate["algorithm"]["demosaic"] = c.LEGACY
        for case in candidate["cases"]:
            case["source_info"]["demosaic"] = c.LEGACY
            case["saved_manifest"]["sources"][0]["demosaic"] = c.LEGACY
        self.assertTrue(c.compare(self.baseline, candidate)["parity_passed"])

    def test_known_twenty_percent_improvement_and_per_case_regression_cap(self):
        candidate = self.candidate()
        def scale(value):
            for key, child in value.items():
                if isinstance(child, dict): scale(child)
                elif child is not None and key not in ("pixel_count", "sample_count"): value[key] *= 0.8
        for case in candidate["cases"]:
            if case["fixture"]["family"] in ("flat", "affine"): continue
            for metric in case["metrics"].values():
                if "rgb" in metric: scale(metric)
        result = c.compare(self.baseline, candidate)
        self.assertTrue(result["replacement_passed"])
        self.assertFalse(result["parity_passed"])
        for g in result["improvements"]:
            self.assertAlmostEqual(g["candidate_mean"], g["baseline_mean"] * 0.8, delta=1e-15)
            self.assertAlmostEqual(g["limit"], g["baseline_mean"] * 0.9, delta=1e-15)
        i = next(i for i, case in enumerate(candidate["cases"]) if case["fixture"]["family"] == "aliasing")
        candidate["cases"][i]["metrics"]["whole"]["rgb"]["max_abs"] = self.baseline["cases"][i]["metrics"]["whole"]["rgb"]["max_abs"] * 1.1
        result = c.compare(self.baseline, candidate)
        self.assertFalse(result["replacement_passed"])
        self.assertEqual(len(result["regressions"]), 1)
        self.assertEqual(result["regressions"][0]["scope"], "whole")

    def test_mismatch_and_invalid_measurements_reject(self):
        first = self.baseline["cases"][0]
        def changed(action):
            case = copy.deepcopy(first); action(case)
            return {**self.baseline, "cases": [case, *self.baseline["cases"][1:]]}
        actions = [lambda x: x["fixture"]["bayer"].update(sha256="changed"),
                   lambda x: x["source_info"].update(content_sha256="changed"),
                   lambda x: x["render_request"].update(mip=1),
                   lambda x: x["measurement_scopes"].update(interior_margin=2),
                   lambda x: x["metrics"]["whole"]["rgb"].update(sample_count=1),
                   lambda x: x["metrics"]["whole"]["rgb"].update(rmse=math.nan),
                   lambda x: x["metrics"]["whole"]["rgb"].update(rmse=True),
                   lambda x: x["metrics"]["whole"]["rgb"].update(rmse=-1),
                   lambda x: x["metrics"]["whole"]["rgb"].update(rmse=2),
                   lambda x: x["saved_manifest"].update(format_version=3),
                   lambda x: x["roi_checks"][0].update(bounds=[0, 0, 1, 1]),
                   lambda x: x["acceptance"]["gates"].update(tile_exact=False)]
        for action in actions:
            with self.assertRaises(ValueError): c.compare(self.baseline, changed(action))
        for candidate in ({**self.baseline, "cases": self.baseline["cases"][:-1]},
                          {**self.baseline, "cases": [first, *self.baseline["cases"][:-1]]},
                          {**self.baseline, "metric_formula_version": 2}):
            with self.assertRaises(ValueError): c.compare(self.baseline, candidate)

    def test_recomputed_correctness_blocks_failed_fidelity(self):
        candidate = self.candidate()
        case = candidate["cases"][0]
        case["metrics"]["observed_fidelity"]["observed"]["max_abs"] = 0.01
        case["acceptance"]["gates"]["observed_sample_fidelity"] = False
        case["acceptance"]["passed"] = False
        candidate["summary"].update(passed=False, passed_cases=2367)
        result = c.compare(self.baseline, candidate)
        self.assertFalse(result["gates"]["correctness"])
        self.assertFalse(result["replacement_passed"])
        self.assertEqual(result["correctness_failures"][0]["gate"], "observed_sample_fidelity")
        case["acceptance"]["gates"]["observed_sample_fidelity"] = True
        with self.assertRaises(ValueError): c.compare(self.baseline, candidate)

    def test_mixed_or_false_header_identity_and_unversioned_policy_reject(self):
        candidate = self.candidate()
        candidate["cases"][0]["source_info"]["demosaic"] = c.LEGACY
        with self.assertRaises(ValueError): c.compare(self.baseline, candidate)
        candidate = self.candidate(); candidate["algorithm"]["demosaic"] = c.LEGACY
        with self.assertRaises(ValueError): c.compare(self.baseline, candidate)
        policy = json.loads(c.POLICY_PATH.read_text())
        policy["improvement_fraction"] = 0
        with self.assertRaises(ValueError): c.compare(self.baseline, self.baseline, policy)


if __name__ == "__main__":
    unittest.main()
