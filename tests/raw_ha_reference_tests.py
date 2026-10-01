"""Independent hand calculations and contracts for the scalar research oracle."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_ha_reference as ha
import raw_quality_harness as h


def sensor(rgb, *, size=(13, 11), origin=(0, 0), pattern=0, phase=(0, 0)):
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
    def test_hand_calculated_green_directions_and_symmetric_tie(self):
        horizontal = ha.green_direction(0.5, 0.25, 0.25, 0.125, 0.375)
        vertical = ha.green_direction(0.5, 0.375, 0.375, 0.125, 0.625)
        self.assertEqual(horizontal, (0.75, 0.375))
        self.assertEqual(vertical, (0.75, 0.4375))
        self.assertEqual(ha.select_direction(horizontal, vertical), 0.40625)
        self.assertEqual(ha.select_direction((0.5, 0.375), vertical), 0.375)
        self.assertEqual(ha.select_direction(horizontal, (0.5, 0.4375)), 0.4375)
        # Place those exact binary samples in an RGGB sensor, away from borders.
        data, width, height, meta = sensor(lambda x, y: (0, 0, 0), size=(13, 13))
        patch = {(6, 6): 0.5, (4, 6): 0.25, (8, 6): 0.25, (6, 4): 0.375, (6, 8): 0.375,
                 (5, 6): 0.125, (7, 6): 0.375, (6, 5): 0.125, (6, 7): 0.625}
        for (x, y), value in patch.items():
            site, _ = h.site_channel(meta, x, y)
            data[y * meta["row_stride_samples"] + x] = meta["black_levels"][site] + int(value * 1024)
        ref = ha.BayerReference(data, width, height, meta)
        self.assertEqual(ref.render((6, 6, 1, 1)).pixels[1], 0.40625)

    def test_hand_calculated_axial_diagonal_chroma_and_signed_values(self):
        # Green center .5, neighbors .125/.375, colors .25/.75:
        # green Laplacian .5; color gradient .5; predictor .5 + .25.
        self.assertEqual(ha.chroma_direction(0.5, 0.125, 0.375, 0.25, 0.75), (1.0, 0.75))
        self.assertEqual(ha.chroma_direction(0.5, 0.625, 0.375, 0.5, 0.5), (0.0, 0.5))
        self.assertEqual(ha.chroma_direction(-0.25, -0.5, -0.5, 1.0, 1.0), (0.5, 1.25))
        self.assertEqual(ha.select_direction((1, -0.5), (1, 1.5)), 0.5)

    def test_chromatic_signed_flat_all_cfa_phases_and_site_levels(self):
        flat = (-0.125, 0.5, 1.25)
        for pattern in range(4):
            for phase in ((0, 0), (1, 0), (0, 1), (1, 1)):
                args = sensor(lambda x, y: flat, origin=(1, 3), pattern=pattern, phase=phase)
                image = ha.BayerReference(*args).render()
                self.assertEqual(image.pixels, flat * (13 * 11))

    def test_exact_affine_interior_all_cfa_phases_and_sensor_origins(self):
        def rgb(x, y):
            return (-0.125 + x / 128 + y / 256, 0.25 + x / 256 - y / 128, 0.75 - x / 128 + y / 256)
        for pattern in range(4):
            for phase in ((0, 0), (1, 0), (0, 1), (1, 1)):
                for origin in ((0, 0), (1, 3)):
                    ref = ha.BayerReference(*sensor(rgb, origin=origin, pattern=pattern, phase=phase))
                    image = ref.render()
                    for y in range(1, 10):
                        for x in range(1, 12):
                            i = (y * 13 + x) * 3
                            self.assertEqual(image.pixels[i:i + 3], rgb(x, y))

    def test_observed_sites_are_exact_and_internal_green_is_not_output_fallback(self):
        args = sensor(lambda x, y: (1 if (x, y) == (2, 4) else 0, 0, 0))
        ref = ha.BayerReference(*args)
        # Radius-two internal green at a final-output border pixel participates
        # in neighboring interior chroma; it must not be replaced by bilinear.
        self.assertNotEqual(ref.green[2, 4], ref.render((2, 4, 1, 1)).pixels[1])
        image = ref.render()
        for y in range(11):
            for x in range(13):
                channel = ref.colors[x, y]
                self.assertEqual(image.pixels[(y * 13 + x) * 3 + channel], ref.observed[x, y])

    def test_thin_images_and_three_pixel_border_match_bilinear(self):
        for size in ((1, 1), (1, 9), (9, 1), (2, 7), (7, 2), (6, 9), (13, 11)):
            ref = ha.BayerReference(*sensor(lambda x, y: ((x % 3) / 2, (y % 2) / 2, -0.125), size=size, origin=(1, 3)))
            ox, oy, w, height = ref.bounds
            for y in range(oy, oy + height):
                for x in range(ox, ox + w):
                    if ref.margin(x, y) < 3:
                        self.assertEqual(ref.render((x, y, 1, 1)).pixels, tuple(ref.bilinear(x, y, c) for c in range(3)))

    def test_roi_partition_empty_and_render_order_independence(self):
        ref = ha.BayerReference(*sensor(lambda x, y: ((x % 3) / 2, (y % 2) / 2, -0.125), origin=(1, 3)))
        whole = ref.render()
        assembled = tuple(v for y in range(3, 14) for x in range(1, 14) for v in ref.render((x, y, 1, 1)).pixels)
        self.assertEqual(assembled, whole.pixels)
        ref.render((4, 6, 5, 4))
        self.assertEqual(ref.render(), whole)
        self.assertEqual(ref.render((1, 3, 0, 0)).pixels, ())
        roi = ref.render((4, 6, 5, 4))
        expected = tuple(v for y in range(3, 7) for x in range(3, 8) for v in whole.pixels[(y * 13 + x) * 3:(y * 13 + x) * 3 + 3])
        self.assertEqual(roi.pixels, expected)

    def test_ownership_and_padding_inactive_samples_are_ignored(self):
        args = sensor(lambda x, y: (0.25, 0.5, 0.75), origin=(1, 3))
        ref = ha.BayerReference(*args)
        expected = ref.render()
        mutated = copy.deepcopy(args)
        ox, oy, w, height = ref.bounds
        stride = mutated[3]["row_stride_samples"]
        for y in range(mutated[2]):
            for x in range(stride):
                if not (ox <= x < ox + w and oy <= y < oy + height):
                    mutated[0][y * stride + x] = 0
        self.assertEqual(ha.BayerReference(*mutated).render(), expected)
        args[0][:] = [0] * len(args[0])
        args[3]["black_levels"][0] = 999
        self.assertEqual(ref.render(), expected)

    def test_invalid_sensor_roi_and_budget_reject(self):
        args = sensor(lambda x, y: (0.25, 0.5, 0.75))
        for patch in ({"pattern": 4}, {"cfa_phase_x": True}, {"row_stride_samples": 1},
                      {"active_width": 0}, {"white_levels": [256, 1536, 1792, 2048]}, {"unknown": 1}):
            data, w, height, meta = copy.deepcopy(args)
            meta.update(patch)
            with self.assertRaises(ValueError): ha.BayerReference(data, w, height, meta)
        with self.assertRaises(ValueError): ha.BayerReference(args[0][:-1], *args[1:])
        bad = list(args[0]); bad[0] = 65536
        with self.assertRaises(ValueError): ha.BayerReference(bad, *args[1:])
        big_meta = dict(args[3], row_stride_samples=ha.MAX_SAMPLES + 1, active_width=1, active_height=1)
        with self.assertRaisesRegex(ValueError, "budget"): ha.BayerReference([], 1, 1, big_meta)
        ref = ha.BayerReference(*args)
        for roi in ((-1, 0, 1, 1), (0, 0, True, 1), (13, 0, 1, 1), (0, 0, 1), (0, 0, 1.5, 1)):
            with self.assertRaises(ValueError): ref.render(roi)

    def test_research_evaluation_pairs_evidence_without_native_acceptance(self):
        baseline = h.read_baseline(Path(__file__).parent / "reference/raw/bilinear_baseline_v3.json.gz")
        old = h.report_json(baseline["cases"][0])
        report = ha.evaluate(baseline, quick=True)
        self.assertEqual(report["case_count"], 148)
        self.assertEqual(report["metadata_sweep"], "quick")
        self.assertEqual(len(report["improvements"]), 24)
        self.assertTrue(all(row["pair_count"] == 4 for row in report["improvements"]))
        self.assertFalse(report["native_replacement_accepted"])
        self.assertNotIn("report_schema_version", report)
        self.assertEqual(h.report_json(baseline["cases"][0]), old)
        self.assertTrue(all(row["measured_correctness"]["observed_fidelity"] for row in report["cases"]))
        self.assertTrue(all(row["measured_correctness"]["quantization"] for row in report["cases"]))

    def test_per_site_quantization_counterexample_exceeds_frozen_flat_limit(self):
        probe = next(p for p in h.ALL_PROBES if p.name == "neutral_mid")
        fixture = h.generate(probe, levels="per-site")
        ref = ha.BayerReference(fixture.native_samples(), fixture.sensor_width, fixture.sensor_height, fixture.metadata)
        # At this observed green site, red is reconstructed as red-observation
        # plus the difference between observed green and estimated green. Two
        # differently quantized green sites make that correction amplify error.
        red = h.f32(ref.observed[30, 24] + ref.observed[30, 25] - ref.green[30, 24])
        self.assertEqual(ref.pixel(30, 25)[0], red)
        self.assertEqual(red, 0.3711903989315033)
        error = abs(red - fixture.truth.pixels[0])
        bound = max(0.5 / (w - b) for b, w in zip(fixture.metadata["black_levels"], fixture.metadata["white_levels"]))
        self.assertGreater(error, bound + 1e-6)
        self.assertEqual(ref.pixel(30, 25)[1], ref.observed[30, 25])

    def test_research_evaluation_rejects_changed_fixture_bytes(self):
        baseline = h.read_baseline(Path(__file__).parent / "reference/raw/bilinear_baseline_v3.json.gz")
        baseline["cases"][0]["fixture"]["bayer"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "differs from preserved evidence"):
            ha.evaluate(baseline, quick=True)


if __name__ == "__main__":
    unittest.main()
