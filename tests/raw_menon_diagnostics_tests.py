"""Checks for research pairing, independent trace arithmetic and publication scope."""
import copy
from fractions import Fraction as F
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_menon_diagnostics as d
import raw_menon_reference as m
import raw_quality_harness as h
from raw_menon_reference_tests import sensor

DIRECTORY = Path(__file__).parent / "reference/raw"


class DiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.baseline = h.read_baseline(DIRECTORY / "bilinear_baseline_v3.json.gz")
        cls.study = d.read_study(DIRECTORY / "menon_scalar_research_v1.json.gz")
        cls.record, cls.frames = d.evaluate(cls.baseline, cls.study)

    def test_trace_reproduces_independently_encoded_signed_affines_all_metadata(self):
        rgb = lambda x, y: (-.25 + x / 128 + y / 256, .125 + x / 256 - y / 128, 1.25 - x / 128 + y / 256)
        for pattern in range(4):
            for phase in ((0, 0), (1, 0), (0, 1), (1, 1)):
                for origin in ((0, 0), (3, 5)):
                    ref = m.BayerReference(*sensor(rgb, pattern=pattern, phase=phase, origin=origin))
                    truth = h.RgbImage(25, 23, tuple(v for y in range(23) for x in range(25) for v in rgb(x, y)), origin)
                    fixture = SimpleNamespace(truth=truth)
                    point = origin[0] + 12, origin[1] + 10
                    for stage in d.STAGES:
                        for c in range(3):
                            trace = d.stage_trace(fixture, ref, point, c, stage)
                            self.assertEqual(trace["value"], rgb(12, 10)[c])
                            self.assertLess(abs(trace["error_components"]["ideal_stencil_model_error"]), 3e-16)
                            self.assertEqual(trace["error_components"]["propagated_input_error"], 0)
                            for row in trace["terms"]:
                                self.assertEqual(row["active_local"], [row["sensor"][i] - origin[i] for i in (0, 1)])

    def test_green_curvature_error_has_independent_rational_value(self):
        rgb = lambda x, y: (-.25, x * x / 1024, 1.25)
        ref = m.BayerReference(*sensor(rgb, size=(25, 25)))
        fixture = SimpleNamespace(truth=h.RgbImage(25, 25, tuple(v for y in range(25) for x in range(25) for v in rgb(x, y))))
        horizontal = d.directional_trace(fixture, ref, (12, 12), 0)
        vertical = d.directional_trace(fixture, ref, (12, 12), 1)
        # Constant observed red cannot cancel the independently curved green.
        self.assertEqual(horizontal["error"], float(F(11**2 + 13**2, 2 * 1024) - F(12**2, 1024)))
        self.assertEqual(horizontal["error_components"]["ideal_stencil_model_error"], 1 / 1024)
        self.assertEqual(horizontal["error_components"]["propagated_input_error"], 0)
        self.assertEqual(vertical["error"], 0)

    def test_preserved_report_matches_regeneration_and_frozen_pairing(self):
        payload = (DIRECTORY / "menon_scalar_diagnostics_v1.json").read_bytes()
        self.assertEqual(payload, h.report_json(self.record).encode())
        self.assertEqual(self.record["reference_source_sha256_lf"], m.source_digest(m))
        self.assertEqual(self.record["scalar_report_sha256"], h.digest(h.report_json(self.study).encode()))
        self.assertEqual(tuple(case["case"][0] for case in self.record["cases"]), d.PROBES)
        self.assertFalse(self.record["native_replacement_accepted"])
        self.assertNotIn("report_schema_version", self.record)
        for case, images in zip(self.record["cases"], self.frames):
            self.assertEqual(case["case"][1:], ["packed", 0, 0, 0, "uniform"])
            for name, im in images.items():
                self.assertEqual(case["rendered_float32_le_sha256"][name], h.digest(h.little_bytes("f", im.pixels)))

    def test_selected_pixel_crop_and_profile_have_explicit_original_coordinates(self):
        for case, images in zip(self.record["cases"], self.frames):
            point = case["selected_pixel"]
            x, y = point["active_local"]
            c = h.CHANNELS.index(point["channel"])
            truth, base, bilinear = (images[name].pixels for name in ("truth", "base", "bilinear"))
            i = (y * 65 + x) * 3 + c
            excess = (base[i] - truth[i]) ** 2 - (bilinear[i] - truth[i]) ** 2
            self.assertGreaterEqual(x, 8)
            self.assertLess(x, 57)
            self.assertGreaterEqual(y, 8)
            self.assertLess(y, 41)
            for yy in range(8, 41):
                for xx in range(8, 57):
                    own = h.site_channel(case["fixture"]["metadata"], xx, yy)[1]
                    for cc in range(3):
                        if cc == own:
                            continue
                        ii = (yy * 65 + xx) * 3 + cc
                        self.assertLessEqual((base[ii] - truth[ii]) ** 2 - (bilinear[ii] - truth[ii]) ** 2, excess)
            cx, cy, w, height = case["crop_active_local"]
            self.assertTrue(0 <= cx <= x < cx + w <= 65 and 0 <= cy <= y < cy + height <= 49)
            for name, values in case["profile"]["values"].items():
                self.assertEqual(len(values), 65)
                self.assertEqual(values[x], point["values"][name])
            self.assertEqual(case["diagnostic_scope_sensor"], [8, 8, 49, 33])
            self.assertEqual(case["frozen_improvement_scope"], "edge_band" if "edge" in case["case"][0] else "interior")

    def test_sine_failure_is_model_error_with_correct_neighbor_green(self):
        case = next(case for case in self.record["cases"] if case["case"][0] == "chromatic_sine_1_8_x")
        point = case["selected_pixel"]
        self.assertEqual(point["active_local"], [35, 24])
        self.assertEqual(point["channel"], "R")
        self.assertEqual(point["observed_channel"], "G")
        self.assertEqual(point["values"]["base"], point["values"]["refined"])
        # Red-neighbor mean is 0.775. Borrowed green curvature subtracts
        # another ~0.10355 although red and green have different phases.
        self.assertAlmostEqual(point["values"]["bilinear"], .775, places=7)
        self.assertAlmostEqual(point["values"]["base"], .67146665, places=7)
        components = point["stage_traces"]["base"]["error_components"]
        self.assertGreater(abs(components["ideal_stencil_model_error"]), .217)
        self.assertLess(abs(components["propagated_input_error"]), 3.4e-5)
        for choices in point["green_choices"]:
            self.assertEqual(choices["selected"], "V")
            self.assertEqual(choices["predictors_H_V"][1]["error"], 0)
        self.assertEqual(case["green_direction_summary"]["strictly_worse_than_other_predictor_count"], 0)

    def test_all_recorded_stencils_close_and_keep_signed_headroom_truth(self):
        values = []
        for case in self.record["cases"]:
            point = case["selected_pixel"]
            traces = list(point["stage_traces"].values()) + point["upstream_base_input_traces"]
            traces += [trace for choice in point["green_choices"] for trace in choice["predictors_H_V"]]
            for trace in traces:
                self.assertAlmostEqual(sum(trace["error_components"].values()), trace["value"] - trace["truth"], places=13)
                self.assertLess(abs(trace["closure_residual"]), 2e-14)
                for row in trace["terms"]:
                    values.append(row["truth"])
                    self.assertEqual(row["weighted_input_error"], row["weight"] * (row["value"] - row["truth"]))
        self.assertLess(min(values), 0)
        self.assertGreater(max(values), 1)
        self.assertEqual(d.display_byte(-.1), 0)
        self.assertEqual(d.display_byte(1.25), 255)
        self.assertEqual(d.display_byte(.5), 188)

    def test_source_contract_and_fixture_metric_tampering_are_rejected(self):
        for key in ("reference_source_sha256_lf", "policy_sha256"):
            bad = {**self.study, key: "0" * 64}
            with self.assertRaisesRegex(ValueError, "provenance"):
                d.validate_study(bad)
        bad = {**self.study, "helper_source_sha256_lf": {**self.study["helper_source_sha256_lf"], "raw_quality_harness.py": "0" * 64}}
        with self.assertRaisesRegex(ValueError, "helper provenance"):
            d.validate_study(bad)
        with self.assertRaisesRegex(ValueError, "pairing"):
            d.evaluate(self.baseline, {**self.study, "baseline_report_sha256": "0" * 64})
        key = tuple(self.record["cases"][0]["case"])
        old = next(c for c in self.baseline["cases"] if d.comparison.case_key(c) == key)
        expected = {name: next(c for c in v["cases"] if tuple(c["case"]) == key) for name, v in self.study["variants"].items()}
        fixture = h.generate(next(p for p in h.ALL_PROBES if p.name == key[0]))
        bad = copy.deepcopy(old)
        bad["fixture"]["bayer"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "fixture differs"):
            d.diagnose_case(fixture, bad, expected)
        for field in ("rendered_float32_le_sha256", "metrics"):
            bad = copy.deepcopy(expected)
            bad["base"][field] = "0" * 64 if field != "metrics" else {}
            with self.assertRaisesRegex(ValueError, "render/metrics differ"):
                d.diagnose_case(fixture, old, bad)

    def test_index_integrity_and_immutable_report_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "menon_scalar_research_v1.json.gz"
            path.write_bytes((DIRECTORY / path.name).read_bytes())
            index_path = path.with_suffix("").with_suffix(".index.json")
            index = json.loads((DIRECTORY / index_path.name).read_bytes())
            index["report_json_sha256"] = "0" * 64
            index_path.write_text(json.dumps(index), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                d.read_study(path)
            index["compressed_sha256"] = "0" * 64
            index_path.write_text(json.dumps(index), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                d.read_study(path)
            target = Path(tmp) / "diagnostics.json"
            first = m.write_report(target, self.record)
            self.assertEqual(first, m.write_report(target, self.record))
            target.write_bytes(b"different evidence\n")
            with self.assertRaisesRegex(ValueError, "overwrite"):
                m.write_report(target, self.record)
            self.assertEqual(target.read_bytes(), b"different evidence\n")


if __name__ == "__main__":
    unittest.main()
