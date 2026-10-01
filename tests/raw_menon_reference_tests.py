"""Independent arithmetic, sensor ownership and research-evidence checks."""
import copy
from fractions import Fraction as F
import gzip
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_ha_reference as ha
import raw_menon_reference as m
import raw_quality_harness as h


def sensor(rgb, *, size=(25, 23), origin=(0, 0), pattern=0, phase=(0, 0)):
    aw, ah = size
    ox, oy = origin
    width, height = ox + aw + 2, oy + ah + 2
    stride = width + 3
    meta = {"row_stride_samples": stride, "pattern": pattern, "cfa_phase_x": phase[0], "cfa_phase_y": phase[1],
            "active_x": ox, "active_y": oy, "active_width": aw, "active_height": ah,
            "black_levels": [256, 512, 768, 1024], "white_levels": [1280, 1536, 1792, 2048]}
    data = [65535] * (stride * height)
    for y in range(ah):
        for x in range(aw):
            site, channel = h.site_channel(meta, ox + x, oy + y)
            data[(oy + y) * stride + ox + x] = h.encode_sample(rgb(x, y)[channel], meta["black_levels"][site], meta["white_levels"][site])
    return data, width, height, meta


class ReferenceTests(unittest.TestCase):
    def test_hand_green_chroma_and_three_term_refinement(self):
        self.assertEqual(m.green_predictor(.5, .25, .25, .125, .375), .375)
        self.assertEqual(m.green_predictor(.5, .375, .375, .125, .625), .4375)
        self.assertEqual(m.pair_difference(.5, .25, .125, .75, .375), .75)
        self.assertEqual(m.pair_difference(-.25, 1.5, -.5, 1.5, -.5), 1.75)
        self.assertEqual(m.refine_difference(.5, .125, .25, .375, subtract=True), .25)
        self.assertEqual(m.refine_difference(.5, .125, .25, .375), .75)
        self.assertEqual(m.refine_difference(.5, 0, .75, 0), .75)

    def test_exact_tie_chooses_horizontal_predictor_on_actual_quadratic_sensor(self):
        ref = m.BayerReference(*sensor(lambda x, y: (x * x / 1024,) * 3, size=(25, 25)))
        p = (12, 12)
        self.assertEqual(ref.classifiers(p), (0.0, 0.0))
        self.assertEqual(ref.directional_green(p, 0), 143 / 1024)
        self.assertEqual(ref.directional_green(p, 1), 144 / 1024)
        self.assertEqual(ref.green(p), 143 / 1024)
        self.assertEqual(m.choose_direction(2, 2), 0)
        self.assertEqual(m.choose_direction(2, 1), 1)
        self.assertEqual(m.choose_direction(1, 2), 0)
        for refine in (False, True):
            self.assertEqual(ref.pixel(*p, refine=refine), (144 / 1024, 143 / 1024, 144 / 1024))

    def test_classifier_matches_independent_exact_rational_edge_sums(self):
        def sample(x, y):
            return F((17 * x + 11 * y + x * y) % 32, 32)
        ref = m.BayerReference(*sensor(lambda x, y: (float(sample(x, y)),) * 3, size=(25, 25)))
        p = (12, 12)
        def chroma(x, y, ex, ey):
            center = sample(x, y)
            green = (sample(x - ex, y - ey) + sample(x + ex, y + ey)) / 2
            green += (2 * center - sample(x - 2 * ex, y - 2 * ey) - sample(x + 2 * ex, y + 2 * ey)) / 4
            return center - green
        expected = []
        for ex, ey in ((1, 0), (0, 1)):
            total = F(0)
            for transverse in range(-2, 3):
                starts = (-2, 0) if transverse % 2 == 0 else (-1,)
                for along in starts:
                    x = p[0] + ex * along + ey * transverse
                    y = p[1] + ey * along + ex * transverse
                    total += (3 if transverse == 0 else 1) * abs(chroma(x, y, ex, ey) - chroma(x + 2 * ex, y + 2 * ey, ex, ey))
            expected.append(float(total))
        self.assertNotEqual(expected[0], expected[1])
        self.assertEqual(ref.classifiers(p), tuple(expected))
        self.assertEqual(ref.direction(p), 1 if expected[1] < expected[0] else 0)

    def test_exact_signed_chromatic_flats_and_affine_interiors_all_metadata(self):
        fields = (lambda x, y: (-.125, .5, 1.25),
                  lambda x, y: (-.125 + x / 128 + y / 256, .25 + x / 256 - y / 128, 1.25 - x / 128 + y / 256))
        for rgb in fields:
            for pattern in range(4):
                for phase in ((0, 0), (1, 0), (0, 1), (1, 1)):
                    for origin in ((0, 0), (1, 3)):
                        ref = m.BayerReference(*sensor(rgb, pattern=pattern, phase=phase, origin=origin))
                        for refine in (False, True):
                            image = ref.render(refine=refine)
                            for y in range(1, 22):
                                for x in range(1, 24):
                                    i = (y * 25 + x) * 3
                                    self.assertEqual(image.pixels[i:i + 3], rgb(x, y))

    def test_internal_green_is_not_final_border_fallback(self):
        ref = m.BayerReference(*sensor(lambda x, y: (x * x / 1024,) * 3, size=(25, 25)))
        ref.pixel(8, 8)
        self.assertIn(("green", (4, 8)), ref._cache)
        self.assertEqual(ref.green((4, 8)), 15 / 1024)
        self.assertNotEqual(ref.green((4, 8)), ref.pixel(4, 8)[1])

    def test_every_observed_channel_is_exact_in_both_variants(self):
        for pattern in range(4):
            ref = m.BayerReference(*sensor(lambda x, y: ((x % 7) / 8, -.125 + (y % 5) / 8, 1.25), pattern=pattern, phase=(1, 1)))
            for refine in (False, True):
                image = ref.render(refine=refine)
                for p, c in ref.colors.items():
                    i = (p[1] * ref.width + p[0]) * 3 + c
                    self.assertEqual(image.pixels[i], ref.observed[p])

    def test_thin_and_minimum_extents_and_bilinear_borders(self):
        for size in ((1, 1), (1, 19), (19, 1), (12, 19), (13, 13), (16, 19), (17, 17), (25, 23)):
            args = sensor(lambda x, y: ((x % 7) / 8, (y % 5) / 8, -.125), size=size, origin=(1, 3))
            ref, bilinear = m.BayerReference(*args), ha.BayerReference(*args)
            ox, oy, w, height = ref.bounds
            for refine, radius in ((False, 6), (True, 8)):
                image = ref.render(refine=refine)
                for y in range(oy, oy + height):
                    for x in range(ox, ox + w):
                        if ref.margin((x, y)) < radius:
                            i = ((y - oy) * w + x - ox) * 3
                            self.assertEqual(image.pixels[i:i + 3], tuple(bilinear.bilinear(x, y, c) for c in range(3)))
        ref = m.BayerReference(*sensor(lambda x, y: (.25, .5, .75), size=(1, 1)))
        self.assertEqual(ref.render().pixels, (.25, 0, 0))

    def test_roi_crop_partition_and_stage_evaluation_order(self):
        args = sensor(lambda x, y: ((x % 7) / 8, (y % 5) / 8, -.125), origin=(1, 3))
        for refine in (False, True):
            ref = m.BayerReference(*args)
            first = ref.render((7, 9, 8, 7), refine=refine)
            whole = ref.render(refine=refine)
            separate = m.BayerReference(*args)
            self.assertEqual(separate.render(refine=refine), whole)
            self.assertEqual(separate.render((7, 9, 8, 7), refine=refine), first)
            expected = tuple(v for y in range(6, 13) for x in range(6, 14)
                             for v in whole.pixels[(y * 25 + x) * 3:(y * 25 + x) * 3 + 3])
            self.assertEqual(first.pixels, expected)
            assembled = tuple(v for y in range(3, 26) for x in range(1, 26)
                              for v in ref.pixel(x, y, refine=refine))
            self.assertEqual(assembled, whole.pixels)
            self.assertEqual(ref.render((1, 3, 0, 0), refine=refine).pixels, ())

    def test_owned_sensor_and_metadata_ignore_hostile_inactive_padding(self):
        args = sensor(lambda x, y: ((x % 7) / 8, (y % 5) / 8, -.125), origin=(1, 3))
        ref = m.BayerReference(*args)
        expected = [ref.render(refine=v) for v in (False, True)]
        mutated = copy.deepcopy(args)
        ox, oy, w, height = ref.bounds
        for y in range(mutated[2]):
            for x in range(mutated[3]["row_stride_samples"]):
                if not (ox <= x < ox + w and oy <= y < oy + height):
                    mutated[0][y * mutated[3]["row_stride_samples"] + x] = 0
        fresh = m.BayerReference(*mutated)
        self.assertEqual([fresh.render(refine=v) for v in (False, True)], expected)
        args[0][:] = [0] * len(args[0])
        args[3]["black_levels"][0] = 999
        self.assertEqual([ref.render(refine=v) for v in (False, True)], expected)
        with self.assertRaises(TypeError):
            ref.observed[1, 3] = 1

    def test_invalid_metadata_roi_stage_controls_and_budget(self):
        args = sensor(lambda x, y: (.25, .5, .75))
        for patch in ({"pattern": 4}, {"cfa_phase_x": True}, {"row_stride_samples": 1}, {"active_width": 0},
                      {"white_levels": [256, 1536, 1792, 2048]}, {"unknown": 1}):
            data, w, height, meta = copy.deepcopy(args)
            meta.update(patch)
            with self.assertRaises(ValueError):
                m.BayerReference(data, w, height, meta)
        with self.assertRaises(ValueError):
            m.BayerReference(args[0][:-1], *args[1:])
        bad = list(args[0])
        bad[0] = 65536
        with self.assertRaises(ValueError):
            m.BayerReference(bad, *args[1:])
        meta = dict(args[3], row_stride_samples=m.MAX_SAMPLES + 1, active_width=1, active_height=1)
        with self.assertRaisesRegex(ValueError, "budget"):
            m.BayerReference([], 1, 1, meta)
        ref = m.BayerReference(*args)
        for roi in ((-1, 0, 1, 1), (0, 0, True, 1), (25, 0, 1, 1), (0, 0, 1), (0, 0, 1.5, 1)):
            with self.assertRaises(ValueError):
                ref.render(roi)
        for fn in (lambda: ref.render(refine=1), lambda: ref.pixel(-1, 0),
                   lambda: ref.pixel(True, 0), lambda: ref.green((0, 0)),
                   lambda: ref.directional_green((12, 12), True)):
            with self.assertRaises(ValueError):
                fn()

    def test_quick_research_pairing_and_corrupted_fixture_refusal(self):
        baseline = h.read_baseline(Path(__file__).parent / "reference/raw/bilinear_baseline_v3.json.gz")
        before = h.report_json(baseline["cases"][0])
        report = m.evaluate(baseline, quick=True)
        self.assertEqual(report["paired_case_count"], 148)
        self.assertEqual(report["metadata_sweep"], "quick")
        self.assertFalse(report["native_replacement_accepted"])
        self.assertNotIn("report_schema_version", report)
        for name, v in report["variants"].items():
            self.assertEqual(v["case_count"], 148)
            self.assertEqual(len(v["improvements"]), 24)
            self.assertEqual(v["research_identity"]["refinement"], name == "refined")
            self.assertTrue(all(c["measured_correctness"]["observed_fidelity"] and c["measured_correctness"]["quantization"] for c in v["cases"]))
            self.assertFalse(v["native_replacement_accepted"])
        self.assertEqual(h.report_json(baseline["cases"][0]), before)
        baseline["cases"][0]["fixture"]["bayer"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "differs from preserved"):
            m.evaluate(baseline, quick=True)
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "report.json"
            self.assertEqual(m.write_report(target, report), m.write_report(target, report))
            target.write_bytes(b"unrelated evidence\n")
            with self.assertRaisesRegex(ValueError, "overwrite"):
                m.write_report(target, report)
            self.assertEqual(target.read_bytes(), b"unrelated evidence\n")
            with self.assertRaisesRegex(ValueError, "2368"):
                m.preserve(Path(tmp) / "partial.json.gz", report)

    def test_preserved_full_study_integrity_provenance_and_idempotence(self):
        directory = Path(__file__).parent / "reference/raw"
        packed = (directory / "menon_scalar_research_v1.json.gz").read_bytes()
        index = json.loads((directory / "menon_scalar_research_v1.index.json").read_text(encoding="utf-8"))
        payload = gzip.decompress(packed)
        record = json.loads(payload)
        self.assertEqual(index["compressed_sha256"], h.digest(packed))
        self.assertEqual(index["report_json_sha256"], h.digest(payload))
        self.assertEqual(record["reference_source_sha256_lf"], m.source_digest(m))
        self.assertEqual(record["policy_sha256"], h.digest(m.comparison.POLICY_PATH.read_bytes()))
        self.assertEqual(record["policy"], json.loads(m.comparison.POLICY_PATH.read_text(encoding="utf-8")))
        self.assertEqual(record["paired_case_count"], 2368)
        self.assertEqual(record["metadata_sweep"], "full")
        for name, variant in record["variants"].items():
            self.assertEqual(variant["case_count"], 2368)
            self.assertEqual(len(variant["cases"]), 2368)
            self.assertEqual(len(variant["improvements"]), 24)
            self.assertTrue(all(v["pair_count"] == 64 for v in variant["improvements"]))
            self.assertEqual(index["variant_summary"][name]["correctness_failure_count"], len(variant["correctness_failures"]))
            self.assertFalse(variant["native_replacement_accepted"])
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "menon_scalar_research_v1.json.gz"
            local_index = m.preserve(target, record)
            local_packed = target.read_bytes()
            # Python 3.12/3.13 gzip OS header bytes can differ. Verify the exact
            # JSON and provenance across runtimes, then byte idempotence within
            # the current runtime; never rewrite the original archive.
            self.assertEqual(gzip.decompress(local_packed), payload)
            self.assertEqual({k: v for k, v in local_index.items() if k != "compressed_sha256"},
                             {k: v for k, v in index.items() if k != "compressed_sha256"})
            self.assertEqual(m.preserve(target, record), local_index)
            self.assertEqual(target.read_bytes(), local_packed)
            bad_index = target.with_suffix("").with_suffix(".index.json")
            bad_index.write_bytes(b"unrelated index\n")
            with self.assertRaisesRegex(ValueError, "replace"):
                m.preserve(target, record)
            self.assertEqual(target.read_bytes(), local_packed)
            self.assertEqual(bad_index.read_bytes(), b"unrelated index\n")

    def test_preserved_external_crosscheck_records_distinct_precision_contracts(self):
        path = Path(__file__).parent / "reference/raw/menon_upstream_check_v1.json"
        record = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(record["reference_source_sha256_lf"], m.source_digest(m))
        self.assertEqual(record["case_count"], 32)
        self.assertTrue(record["passed"])
        self.assertFalse(record["native_replacement_accepted"])
        for case in record["cases"]:
            self.assertEqual(case["variants"]["base"]["max_abs"], 0)
            for row in case["variants"].values():
                self.assertTrue(row["passed"])
                self.assertLessEqual(row["max_abs"], row["rounding_bound"])


if __name__ == "__main__":
    unittest.main()
