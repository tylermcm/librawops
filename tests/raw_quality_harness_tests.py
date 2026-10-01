"""Scalar metrics and analytical fixtures, independent of the native renderer."""

from dataclasses import replace
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_quality_harness as h


def image(values, width=1, height=1, **kwargs):
    return h.RgbImage(width, height, tuple(values), **kwargs)


class MetricTests(unittest.TestCase):
    def test_exact_zero_and_empty(self):
        truth = image((-0.1, 0.0, 1.25))
        metrics = h.measure(truth, truth)
        self.assertEqual(metrics["pixel_count"], 1)
        self.assertEqual(metrics["rgb"], {"sample_count": 3, "max_abs": 0.0, "mae": 0.0, "rmse": 0.0})
        for channel in metrics["channels"].values():
            self.assertEqual(channel["sample_count"], 1)
            self.assertEqual(channel["rmse"], 0)
        self.assertIsNone(metrics["residual_chroma"])
        empty = h.measure(truth, truth, (0, 0, 0, 1))
        self.assertEqual(empty["pixel_count"], 0)
        self.assertEqual(empty["rgb"], {"sample_count": 0, "max_abs": None, "mae": None, "rmse": None})
        empty_image = image((), 0, 0)
        chroma = h.measure(empty_image, empty_image, neutral=True)["residual_chroma"]
        self.assertEqual(chroma, {"sample_count": 0, "max_abs": None, "rmse": None})

    def test_channel_isolated_and_rgb_denominator(self):
        metrics = h.measure(image((0, 0, 0, 3, 0, 0), 2), image((0,) * 6, 2))
        self.assertEqual(metrics["pixel_count"], 2)
        self.assertEqual(metrics["channels"]["R"]["mae"], 1.5)
        self.assertAlmostEqual(metrics["channels"]["R"]["rmse"], math.sqrt(4.5))
        self.assertEqual(metrics["channels"]["G"]["max_abs"], 0)
        self.assertEqual(metrics["rgb"]["sample_count"], 6)
        self.assertEqual(metrics["rgb"]["max_abs"], 3)
        self.assertEqual(metrics["rgb"]["mae"], 0.5)
        self.assertAlmostEqual(metrics["rgb"]["rmse"], math.sqrt(1.5))

    def test_signed_errors_and_neutral_chroma(self):
        metrics = h.measure(image((1, -2, 3)), image((0, 0, 0)), neutral=True)
        self.assertEqual(metrics["rgb"]["mae"], 2)
        self.assertAlmostEqual(metrics["rgb"]["rmse"], math.sqrt(14 / 3))
        self.assertEqual(metrics["residual_chroma"]["sample_count"], 2)
        self.assertEqual(metrics["residual_chroma"]["max_abs"], 5)
        self.assertAlmostEqual(metrics["residual_chroma"]["rmse"], math.sqrt(17))
        # Opposite errors must not cancel before MAE/RMSE accumulation.
        signed = h.measure(image((1, 1, 1, -1, -1, -1), 2), image((0,) * 6, 2), neutral=True)
        self.assertEqual(signed["rgb"]["mae"], 1)
        self.assertEqual(signed["rgb"]["rmse"], 1)
        self.assertEqual(signed["residual_chroma"]["rmse"], 0)

    def test_shifted_scope_and_float64_precision(self):
        truth = image((0,) * 12, 2, 2, origin=(1, 3))
        rendered = image((9, 9, 9, 9, 9, 9, 9, 9, 9, 1, 2, 3), 2, 2, origin=(1, 3))
        metrics = h.measure(rendered, truth, (2, 4, 1, 1))
        self.assertEqual(metrics["rgb"]["max_abs"], 3)
        self.assertEqual(metrics["rgb"]["mae"], 2)
        # Conversion to float32 before subtraction would erase this difference.
        precise = h.measure(image((1 + 2**-40, 1, 1)), image((1, 1, 1)))
        self.assertEqual(precise["channels"]["R"]["max_abs"], 2**-40)

    def test_invalid_inputs_fail_closed(self):
        truth = image((0,) * 6, 2)
        for invalid in (image((0,)), image((0,) * 6, 1, 2),
                        replace(truth, origin=(1, 0)), replace(truth, domain="srgb"),
                        replace(truth, width=True), replace(truth, origin=(0,))):
            with self.assertRaises(ValueError):
                h.measure(invalid, truth)
        for value in (math.nan, math.inf, -math.inf):
            bad = image((0, 0, 0, value, 0, 0), 2)
            with self.assertRaises(ValueError):
                h.measure(bad, truth, (0, 0, 1, 1))
            with self.assertRaises(ValueError):
                h.measure(truth, bad, (0, 0, 1, 1))
        with self.assertRaises(ValueError):
            h.measure(image((1e308, 0, 0)), image((-1e308, 0, 0)))
        for scope in ((0, 0, 3, 1), (-1, 0, 1, 1), (0, 0, -1, 1), (False, 0, 1, 1), (0, 0, 1)):
            with self.assertRaises(ValueError):
                h.measure(truth, truth, scope)
        with self.assertRaises(ValueError):
            h.measure(image((0, 0, 1)), image((0, 0, 1)), neutral=True)
        with self.assertRaises(ValueError):
            h.report_json({"value": math.nan})

    def test_native_render_shape_validation(self):
        import array
        payload = array.array("f", (-0.1, 0.0, 1.25)).tobytes()
        decoded = h.decode_render((1, 1, payload), (1, 3, 1, 1))
        self.assertEqual(decoded.bounds, (1, 3, 1, 1))
        self.assertGreater(decoded.pixels[-1], 1)
        for result in ((2, 1, payload), (1, 1, payload[:-1]),
                       (1, 1, array.array("f", (math.nan, 0, 0)).tobytes())):
            with self.assertRaises(ValueError):
                h.decode_render(result, (1, 3, 1, 1))

    def test_boolean_selection_denominators_and_empty_scope(self):
        truth = image((0,) * 42, 7, 2, origin=(1, 3))
        pixels = [0.0] * 42
        pixels[3:6] = (1, -2, 3)
        # Large errors outside the band must not contribute to its scores.
        pixels[0:3] = pixels[18:21] = (99, 99, 99)
        rendered = image(pixels, 7, 2, origin=(1, 3))
        probe = h.EdgeProbe("vertical-test", True, (0,) * 3, (1,) * 3, slope=0, offset=3)
        mask = h.edge_band_selection(truth, probe)
        self.assertEqual(mask, (False, True, True, True, True, True, False) * 2)
        measured = h.measure(rendered, truth, neutral=True, selection=mask)
        self.assertEqual(measured["pixel_count"], 10)
        self.assertEqual(measured["rgb"]["sample_count"], 30)
        self.assertEqual(measured["rgb"]["max_abs"], 3)
        self.assertEqual(measured["rgb"]["mae"], 0.2)
        self.assertAlmostEqual(measured["rgb"]["rmse"], math.sqrt(14 / 30))
        self.assertEqual(measured["residual_chroma"]["sample_count"], 20)
        self.assertAlmostEqual(measured["residual_chroma"]["rmse"], math.sqrt(34 / 20))
        intersection = h.measure(rendered, truth, (2, 3, 1, 1), selection=mask)
        self.assertEqual(intersection["rgb"]["mae"], 2)
        empty = h.measure(rendered, truth, neutral=True, selection=(False,) * 14)
        self.assertEqual(empty["pixel_count"], 0)
        self.assertIsNone(empty["rgb"]["rmse"])
        self.assertIsNone(empty["residual_chroma"]["rmse"])

    def test_selection_validation_checks_all_input_samples(self):
        truth = image((0,) * 6, 2)
        for mask in ((True,), (1, 0), (True, None), (True, False, True)):
            with self.assertRaisesRegex(ValueError, "selection"):
                h.measure(truth, truth, selection=mask)
        with self.assertRaisesRegex(ValueError, "nonfinite"):
            h.measure(image((0, 0, 0, math.inf, 0, 0), 2), truth, selection=(True, False))


