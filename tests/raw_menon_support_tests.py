"""Independent geometry checks for the proposed Menon support/border contract."""
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_menon_support as m


class MenonSupportTests(unittest.TestCase):
    def test_classifier_edges_are_balanced_not_shifted_correlation(self):
        for axis, edges in zip(m.AXES, (m.H_EDGES, m.V_EDGES)):
            endpoints = []
            self.assertEqual(len(edges), 8)
            self.assertEqual(sum(w for _, _, w in edges), 12)
            for x, y, w in edges:
                a, b = (x, y), (x + 2 * axis[0], y + 2 * axis[1])
                self.assertEqual((x + y) % 2, 0)
                self.assertEqual(w, 3 if (y if axis[0] else x) == 0 else 1)
                self.assertTrue(all(-2 <= v <= 2 for p in (a, b) for v in p))
                endpoints.extend((a, b))
            self.assertEqual(sum(x for x, _ in endpoints), 0)
            self.assertEqual(sum(y for _, y in endpoints), 0)
        self.assertIn((-2, 0, 3), m.H_EDGES)
        self.assertNotIn((2, 0, 3), m.H_EDGES)

    def test_hand_traced_outer_dependency_chain(self):
        s = m.Support("RGGB")
        # Red target: decision uses CH(-2, 0), whose green estimate reads (-4, 0).
        p = (0, 0)
        self.assertIn((-4, 0), s.decision(p))
        # Green x=1 -> red x=2 decision -> green predictor's raw x=6.
        self.assertIn((6, 0), s.color_at_green((1, 0), 0))
        # Blue target x=1,y=1 -> green x=2,y=1 -> blue neighbor x=3,y=1 -> raw x=7.
        self.assertIn((7, 1), s.base((1, 1), 0))
        # Refinement adds two further one-step accesses to that stage.
        self.assertIn((9, 1), s.refined((1, 1), 0))
        self.assertFalse(any(abs(x - 1) > 8 or abs(y - 1) > 8 for x, y in s.refined((1, 1), 0)))

    def test_each_stage_radius_all_patterns_phases_and_parities(self):
        for pattern, px, py in itertools.product(m.PATTERNS, (0, 1), (0, 1)):
            s = m.Support(pattern, (px, py))
            for p in itertools.product((0, 1), repeat=2):
                own = s.channel(p)
                if own == 1:
                    for c in (0, 2):
                        self.assertEqual(m.radius(s.color_at_green(p, c), p), 5)
                        self.assertEqual(m.radius(s.after_green_locations(p, c), p), 7)
                else:
                    other = 2 if own == 0 else 0
                    for axis in m.AXES:
                        self.assertEqual(m.radius(s.directional_green(p, axis), p), 2)
                    self.assertEqual(m.radius(s.decision(p), p), 4)
                    self.assertEqual(m.radius(s.green(p), p), 4)
                    self.assertEqual(m.radius(s.base(p, other), p), 6)
                    self.assertEqual(m.radius(s.refined_green(p), p), 6)
                    self.assertEqual(m.radius(s.refined(p, other), p), 8)

    def test_observed_channels_are_copied_in_every_stage(self):
        for pattern, px, py in itertools.product(m.PATTERNS, (0, 1), (0, 1)):
            s = m.Support(pattern, (px, py))
            for p in itertools.product((0, 1), repeat=2):
                c = s.channel(p)
                for stage in (s.base, s.after_green_locations, s.refined):
                    self.assertEqual(stage(p, c), {p})
                for refine in (False, True):
                    self.assertEqual(s.output(m.Bounds(0, 0, 17, 17), p, c, refine), {p})

    def test_green_axes_follow_sensor_phase_and_translation(self):
        for pattern, px, py in itertools.product(m.PATTERNS, (0, 1), (0, 1)):
            s = m.Support(pattern, (px, py))
            for p in itertools.product((0, 1), repeat=2):
                if s.channel(p) == 1:
                    for c in (0, 2):
                        e = s.axis_for_color(p, c)
                        self.assertEqual(s.channel(m.shifted(p, e, -1)), c)
                        self.assertEqual(s.channel(m.shifted(p, e, 1)), c)
                translated = (p[0] + 3, p[1] + 5)
                t = m.Support(pattern, (px ^ 1, py ^ 1))
                for c in range(3):
                    shifted = {(x + 3, y + 5) for x, y in s.refined(p, c)}
                    self.assertEqual(t.refined(translated, c), shifted)

    def test_thin_images_fallback_clipped_matching_color_only(self):
        for shape, origin, pattern in itertools.product(
                ((1, 1), (1, 19), (19, 1), (12, 19), (16, 19)), ((0, 0), (3, 5)), m.PATTERNS):
            b = m.Bounds(*origin, *shape)
            s = m.Support(pattern)
            for p in b.points():
                self.assertTrue(s.uses_fallback(b, p, True))
                for c in range(3):
                    leaves = s.output(b, p, c)
                    self.assertTrue(all(b.contains(q) and s.channel(q) == c for q in leaves))
                    self.assertLessEqual(m.radius(leaves, p), 1)
        s = m.Support()
        b = m.Bounds(0, 0, 1, 1)
        self.assertEqual(s.output(b, (0, 0), 1), set())
        self.assertEqual(s.output(b, (0, 0), 2), set())

    def test_minimum_extent_and_internal_stages_do_not_use_final_fallback(self):
        s = m.Support()
        for refine, h in ((False, 6), (True, 8)):
            b = m.Bounds(3, 5, 2 * h + 1, 2 * h + 1)
            center = (3 + h, 5 + h)
            self.assertFalse(s.uses_fallback(b, center, refine))
            self.assertTrue(s.uses_fallback(b, (center[0] - 1, center[1]), refine))
            for c in range(3):
                leaves = s.output(b, center, c, refine)
                self.assertTrue(all(b.contains(q) for q in leaves))
        b = m.Bounds(0, 0, 17, 17)
        # Internal green x=4,y=8 is required by interior output even though its
        # own final output falls back. The stage still needs the radius-4 decision.
        self.assertTrue(s.uses_fallback(b, (4, 8), True))
        self.assertIn((0, 8), s.green((4, 8)))
        self.assertNotIn((0, 8), s.output(b, (4, 8), 1, True))

    def test_roi_order_partition_and_expanded_rectangle_containment(self):
        b = m.Bounds(3, 5, 23, 21)
        s = m.Support("GBRG", (1, 0))
        roi = [(x, y) for y in range(11, 20) for x in range(9, 20)]
        for refine, h in ((False, 6), (True, 8)):
            whole = {p: tuple(s.output(b, p, c, refine) for c in range(3)) for p in roi}
            partitioned = {}
            for partition in (roi[::3], roi[1::3], roi[2::3]):
                for p in reversed(partition):
                    partitioned[p] = tuple(s.output(b, p, c, refine) for c in range(3))
            self.assertEqual(whole, partitioned)
            for p, channels in whole.items():
                self.assertTrue(all(b.contains(q) and max(abs(q[0] - p[0]), abs(q[1] - p[1])) <= h
                                    for leaves in channels for q in leaves))
            leaves = set().union(*(v for channels in whole.values() for v in channels))
            self.assertTrue(all(max(b.x, 9 - h) <= x < min(b.x + b.width, 20 + h)
                                and max(b.y, 11 - h) <= y < min(b.y + b.height, 20 + h)
                                for x, y in leaves))

    def test_invalid_metadata_points_controls_and_budget(self):
        for pattern, phase in (("RGBG", (0, 0)), ([], (0, 0)), ("RGGB", (True, 0)),
                               ("RGGB", (2, 0)), ("RGGB", [0, 0])):
            with self.assertRaises(ValueError):
                m.Support(pattern, phase)
        for args in ((-1, 0, 5, 5), (0, 0, 0, 1), (0, 0, 1, True), (0, 0, 65, 65)):
            with self.assertRaises(ValueError):
                m.Bounds(*args)
        s, b = m.Support(), m.Bounds(0, 0, 17, 17)
        for p, c, refine in (((17, 0), 0, True), ((0, 0), True, True), ((0, 0), 3, True),
                             ((0, 0), 0, 1), ((0.0, 0), 0, True)):
            with self.assertRaises(ValueError):
                s.output(b, p, c, refine)

    def test_report_provenance_counts_and_preserved_evidence(self):
        report = m.make_report()
        self.assertFalse(report["evaluates_pixels"])
        self.assertFalse(report["native_replacement_accepted"])
        self.assertEqual(report["cfa_phase_site_cases"], 64)
        self.assertEqual(report["active_layout_cases"], 256)
        self.assertGreater(report["channel_output_checks"], 100000)
        self.assertTrue(all(v > 0 for v in report["interior_point_variant_checks"].values()))
        evidence = Path(__file__).parent / "reference/raw/menon_support_contract_v1.json"
        self.assertEqual(json.loads(evidence.read_text(encoding="utf-8")), report)

    def test_cli_repeatability_and_overwrite_refusal(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "proof.json"
            cmd = [sys.executable, str(Path(m.__file__)), str(target)]
            first = subprocess.run(cmd, capture_output=True, check=True)
            payload = target.read_bytes()
            second = subprocess.run(cmd, capture_output=True, check=True)
            self.assertEqual(first.stdout, second.stdout)
            self.assertEqual(payload, target.read_bytes())
            target.write_bytes(b"different evidence\n")
            failed = subprocess.run(cmd, capture_output=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(target.read_bytes(), b"different evidence\n")


if __name__ == "__main__":
    unittest.main()
