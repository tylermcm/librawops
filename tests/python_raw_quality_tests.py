"""Source-only RawSession integration of the independent quality harness."""

import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_quality_harness as h


class RawQualityTests(unittest.TestCase):
    def test_impulse_core_surround_and_lost_unobserved_channel(self):
        for probe in h.IMPULSE_PROBES:
            record = h.render_case(raw, h.generate(probe))
            self.assertTrue(record["acceptance"]["passed"])
            self.assertEqual(record["metrics"]["impulse_center"]["pixel_count"], 1)
            self.assertEqual(record["metrics"]["impulse_neighborhood"]["pixel_count"], 25)
            self.assertEqual(record["metrics"]["impulse_surround"]["pixel_count"], 24)
            self.assertEqual(record["measurement_scopes"]["impulse_center"], (32, 24, 1, 1))
            self.assertEqual(record["metrics"]["quantization"]["observed"]["max_abs"], 0)
            self.assertEqual(record["acceptance"]["quality_role"], "baseline characterization")
            self.assertIsNone(record["acceptance"]["reconstruction_max_limit"])
            # RGGB/(0,0) observes red at (32,24). Green/blue impulses are
            # invisible to Bayer sampling, so their native reconstruction is
            # the background everywhere and their center channel error is 1.
            if probe.name in ("green_impulse", "blue_impulse"):
                self.assertEqual(record["metrics"]["impulse_center"]["rgb"]["max_abs"], 1)
                self.assertEqual(record["metrics"]["impulse_surround"]["rgb"]["max_abs"], 0)
            else:
                self.assertGreater(record["metrics"]["impulse_surround"]["rgb"]["max_abs"], 0)
            if probe.neutral:
                self.assertGreater(record["metrics"]["impulse_center"]["residual_chroma"]["rmse"], 0)
        shifted = h.render_case(raw, h.generate(h.IMPULSE_PROBES[0], layout="shifted"))
        self.assertEqual(shifted["measurement_scopes"]["impulse_center"], (33, 27, 1, 1))

    def test_detail_and_aliasing_are_characterized_with_common_correctness_gates(self):
        for probe in h.DETAIL_PROBES + h.ALIASING_PROBES:
            with self.subTest(probe=probe.name):
                record = h.render_case(raw, h.generate(probe, layout="shifted", pattern=2,
                                                      phase=(0, 1), levels="per-site"))
                self.assertTrue(record["acceptance"]["passed"], record["acceptance"])
                self.assertIsNone(record["acceptance"]["gates"]["analytical_reconstruction"])
                self.assertGreater(record["metrics"]["whole"]["rgb"]["rmse"], 0)
                if probe.neutral:
                    self.assertGreater(record["metrics"]["whole"]["residual_chroma"]["rmse"], 0)
                else:
                    self.assertIsNone(record["metrics"]["whole"]["residual_chroma"])
    def test_edge_bands_and_characterization_policy(self):
        for probe in h.EDGE_PROBES:
            with self.subTest(probe=probe.name):
                fixture = h.generate(probe, layout="shifted", pattern=3, phase=(1, 0), levels="per-site")
                record = h.render_case(raw, fixture)
                self.assertTrue(record["acceptance"]["passed"], record["acceptance"])
                self.assertEqual(record["acceptance"]["quality_role"], "baseline characterization")
                self.assertIsNone(record["acceptance"]["analytical_scope"])
                self.assertIsNone(record["acceptance"]["reconstruction_max_limit"])
                self.assertIsNone(record["acceptance"]["gates"]["analytical_reconstruction"])
                scope = record["measurement_scopes"]["edge_band"]
                self.assertEqual(scope["half_width"], 2)
                self.assertTrue(scope["inclusive"])
                self.assertGreater(scope["pixel_count"], 0)
                self.assertLess(scope["pixel_count"], 65 * 49)
                self.assertEqual(record["metrics"]["edge_band"]["pixel_count"], scope["pixel_count"])
                self.assertEqual(record["metrics"]["edge_band"]["rgb"]["sample_count"], scope["pixel_count"] * 3)
                if probe.neutral:
                    self.assertGreater(record["metrics"]["edge_band"]["residual_chroma"]["rmse"], 0)
                else:
                    self.assertIsNone(record["metrics"]["edge_band"]["residual_chroma"])
                if probe.transition_width == 0:
                    self.assertGreater(record["metrics"]["edge_band"]["rgb"]["max_abs"], 0.1)

    def test_edge_records_reproduce_with_fixed_scope_and_parameters(self):
        fixture = h.generate(h.EDGE_PROBES[1], layout="shifted", pattern=1, phase=(0, 1), levels="per-site")
        first = h.render_case(raw, fixture)
        second = h.render_case(raw, fixture)
        self.assertEqual(h.report_json(first), h.report_json(second))
        self.assertEqual(first["fixture"]["parameters"]["transition_width"], 0)
        self.assertEqual(first["fixture"]["parameters"]["slope"], 0.25)
        self.assertEqual(first["fixture"]["parameters"]["offset"], 26.375)

    def test_shifted_affine_all_patterns_phases_and_gates(self):
        for pattern in range(4):
            for phase in ((0, 0), (1, 0), (0, 1), (1, 1)):
                with self.subTest(pattern=pattern, phase=phase):
                    fixture = h.generate(h.PROBES[-1], layout="shifted", pattern=pattern,
                                         phase=phase, levels="per-site")
                    record = h.render_case(raw, fixture)
                    self.assertTrue(record["acceptance"]["passed"], record["acceptance"])
                    self.assertEqual(record["saved_manifest"]["format_version"], 3)
                    self.assertEqual(record["saved_manifest"]["operations"], [])
                    self.assertEqual(record["saved_manifest"]["output"], record["source_info"]["id"])
                    self.assertEqual(record["metrics"]["whole"]["pixel_count"], 65 * 49)
                    self.assertEqual(record["metrics"]["interior"]["pixel_count"], 63 * 47)
                    self.assertEqual(record["measurement_scopes"]["interior_margin"], 1)
                    self.assertGreater(record["metrics"]["whole"]["rgb"]["max_abs"],
                                       record["acceptance"]["reconstruction_max_limit"])
                    self.assertEqual(record["source_info"]["active_area"], (1, 3, 65, 49))
                    self.assertNotIn("working_space", record["saved_manifest"]["sources"][0])

    def test_neutral_flats_preserve_signed_headroom_and_reproducible_record(self):
        for index in (0, 3):
            fixture = h.generate(h.PROBES[index])
            record = h.render_case(raw, fixture)
            self.assertTrue(record["acceptance"]["passed"], record["acceptance"])
            self.assertEqual(record["acceptance"]["analytical_scope"], "whole")
            self.assertEqual(record["metrics"]["whole"]["residual_chroma"]["rmse"], 0)
            self.assertEqual(h.report_json(record), h.report_json(h.render_case(raw, fixture)))
            self.assertEqual(json.loads(h.report_json(record))["fixture"]["seed"], 0)
            session = raw.RawSession(fixture.native_samples(), 65, 49, fixture.metadata)
            try:
                result = session.render_manifest(json.dumps(record["saved_manifest"]))
                pixels = h.decode_render(result, fixture.truth.bounds).pixels
                if index == 0:
                    self.assertLess(max(pixels), 0)
                else:
                    self.assertGreater(min(pixels), 1)
            finally:
                session.close()

    def test_corrupt_render_is_reported_as_a_failed_gate(self):
        import array
        from types import SimpleNamespace

        class CorruptSession:
            def __init__(self, *args, **kwargs):
                self.session = raw.RawSession(*args, **kwargs)

            def export_manifest(self):
                return self.session.export_manifest()

            def source_info(self):
                return self.session.source_info()

            def close(self):
                self.session.close()

            def render_manifest(self, *args):
                width, height, payload = self.session.render_manifest(*args)
                pixels = array.array("f")
                pixels.frombytes(payload)
                pixels[0] += 0.1
                return width, height, pixels.tobytes()

        record = h.render_case(SimpleNamespace(RawSession=CorruptSession), h.generate(h.PROBES[1]))
        self.assertFalse(record["acceptance"]["passed"])
        self.assertFalse(record["acceptance"]["gates"]["analytical_reconstruction"])
        self.assertFalse(record["acceptance"]["gates"]["observed_sample_fidelity"])
        self.assertTrue(record["acceptance"]["gates"]["quantization_bound"])
        self.assertGreater(record["metrics"]["whole"]["rgb"]["max_abs"], 0.09)


if __name__ == "__main__":
    unittest.main()