class FixtureTests(unittest.TestCase):
    def test_zero_fixture_has_independent_canonical_bytes_and_padding(self):
        fixture = h.generate(h.PROBES[1])
        self.assertEqual(fixture.truth.pixels, (0.0,) * (65 * 49 * 3))
        self.assertEqual(fixture.bayer_le, struct.pack("<H", 4000) * (65 * 49))
        expected = hashlib.sha256(b"\x00" * (65 * 49 * 3 * 4)).hexdigest()
        self.assertEqual(h.digest(h.little_bytes("f", fixture.truth.pixels)), expected)
        shifted = h.generate(h.PROBES[1], layout="shifted", levels="per-site")
        self.assertEqual(shifted.truth.bounds, (1, 3, 65, 49))
        self.assertEqual(len(shifted.bayer_le), 72 * 55 * 2)
        samples = shifted.native_samples()
        self.assertEqual(samples[0], 65535)
        self.assertEqual(samples[3 * 72 + 69], 65535)
        self.assertEqual(samples[3 * 72 + 1], 8000)  # Original sensor odd/odd.
        self.assertEqual(samples[3 * 72 + 2], 3000)
        self.assertEqual(samples[4 * 72 + 1], 7000)
        self.assertEqual(samples[4 * 72 + 2], 4000)

    def test_all_cfa_sites_phases_use_original_sensor_coordinates(self):
        # Expected channels at sensor (1,3), independently written per pattern.
        expected = {0: (2, 1, 1, 0), 1: (0, 1, 1, 2), 2: (1, 2, 0, 1), 3: (1, 0, 2, 1)}
        probe = h.PROBES[4]
        for pattern, channels in expected.items():
            for phase, channel, site in zip(((0, 0), (1, 0), (0, 1), (1, 1)), channels, (3, 2, 1, 0)):
                fixture = h.generate(probe, layout="shifted", pattern=pattern, phase=phase, levels="per-site")
                black = (4000, 7000, 3000, 8000)[site]
                white = (19000, 23000, 16000, 27000)[site]
                value = struct.unpack("<f", struct.pack("<f", probe.constant[channel]))[0]
                code = int(math.floor(black + value * (white - black) + 0.5))
                self.assertEqual(fixture.native_samples()[3 * 72 + 1], code)

    def test_affine_values_determinism_and_signed_headroom(self):
        probe = h.PROBES[-1]
        fixture = h.generate(probe, layout="shifted", pattern=3, phase=(1, 0), levels="per-site", seed=17)
        repeated = h.generate(probe, layout="shifted", pattern=3, phase=(1, 0), levels="per-site", seed=17)
        self.assertEqual(fixture, repeated)
        for c in range(3):
            expected = struct.unpack("<f", struct.pack("<f", probe.constant[c] + 64 * probe.dx[c] + 48 * probe.dy[c]))[0]
            self.assertEqual(fixture.truth.pixels[-3 + c], expected)
        self.assertLess(min(fixture.truth.pixels), 0)
        self.assertGreater(max(fixture.truth.pixels), 1)
        with self.assertRaises(ValueError):
            h.encode_sample(10, 4000, 19000)
        with self.assertRaises(ValueError):
            h.encode_sample(-1, 4000, 19000)
        self.assertEqual(h.encode_sample(0.5, 0, 3), 2)  # Half-up, not ties-to-even.
        for options in ({"pattern": 4}, {"phase": (2, 0)}, {"phase": (False, 0)},
                        {"seed": -1}, {"levels": "invalid"}, {"layout": "invalid"}):
            with self.assertRaises(ValueError):
                h.generate(probe, **options)

    def test_quantization_denominators_and_observed_fidelity(self):
        fixture = h.generate(h.Probe("one-third", "flat", True, (1 / 3,) * 3), levels="per-site")
        measured = h.observed_metrics(fixture, fixture.truth)
        self.assertEqual(measured["quantization"]["observed"]["sample_count"], 65 * 49)
        counts = [v["sample_count"] for v in measured["quantization"]["channels"].values()]
        self.assertEqual(counts, [33 * 25, 32 * 25 + 33 * 24, 32 * 24])
        # At even/even R sites 15000 is divisible by three; float32 truth has
        # the independently known 1/3 rounding residual.
        self.assertAlmostEqual(measured["quantization"]["channels"]["R"]["max_abs"], abs(1 / 3 - struct.unpack("<f", struct.pack("<f", 1 / 3))[0]))
        self.assertEqual(measured["quantization"]["observed"], measured["observed_fidelity"]["observed"])
        with self.assertRaises(ValueError):
            h.observed_metrics(fixture, replace(fixture.truth, domain="srgb"))


