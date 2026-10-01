"""Independent arithmetic/read-support and preserved guarded-study checks."""
from collections.abc import Mapping
import copy
from dataclasses import replace
from fractions import Fraction as F
import gzip
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_ha_reference as ha
import raw_menon_guard as g
import raw_menon_reference as m
import raw_quality_harness as h
from raw_menon_reference_tests import sensor

DIRECTORY = Path(__file__).parent / "reference/raw"


class ObservedReads(Mapping):
    def __init__(self, values):
        self.values, self.reads = values, set()

    def __getitem__(self, key):
        self.reads.add(key)
        return self.values[key]

    def __iter__(self):
        return iter(self.values)

    def __len__(self):
        return len(self.values)

    def __contains__(self, key):
        return key in self.values


class GuardTests(unittest.TestCase):
    @classmethod
    def study_inputs(cls):
        if not hasattr(cls, "_study_inputs"):
            cls._study_inputs = (h.read_baseline(DIRECTORY / "bilinear_baseline_v3.json.gz"),
                                 g.diagnostics.read_study(DIRECTORY / "menon_scalar_research_v1.json.gz"))
        return cls._study_inputs

    @classmethod
    def quick_report(cls):
        if not hasattr(cls, "_quick_report"):
            cls._quick_report = g.evaluate(*cls.study_inputs(), quick=True)
        return cls._quick_report

    def test_independent_rational_guard_signed_values_and_exact_zero_ties(self):
        kept = g.interpolate(.5, .25, .75, .125, .375, .125, .375)
        self.assertEqual(kept.green_curvature, float(F(1, 2) - (F(1, 8) + F(3, 8)) / 2))
        self.assertEqual(kept.color_curvature, float((F(1, 4) + F(3, 4) - F(1, 8) - F(3, 8)) / 16))
        self.assertTrue(kept.kept)
        self.assertEqual(kept.value, .75)
        opposing = g.interpolate(.5, .25, .75, .125, .375, .75, .75)
        self.assertFalse(opposing.kept)
        self.assertEqual(opposing.value, .5)
        negative = g.interpolate(-.25, 1.5, 1.5, .125, .375, 2, 2)
        self.assertTrue(negative.kept)
        self.assertEqual(negative.value, 1)
        headroom = g.interpolate(.5, 1.5, 1.5, .25, .25, 1, 1)
        self.assertTrue(headroom.kept)
        self.assertEqual(headroom.value, 1.75)
        for zero in (g.interpolate(.25, .25, .75, .125, .375, .125, .375),
                     g.interpolate(.5, .25, .75, .125, .375, .25, .75)):
            self.assertFalse(zero.kept)
            self.assertEqual(zero.value, .5)
        signed_zero = g.interpolate(.5, -0., -0., 0., 0., 0., 0.)
        self.assertEqual(math.copysign(1, signed_zero.product), -1)
        self.assertFalse(signed_zero.kept)
        for index in range(7):
            values = [.5] * 7
            for bad in (float("inf"), float("nan")):
                values[index] = bad
                with self.assertRaisesRegex(ValueError, "finite"):
                    g.interpolate(*values)

    def test_symbolic_support_and_actual_observation_reads_all_cfa_phase_sites(self):
        proof = g.support_report()
        self.assertEqual(proof["stage_radii"], g.RADII)
        self.assertEqual(proof["cfa_phase_site_cases"], 64)
        self.assertEqual(proof["active_layout_cases"], 256)
        self.assertEqual(proof["channel_output_checks"], 137088)
        self.assertEqual(proof["interior_point_checks"], 3744)
        self.assertTrue(proof["active_confinement_passed"])
        for pattern in range(4):
            for phase in ((0, 0), (1, 0), (0, 1), (1, 1)):
                geometry = g.Support(h.PATTERNS[pattern], phase)
                for x, y in ((12, 12), (12, 13), (13, 12), (13, 13)):
                    p = x, y
                    for c in range(3):
                        ref = g.BayerReference(*sensor(lambda xx, yy: ((17 * xx + 11 * yy + xx * yy) % 32 / 32,) * 3,
                                                       size=(27, 27), pattern=pattern, phase=phase))
                        ref.observed = ObservedReads(ref.observed)
                        ref.base(p, c)
                        expected = geometry.base(p, c)
                        self.assertTrue(ref.observed.reads <= expected)
                        self.assertLessEqual(g.support.radius(expected, p), 6)
                        if c == geometry.channel(p):
                            self.assertEqual(ref.observed.reads, {p})
                        elif geometry.channel(p) == 1:
                            e = geometry.axis_for_color(p, c)
                            for n in (-3, 3):
                                q = g.support.shifted(p, e, n)
                                self.assertIn(q, ref.observed.reads)
                                self.assertEqual(geometry.channel(q), c)

    def test_exact_signed_affine_interiors_and_observations_all_metadata(self):
        rgb = lambda x, y: (-.25 + x / 128 + y / 256, .125 + x / 256 - y / 128, 1.25 - x / 128 + y / 256)
        for pattern in range(4):
            for phase in ((0, 0), (1, 0), (0, 1), (1, 1)):
                for origin in ((0, 0), (3, 5)):
                    ref = g.BayerReference(*sensor(rgb, pattern=pattern, phase=phase, origin=origin))
                    image = ref.render()
                    for y in range(ref.height):
                        for x in range(ref.width):
                            p = x + origin[0], y + origin[1]
                            values = image.pixels[(y * ref.width + x) * 3:(y * ref.width + x) * 3 + 3]
                            self.assertEqual(values[ref.colors[p]], rgb(x, y)[ref.colors[p]])
                            if ref.margin(p) >= 6:
                                self.assertEqual(values, rgb(x, y))

    def test_thin_minimum_borders_and_internal_stage_are_distinct(self):
        rgb = lambda x, y: (x * x / 1024, x * x / 512, .5 - x * x / 1024)
        for size in ((1, 1), (1, 19), (19, 1), (12, 19), (13, 13), (21, 19)):
            args = sensor(rgb, size=size, origin=(3, 5), pattern=2, phase=(1, 1))
            ref, independent = g.BayerReference(*args), ha.BayerReference(*args)
            image = ref.render()
            for y in range(ref.bounds[1], ref.bounds[1] + ref.height):
                for x in range(ref.bounds[0], ref.bounds[0] + ref.width):
                    if ref.margin((x, y)) < 6:
                        self.assertEqual(ref.pixel(x, y), tuple(independent.bilinear(x, y, c) for c in range(3)))
            self.assertEqual(image.bounds, ref.bounds)
        ref = g.BayerReference(*sensor(rgb, size=(13, 13)))
        p = 5, 6  # Green at margin five: final fallback, internal guard valid.
        self.assertNotEqual(ref.color_at_green(p, 0), ref.pixel(*p)[0])
        self.assertEqual(ref.margin(p), 5)
        ref.pixel(6, 6)  # Minimum full-support output composes those stages.
        with self.assertRaises(ValueError):
            ref.guard_details((3, 6), 0)

    def test_roi_order_ownership_hostile_padding_and_decision_denominator(self):
        args = sensor(lambda x, y: ((17 * x + 11 * y) % 32 / 32, .25 + (x % 8) / 32, 1.25 - (y % 8) / 32),
                      origin=(3, 5), pattern=3, phase=(0, 1))
        ref = g.BayerReference(*args)
        full = ref.render()
        counts = g.guard_summary(ref)
        self.assertEqual(counts["total"], sum(ref.colors[x, y] == 1 for y in range(11, 22) for x in range(9, 22)) * 2)
        self.assertEqual(sum(counts[k] for k in ("kept", "opposing_signs", "zero_product")), counts["total"])
        clone = g.BayerReference(*args)
        for y, height in ((18, 10), (5, 7), (12, 6)):
            roi = clone.render((3, y, 25, height))
            first = (y - 5) * 25 * 3
            self.assertEqual(roi.pixels, full.pixels[first:first + 25 * height * 3])
        self.assertEqual(clone.render((3, 5, 0, 0)).pixels, ())
        self.assertEqual(g.guard_summary(clone), counts)
        before = full.pixels
        samples, width, height, meta = args
        for y in range(height):
            for x in range(meta["row_stride_samples"]):
                if not (3 <= x < 28 and 5 <= y < 28):
                    samples[y * meta["row_stride_samples"] + x] = 0
        self.assertEqual(g.BayerReference(*args).render().pixels, before)
        samples[:] = [0] * len(samples)
        meta["black_levels"][:] = [0] * 4
        self.assertEqual(ref.render().pixels, before)

    def test_refinement_invalid_controls_and_inherited_sensor_budget_are_rejected(self):
        ref = g.BayerReference(*sensor(lambda x, y: (.25,) * 3))
        for fn in (lambda: ref.render(refine=True), lambda: ref.pixel(12, 12, refine=True),
                   lambda: ref.render(refine=0), lambda: ref.refined((12, 12), 0),
                   lambda: ref.refined_green((12, 12)), lambda: ref.after_green_locations((12, 12), 0),
                   lambda: ref.guard_details((12, 12), 1), lambda: ref.guard_details((13, 12), True),
                   lambda: ref.render((0, 0, 26, 23)), lambda: ref.pixel(True, 12)):
            with self.assertRaises(ValueError):
                fn()
        geometry = g.Support()
        with self.assertRaises(ValueError):
            geometry.output(g.support.Bounds(0, 0, 25, 23), (12, 12), 0, refine=True)
        args = sensor(lambda x, y: (.25,) * 3)
        with patch.object(m, "MAX_SAMPLES", 10):
            with self.assertRaisesRegex(ValueError, "budget"):
                g.BayerReference(*args)

    def test_measured_sine_counterexample_suppresses_the_wrong_signed_correction(self):
        fixture = h.generate(next(p for p in h.ALL_PROBES if p.name == "chromatic_sine_1_8_x"))
        args = fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata
        ref, original = g.BayerReference(*args), m.BayerReference(*args)
        decision = ref.guard_details((35, 24), 0)
        self.assertFalse(decision.kept)
        self.assertLess(decision.green_curvature, 0)
        self.assertGreater(decision.color_curvature, 0)
        self.assertEqual(decision.value, ref.bilinear((35, 24), 0))
        self.assertAlmostEqual(decision.value, .775, places=7)
        truth = fixture.truth.pixels[(24 * 65 + 35) * 3]
        self.assertLess(abs(decision.value - truth), abs(original.pixel(35, 24, refine=False)[0] - truth))

    def test_quick_evaluation_pairing_scopes_guard_counts_and_tamper_refusal(self):
        baseline, original = self.study_inputs()
        record = self.quick_report()
        self.assertEqual(record["paired_case_count"], 148)
        self.assertEqual(record["research_identity"], g.IDENTITY)
        self.assertEqual(record["variant"]["research_identity"], g.IDENTITY)
        self.assertFalse(record["native_replacement_accepted"])
        self.assertEqual(len(record["variant"]["improvements"]), 24)
        self.assertTrue(all(row["pair_count"] == 4 for row in record["variant"]["improvements"]))
        self.assertTrue(all(c["measured_correctness"]["observed_fidelity"] and c["measured_correctness"]["quantization"]
                            for c in record["variant"]["cases"]))
        for name in ("kept", "opposing_signs", "zero_product", "total"):
            self.assertEqual(record["guard_decision_totals"][name], sum(c["guard_decisions"][name] for c in record["variant"]["cases"]))
        bad = copy.deepcopy(original)
        bad["variants"]["base"]["cases"][0]["rendered_float32_le_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "original study identity"):
            g.evaluate(baseline, bad, quick=True)
        bad = copy.deepcopy(baseline)
        bad["cases"][0]["fixture"]["bayer"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "pairing"):
            g.evaluate(bad, original, quick=True)
        generate = h.generate
        def corrupt(*args, **kwargs):
            fixture = generate(*args, **kwargs)
            return replace(fixture, bayer_le=b"\x00\x00" + fixture.bayer_le[2:])
        with patch.object(h, "generate", side_effect=corrupt):
            with self.assertRaisesRegex(ValueError, "fixture differs"):
                g.evaluate(baseline, original, quick=True)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, "2368"):
                g.preserve(Path(tmp) / "partial.json.gz", record)

    def test_preserved_full_report_provenance_all_pairs_and_quick_regeneration(self):
        payload = gzip.decompress((DIRECTORY / "menon_guarded_research_v1.json.gz").read_bytes())
        record = json.loads(payload)
        index = json.loads((DIRECTORY / "menon_guarded_research_v1.index.json").read_bytes())
        self.assertEqual(index["compressed_sha256"], h.digest((DIRECTORY / "menon_guarded_research_v1.json.gz").read_bytes()))
        self.assertEqual(index["report_json_sha256"], h.digest(payload))
        self.assertEqual(record["reference_source_sha256_lf"], m.source_digest(g))
        for name, digest in record["helper_source_sha256_lf"].items():
            self.assertEqual(digest, h.digest((Path(g.__file__).parent / name).read_bytes().replace(b"\r\n", b"\n")))
        self.assertEqual(record["policy_sha256"], h.digest(g.comparison.POLICY_PATH.read_bytes()))
        self.assertEqual(record["policy"], json.loads(g.comparison.POLICY_PATH.read_bytes()))
        self.assertEqual(record["original_scalar_report_sha256"], g.ORIGINAL_REPORT_SHA256)
        self.assertEqual(record["support_proof"], g.support_report())
        self.assertEqual(record["paired_case_count"], 2368)
        self.assertEqual(record["variant"]["case_count"], 2368)
        self.assertFalse(record["native_replacement_accepted"])
        self.assertEqual(record["variant"]["research_identity"], g.IDENTITY)
        self.assertTrue(all(case["measured_correctness"]["observed_fidelity"] and case["measured_correctness"]["quantization"]
                            for case in record["variant"]["cases"]))
        baseline, original = self.study_inputs()
        self.assertEqual({tuple(case["case"]) for case in record["variant"]["cases"]}, {g.comparison.case_key(c) for c in baseline["cases"]})
        self.assertTrue(all(row["pair_count"] == 64 for row in record["variant"]["improvements"]))
        quick = self.quick_report()
        frozen = {tuple(case["case"]): case for case in record["variant"]["cases"]}
        for case in quick["variant"]["cases"]:
            self.assertEqual(h.report_json(case), h.report_json(frozen[tuple(case["case"])]))

    def test_full_preservation_idempotence_and_overwrite_refusal(self):
        payload = gzip.decompress((DIRECTORY / "menon_guarded_research_v1.json.gz").read_bytes())
        record = json.loads(payload)
        original_index = json.loads((DIRECTORY / "menon_guarded_research_v1.index.json").read_bytes())
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "menon_guarded_research_v1.json.gz"
            index = g.preserve(target, record)
            packed = target.read_bytes()
            self.assertEqual(gzip.decompress(packed), payload)
            self.assertEqual({k: v for k, v in index.items() if k != "compressed_sha256"},
                             {k: v for k, v in original_index.items() if k != "compressed_sha256"})
            self.assertEqual(g.preserve(target, record), index)
            self.assertEqual(target.read_bytes(), packed)
            index_path = target.with_suffix("").with_suffix(".index.json")
            index_path.write_bytes(b"different index\n")
            with self.assertRaisesRegex(ValueError, "replace"):
                g.preserve(target, record)
            self.assertEqual(target.read_bytes(), packed)
            self.assertEqual(index_path.read_bytes(), b"different index\n")


if __name__ == "__main__":
    unittest.main()
