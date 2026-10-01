"""Independent analytical calculations and evidence checks for budget audit v1."""
import copy
from fractions import Fraction as F
import gzip
import itertools
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_ha_reference as ha
import raw_quality_harness as h
import raw_quantization_audit as audit


def exact_fixture(probe, pattern, phase, origin):
    ox, oy = origin
    w, height = 65, 49
    sw, sh, stride = ox + w + 2, oy + height + 2, ox + w + 5
    black = (256, 512, 768, 1024)
    meta = {"row_stride_samples": stride, "pattern": pattern, "cfa_phase_x": phase[0], "cfa_phase_y": phase[1],
            "active_x": ox, "active_y": oy, "active_width": w, "active_height": height,
            "black_levels": black, "white_levels": tuple(b + 1024 for b in black)}
    truth = tuple(float(v) for y in range(height) for x in range(w) for v in audit.ideal_rgb(probe, x, y))
    samples = [65535] * (stride * sh)
    for y in range(height):
        for x in range(w):
            site, c = h.site_channel(meta, ox + x, oy + y)
            value = truth[(y * w + x) * 3 + c]
            code = h.encode_sample(value, black[site], black[site] + 1024)
            if F(code - black[site], 1024) != F(value):
                raise ValueError("test fixture must be exactly encoded")
            samples[(oy + y) * stride + ox + x] = code
    return h.Fixture(probe, "exact", "exact-power-of-two", 0, h.RgbImage(w, height, truth, origin),
                     sw, sh, meta, h.little_bytes("H", samples))