class EdgeFixtureTests(unittest.TestCase):
    def test_signed_distance_and_four_pixel_transition(self):
        # Normal length is exactly 1.25 for slope 3/4. These hand-computed
        # points exercise -2, 0, +1 and +2 native-pixel distances.
        probe = h.EdgeProbe("analytic-test", False, (-0.1, 0.2, 1.2), (1.1, 0.8, 0.0),
                            slope=0.75, offset=0, transition_width=4)
        self.assertEqual(h.edge_distance(probe, 2, 6), -2)
        self.assertEqual(h.edge_distance(probe, 0, 0), 0)
        self.assertEqual(h.edge_distance(probe, 2, 1), 1)
        self.assertEqual(h.edge_distance(probe, 4, 2), 2)
        expected = lambda values: tuple(struct.unpack("<f", struct.pack("<f", v))[0] for v in values)
        self.assertEqual(h.probe_rgb(probe, 2, 6), expected((-0.1, 0.2, 1.2)))
        self.assertEqual(h.probe_rgb(probe, 0, 0), expected((0.5, 0.5, 0.6)))
        self.assertEqual(h.probe_rgb(probe, 2, 1), expected((0.8, 0.65, 0.3)))
        self.assertEqual(h.probe_rgb(probe, 4, 2), expected((1.1, 0.8, 0.0)))

    def test_hard_step_tie_rule_and_inclusive_band(self):
        probe = h.EdgeProbe("hard-test", True, (-0.07321,) * 3, (1.18321,) * 3,
                            slope=0, offset=3, transition_width=0)
        low = struct.unpack("<f", struct.pack("<f", -0.07321))[0]
        high = struct.unpack("<f", struct.pack("<f", 1.18321))[0]
        self.assertEqual(h.probe_rgb(probe, 2, 0), (low,) * 3)
        self.assertEqual(h.probe_rgb(probe, 3, 0), (high,) * 3)
        self.assertEqual(h.probe_rgb(probe, 4, 0), (high,) * 3)
        truth = image((0,) * 21, 7)
        self.assertEqual(h.edge_band_selection(truth, probe), (False, True, True, True, True, True, False))
        # An edge outside the image gives an empty scope, not a zero score.
        outside = replace(probe, offset=100)
        selected = h.edge_band_selection(truth, outside)
        self.assertEqual(h.measure(truth, truth, neutral=True, selection=selected)["rgb"]["max_abs"], None)

    def test_edge_generation_layout_independence_and_rejection(self):
        for probe in h.EDGE_PROBES:
            packed = h.generate(probe)
            shifted = h.generate(probe, layout="shifted", pattern=3, phase=(1, 1), levels="per-site")
            self.assertEqual(packed.truth.pixels, shifted.truth.pixels)
            self.assertEqual(h.edge_band_selection(packed.truth, probe), h.edge_band_selection(shifted.truth, probe))
            self.assertEqual(shifted, h.generate(probe, layout="shifted", pattern=3, phase=(1, 1), levels="per-site"))
            self.assertLess(min(packed.truth.pixels), 0)
            self.assertGreater(max(packed.truth.pixels), 1)
        probe = h.EDGE_PROBES[0]
        for invalid in (replace(probe, slope=math.inf), replace(probe, offset=math.nan),
                        replace(probe, slope=True), replace(probe, transition_width=-1),
                        replace(probe, transition_width=3), replace(probe, low=(0,)),
                        replace(probe, high=(2,) * 3)):
            with self.assertRaises(ValueError):
                h.generate(invalid)
        with self.assertRaises(ValueError):
            h.edge_band_selection(h.generate(h.PROBES[0]).truth, h.PROBES[0])


class DetailFixtureTests(unittest.TestCase):
    def test_impulses_have_one_native_peak_with_independent_values(self):
        for probe in h.IMPULSE_PROBES:
            fixture = h.generate(probe)
            center = (24 * 65 + 32) * 3
            for c in range(3):
                expected = 1.125 if probe.neutral or probe.name.startswith(("red", "green", "blue")[c]) else 0.125
                self.assertEqual(fixture.truth.pixels[center + c], expected)
            changed = [i // 3 for i in range(0, len(fixture.truth.pixels), 3)
                       if fixture.truth.pixels[i:i + 3] != (0.125,) * 3]
            self.assertEqual(changed, [24 * 65 + 32])
            shifted = h.generate(probe, layout="shifted", pattern=2, phase=(1, 0), levels="per-site")
            self.assertEqual(fixture.truth.pixels, shifted.truth.pixels)
        for position in ((0, 24), (64, 24), (32, 48), (True, 24), (32,)):
            with self.assertRaises(ValueError):
                h.generate(replace(h.IMPULSE_PROBES[0], position=position))

    def test_sine_quarter_cycles_channel_phase_and_period(self):
        # Known sin values at 0, 1/4, 1/2 and 3/4 cycle: 0, 1, 0, -1.
        # Binary means/amplitudes avoid f32 reference rounding ambiguity.
        for frequency, quarter in ((1 / 32, 8), (1 / 8, 2)):
            probe = h.SineProbe("phase-test", False, (frequency, 0), (0.5,) * 3,
                                (0.5,) * 3, (0.0, 0.25, 0.5))
            for x, expected in ((0, (0.5, 1.0, 0.5)), (quarter, (1.0, 0.5, 0.0)),
                                (2 * quarter, (0.5, 0.0, 0.5)), (3 * quarter, (0.0, 0.5, 1.0))):
                for actual, reference in zip(h.probe_rgb(probe, x, 17), expected):
                    self.assertAlmostEqual(actual, reference, delta=1e-7)
            self.assertEqual(h.probe_rgb(probe, 0, 0), h.probe_rgb(probe, 4 * quarter, 0))
            vertical = replace(probe, frequency=(0, frequency))
            self.assertEqual(h.probe_rgb(probe, quarter, 0), h.probe_rgb(vertical, 0, quarter))
        near_nyquist = h.SineProbe("near-test", True, (7 / 16, 0), (0.5,) * 3,
                                  (0.5,) * 3, (0.0,) * 3, "aliasing")
        self.assertEqual(h.probe_rgb(near_nyquist, 4, 0), (0.0,) * 3)  # 7/4 cycles.
        self.assertEqual(h.probe_rgb(near_nyquist, 12, 0), (1.0,) * 3)  # 21/4 cycles.

    def test_alternating_and_checkerboard_parity(self):
        for axis, expected in (("x", (0, 1, 0, 0)), ("y", (0, 0, 1, 0)),
                               ("checkerboard", (0, 1, 1, 0))):
            probe = h.AlternatingProbe("parity-test", True, (0,) * 3, (1,) * 3, axis)
            actual = [h.probe_rgb(probe, x, y)[0] for x, y in ((0, 0), (1, 0), (0, 1), (2, 2))]
            self.assertEqual(actual, list(expected))
        for probe in h.ALIASING_PROBES + h.DETAIL_PROBES:
            fixture = h.generate(probe)
            self.assertEqual(fixture.truth.pixels, h.generate(probe, layout="shifted").truth.pixels)
            self.assertLess(min(fixture.truth.pixels), 0)
            self.assertGreater(max(fixture.truth.pixels), 1)
            self.assertEqual(fixture, h.generate(probe))

    def test_detail_parameters_reject_nonfinite_and_invalid_controls(self):
        probe = h.DETAIL_PROBES[0]
        for invalid in (replace(probe, mean=(math.inf,) * 3), replace(probe, phase_cycles=(0,)),
                        replace(probe, frequency=(0, 0)), replace(probe, frequency=(0.6, 0)),
                        replace(probe, amplitude=(-0.1,) * 3), replace(probe, amplitude=(2,) * 3),
                        replace(probe, family="unknown"), replace(h.ALIASING_PROBES[0], axis="unknown"),
                        replace(h.IMPULSE_PROBES[0], peak=(math.nan,) * 3)):
            with self.assertRaises(ValueError):
                h.generate(invalid)


class BaselineEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = Path(__file__).resolve().parent / "reference/raw/bilinear_baseline_v3.json.gz"
        cls.record = h.read_baseline(cls.path)

    def test_preserved_complete_corpus_identity_and_readable_index(self):
        summary = self.record["summary"]
        self.assertEqual(summary["case_count"], 2368)
        self.assertEqual(summary["probe_count"], 37)
        self.assertEqual(summary["analytical_cases"], 448)
        self.assertEqual(summary["characterization_cases"], 1920)
        self.assertEqual(summary["family_cases"], {"flat": 320, "affine": 128, "slanted_edge": 512,
                                                 "impulse": 256, "smooth_detail": 512, "aliasing": 640})
        self.assertEqual(struct.unpack("<I", self.path.read_bytes()[4:8])[0], 0)  # gzip mtime.
        index = json.loads(h.baseline_index_path(self.path).read_text())
        self.assertEqual(index["summary"], summary)
        self.assertEqual(len(index["probe_maxima"]), 37)
        self.assertTrue(all(p["case_count"] == 64 for p in index["probe_maxima"]))
        self.assertEqual(index["compressed_sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_preservation_is_repeatable_and_refuses_different_existing_evidence(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / self.path.name
            first = h.preserve_baseline(path, self.record)
            before = (path.read_bytes(), h.baseline_index_path(path).read_bytes())
            self.assertEqual(h.preserve_baseline(path, self.record), first)
            self.assertEqual((path.read_bytes(), h.baseline_index_path(path).read_bytes()), before)
            self.assertEqual(h.read_baseline(path), self.record)
            changed = {**self.record, "build": {**self.record["build"], "description": "different build"}}
            with self.assertRaises(FileExistsError):
                h.preserve_baseline(path, changed)
            self.assertEqual((path.read_bytes(), h.baseline_index_path(path).read_bytes()), before)
            payload = bytearray(path.read_bytes()); payload[-1] ^= 1
            path.write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "compressed identity"):
                h.read_baseline(path)

    def test_incomplete_failed_or_changed_corpus_rejects(self):
        record = self.record
        cases = record["cases"]
        first = cases[0]
        failed = {**first, "acceptance": {**first["acceptance"],
                  "gates": {**first["acceptance"]["gates"], "roi_exact": False}}}
        empty_gates = {**first, "acceptance": {**first["acceptance"], "gates": {}}}
        changed = {**first, "fixture": {**first["fixture"], "parameters": {"unknown": 1}}}
        nonfinite = {**record, "build": {"bad_value": math.nan}}
        for bad in ({**record, "cases": cases[:-1]}, {**record, "cases": [first, *cases[:-1]]},
                    {**record, "cases": [failed, *cases[1:]]}, {**record, "cases": [empty_gates, *cases[1:]]},
                    {**record, "cases": [changed, *cases[1:]]}, {**record, "generator_version": 2},
                    {**record, "seed": 1}, {**record, "summary": {**record["summary"], "metadata_sweep": "quick"}},
                    nonfinite):
            with self.assertRaises(ValueError):
                h.validate_baseline(bad)


if __name__ == "__main__":
    unittest.main()