class AuditTests(unittest.TestCase):
    def test_uniform_green_axial_and_diagonal_bounds_by_hand(self):
        q = audit.Budget(F(1, 2048))
        green = audit.green_budget(q, q, q, q, q)
        self.assertEqual(green.quantization, F(1, 1024))
        self.assertEqual(audit.chroma_budget(q, green, green, q, q).quantization, F(1, 512))
        self.assertEqual(audit.chroma_budget(green, green, green, q, q).quantization, F(5, 2048))
        self.assertEqual(green.roundoff, 0)

    def test_green_bound_contains_every_independent_input_error_sign(self):
        limits = (F(1, 100), F(1, 200), F(1, 300), F(1, 400), F(1, 500))
        budget = audit.green_budget(*(audit.Budget(v) for v in limits))
        largest = F(0)
        for signs in itertools.product((-1, 1), repeat=5):
            c, a, b, near_a, near_b = (s * q for s, q in zip(signs, limits))
            error = (near_a + near_b) / 2 + (2 * c - a - b) / 4
            largest = max(largest, abs(error))
            self.assertLessEqual(abs(error), budget.quantization)
        self.assertEqual(largest, budget.quantization)

    def test_chroma_bound_contains_every_stage_error_sign(self):
        limits = (F(1, 100), F(1, 200), F(1, 300), F(1, 400), F(1, 500))
        budget = audit.chroma_budget(*(audit.Budget(v) for v in limits))
        for signs in itertools.product((-1, 1), repeat=5):
            center, a, b, ca, cb = (s * q for s, q in zip(signs, limits))
            self.assertLessEqual(abs((ca + cb) / 2 + center - (a + b) / 2), budget.quantization)

    def test_tie_envelope_preserves_separate_components(self):
        first, second = audit.Budget(F(1, 2), F(1, 10)), audit.Budget(F(1, 4), F(1, 3))
        envelope = audit.envelope(first, second)
        self.assertEqual(envelope, audit.Budget(F(1, 2), F(1, 3)))
        self.assertGreaterEqual(envelope.total, ((first + second) * F(1, 2)).total)
        with self.assertRaises(ValueError):
            first * F(-1)

    def test_truth_storage_is_distinct_from_sensor_quantization(self):
        mid = h.generate(next(p for p in h.PROBES if p.name == "neutral_mid"))
        magnitude, storage = audit.truth_profile(mid)
        self.assertEqual(magnitude, (F(mid.probe.constant[0]),) * 3)
        self.assertEqual(storage, (abs(F(mid.truth.pixels[0]) - F(mid.probe.constant[0])),) * 3)
        self.assertGreater(storage[0], 0)
        zero = h.generate(next(p for p in h.PROBES if p.name == "neutral_zero"))
        self.assertEqual(audit.truth_profile(zero), ((F(0),) * 3, (F(0),) * 3))

    def test_exact_signed_headroom_and_affine_reconstruction_all_patterns_phases_origins(self):
        probes = (h.Probe("exact_flat", "flat", False, (-0.125, 0.5, 1.125)),
                  h.Probe("exact_affine", "affine", False, (-0.125, 0.25, 1.125),
                          (1/1024, 1/512, -1/512), (1/512, -1/1024, 1/1024)))
        for probe in probes:
            for pattern in range(4):
                for phase in itertools.product(range(2), repeat=2):
                    for origin in ((0, 0), (1, 3)):
                        fixture = exact_fixture(probe, pattern, phase, origin)
                        image = ha.BayerReference(fixture.native_samples(), fixture.sensor_width,
                                                 fixture.sensor_height, fixture.metadata).render()
                        scope = fixture.truth.bounds if probe.family == "flat" else (origin[0]+1, origin[1]+1, 63, 47)
                        metrics = h.measure(image, fixture.truth, scope)
                        self.assertEqual(metrics["rgb"]["max_abs"], 0)
                        self.assertEqual(h.observed_metrics(fixture, image)["quantization"]["observed"]["max_abs"], 0)
                        self.assertEqual(audit.truth_profile(fixture)[1], (F(0),) * 3)

    def test_deliberately_quantized_fixtures_fit_derived_channel_bounds(self):
        for probe in h.PROBES:
            profile = audit.truth_profile(h.generate(probe))
            for pattern in range(4):
                for phase in itertools.product(range(2), repeat=2):
                    fixture = h.generate(probe, layout="shifted" if phase[0] else "packed", pattern=pattern,
                                         phase=phase, levels="per-site")
                    reference = ha.BayerReference(fixture.native_samples(), fixture.sensor_width,
                                                 fixture.sensor_height, fixture.metadata)
                    image = reference.render()
                    scope = fixture.truth.bounds if probe.family == "flat" else (fixture.truth.origin[0]+1, fixture.truth.origin[1]+1, 63, 47)
                    metrics = h.measure(image, fixture.truth, scope)
                    budgets = audit.fixture_budgets(fixture, profile)
                    for c, budget in zip(h.CHANNELS, budgets):
                        self.assertLessEqual(F(metrics["channels"][c]["max_abs"]), budget.total)
                    # The harness fidelity metric compares to binary64
                    # normalization. The scalar copies its float32 observation
                    # exactly, whose normalization rounding is accounted for.
                    self.assertLessEqual(h.observed_metrics(fixture, image)["observed_fidelity"]["observed"]["max_abs"], 1e-6)
                    for (x, y), observed in reference.observed.items():
                        c = reference.colors[x, y]
                        i = ((y-fixture.truth.origin[1])*65 + x-fixture.truth.origin[0])*3 + c
                        self.assertEqual(image.pixels[i], observed)

    def test_counterexample_exact_decomposition_and_original_failure_remain(self):
        result = audit.counterexample()
        self.assertEqual(result["rendered"], 0.3711903989315033)
        self.assertEqual(result["truth"], 0.37123000621795654)
        self.assertEqual(result["signed_error"], -3.960728645324707e-5)
        self.assertTrue(result["exact_rational_decomposition_closed"])
        self.assertGreater(abs(result["signed_error"]), 0.5/13000 + 1e-6)
        self.assertGreater(result["selected_stencil_quantization_bound"], abs(result["sensor_quantization_contribution"]))
        # Independently group the displayed stencil by original sensor site:
        # green-even-column 1, red 1, green-odd-column 1/2 in absolute weight.
        expected = F(1, 26000) + F(1, 48000) + F(1, 80000) + F(1, 64000)
        self.assertEqual(result["selected_stencil_quantization_bound"], float(expected))
        self.assertEqual(result["stage_roundoff_contribution"], 2**-26)
        self.assertAlmostEqual(sum(result[k] for k in ("sensor_quantization_contribution", "normalization_roundoff_contribution",
                                                      "truth_storage_contribution", "stage_roundoff_contribution")), result["signed_error"], places=18)

    def test_source_verification_normalizes_only_checkout_newlines(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "source.py"
            path.write_bytes(b"value = 1\n")
            expected = audit.source_digest(path)
            path.write_bytes(b"value = 1\r\n")
            self.assertEqual(audit.source_digest(path), expected)
            path.write_bytes(b"value = 2\r\n")
            self.assertNotEqual(audit.source_digest(path), expected)

    def test_full_preserved_evidence_audit_does_not_revise_policy(self):
        baseline = h.read_baseline(audit.EVIDENCE / "bilinear_baseline_v3.json.gz")
        policy_before = (audit.EVIDENCE / "replacement_policy_v1.json").read_bytes()
        baseline_before = (audit.EVIDENCE / "bilinear_baseline_v3.json.gz").read_bytes()
        report = audit.evaluate(baseline)
        self.assertEqual(report["summary"], {"analytical_cases": 448, "frozen_analytical_failures": 144,
                                          "derived_budget_failures": 0, "observed_fidelity_failures": 0,
                                          "quantization_failures": 0, "frozen_regression_checks": 2784,
                                          "frozen_improvement_gates_passed": 16, "frozen_improvement_gates_total": 24})
        self.assertFalse(report["native_replacement_accepted"])
        self.assertEqual((audit.EVIDENCE / "replacement_policy_v1.json").read_bytes(), policy_before)
        self.assertEqual((audit.EVIDENCE / "bilinear_baseline_v3.json.gz").read_bytes(), baseline_before)
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            audit.preserve(report, directory)
            audit.preserve(report, directory)
            index = json.loads((directory / "ha_error_budget_audit_v1.index.json").read_text())
            compressed = (directory / index["filename"]).read_bytes()
            data = gzip.decompress(compressed)
            self.assertEqual(h.digest(compressed), index["compressed_sha256"])
            self.assertEqual(h.digest(data), index["report_json_sha256"])
            self.assertEqual(json.loads(data), report)
            changed = copy.deepcopy(report)
            changed["native_replacement_accepted"] = True
            with self.assertRaisesRegex(ValueError, "refusing to replace"):
                audit.preserve(changed, directory)
            self.assertEqual((directory / index["filename"]).read_bytes(), compressed)
        bad = copy.deepcopy(baseline)
        bad["cases"][0]["fixture"]["bayer"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "provenance mismatch"):
            audit.evaluate(bad)

    def test_nonanalytical_and_wrong_size_budget_requests_reject(self):
        fixture = h.generate(h.EDGE_PROBES[0])
        with self.assertRaises(ValueError):
            audit.fixture_budgets(fixture)
        from dataclasses import replace
        fixture = h.generate(h.PROBES[0])
        with self.assertRaises(ValueError):
            audit.fixture_budgets(replace(fixture, truth=h.RgbImage(1, 1, (0, 0, 0))))


if __name__ == "__main__":
    unittest.main()
