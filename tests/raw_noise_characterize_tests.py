"""Original diagnostic controls; optional NumPy, no decoder/camera/module required."""
import copy
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import raw_noise_characterize as noise


class NoiseDiagnosticsTests(unittest.TestCase):
    def test_constant_and_affine_have_no_residual(self):
        y, x = np.mgrid[:64, :64]
        for p in (np.full((64, 64), .25), .25 + (x + 2 * y) / 2048):
            for radius in noise.RADII:
                np.testing.assert_array_equal(noise.residual(p, radius), np.zeros((48, 48)))
        metric = noise.plane_metrics(.25 + (x + 2 * y) / 2048)
        self.assertEqual(metric["first_difference_rms"], {"x": 1 / 2048, "y": 2 / 2048})
        self.assertIsNone(metric["residuals"]["4"]["correlation"]["x1"])

    def test_impulse_matches_explicit_neighborhood_and_energy(self):
        p = np.zeros((64, 64)); p[32, 32] = 1
        for r in noise.RADII:
            expected = np.empty((48, 48))
            for y in range(8, 56):
                for x in range(8, 56):
                    mean = math.fsum(float(p[yy, xx]) for yy in range(y-r, y+r+1) for xx in range(x-r, x+r+1)) / (2*r+1)**2
                    expected[y-8, x-8] = p[y, x] - mean
            np.testing.assert_allclose(noise.residual(p, r), expected, atol=1e-15, rtol=0)
            size = (2*r+1)**2
            self.assertAlmostEqual(noise.rms(expected), math.sqrt((size-1)/size) / 48, places=15)

    def test_box_sum_matches_independent_non_dyadic_oracle(self):
        p = np.random.default_rng(31415).normal(.3, .02, (32, 35))
        for r in noise.RADII:
            expected = np.array([[p[y, x] - math.fsum(float(p[yy, xx]) for yy in range(y-r, y+r+1) for xx in range(x-r, x+r+1)) / (2*r+1)**2
                for x in range(8, 27)] for y in range(8, 24)])
            np.testing.assert_allclose(noise.residual(p, r), expected, atol=2e-14, rtol=0)

    def test_uniform_and_opponent_perturbations_separate_proxies(self):
        p = np.random.default_rng(123).normal(0, .02, (64, 64))
        gray = noise.rgb_metrics(np.stack((p, p, p), axis=2))
        opponent = noise.rgb_metrics(np.stack((p, np.zeros_like(p), -p), axis=2))
        self.assertEqual(gray["joint_chroma_residual_rms"], {"1": 0.0, "4": 0.0})
        self.assertEqual(opponent["L_first_difference_joint_rms"], 0)
        self.assertGreater(opponent["joint_chroma_residual_rms"]["4"], .01)
        twice = noise.rgb_metrics(np.stack((2*p, np.zeros_like(p), -2*p), axis=2))
        for r in ("1", "4"):
            self.assertEqual(twice["joint_chroma_residual_rms"][r], 2*opponent["joint_chroma_residual_rms"][r])

    def test_slow_and_alternating_structure_have_distinct_scale_and_lag_signatures(self):
        _, x = np.mgrid[:128, :128]
        slow = noise.plane_metrics(np.sin(2*np.pi*x/16) / 32)
        alternating = noise.plane_metrics(np.where(x % 2, 1/32, -1/32))
        self.assertGreater(slow["residuals"]["4"]["rms"], 5*slow["residuals"]["1"]["rms"])
        self.assertGreater(slow["residuals"]["4"]["correlation"]["x1"], .9)
        self.assertAlmostEqual(alternating["residuals"]["4"]["correlation"]["x1"], -1)
        self.assertAlmostEqual(alternating["residuals"]["4"]["correlation"]["x2"], 1)

    def test_signed_headroom_and_fixed_centers_are_not_clipped_or_shifted(self):
        p = np.full((64, 64), .5); p[32, 32] = -1; p[33, 33] = 2
        metric = noise.plane_metrics(p)
        self.assertEqual(metric["count"], 48*48)
        self.assertEqual((metric["negative_count"], metric["above_one_count"], metric["min"], metric["max"]), (1, 1, -1, 2))
        self.assertEqual(metric["percentiles_1_50_99"], [.5, .5, .5])
        p[:4] = 100
        # Neither original centers nor radius-four support reaches this strip.
        self.assertEqual(noise.plane_metrics(p), metric)

    def test_correlation_degeneracy_and_zero_ratios_are_explicit(self):
        x = np.arange(64, dtype=float)
        self.assertIsNone(noise.correlation(np.ones(64), x))
        self.assertIsNone(noise.correlation(x*1e-15, x))
        self.assertAlmostEqual(noise.correlation(x, -x), -1)
        m = noise.rgb_metrics(np.full((64, 64, 3), .25))
        self.assertEqual(noise.ratios(m, m), {"joint_chroma_residual_rms": {"1": None, "4": None}, "L_first_difference_joint_rms": None})

    def test_site_mapping_keeps_phases_colors_and_site_normalization(self):
        codes = np.arange(80*88, dtype=np.uint16).reshape(80, 88)
        for pattern in range(4):
            for px in (0, 1):
                for py in (0, 1):
                    meta = {"pattern": pattern, "cfa_phase_x": px, "cfa_phase_y": py,
                            "black_levels": [1000, 1100, 1200, 1300], "white_levels": [12000, 13000, 14000, 15000]}
                    values, sites, colors = noise.normalized_sites(codes, meta, [3, 5, 64, 64])
                    rgb = np.zeros((64, 64, 3), dtype=np.float32)
                    for row in range(64):
                        for col in range(64):
                            site = ((row+5+py)%2)*2 + (col+3+px)%2
                            self.assertEqual(sites[row, col], site)
                            value = (np.float32(codes[row+5,col+3]) - np.float32(meta['black_levels'][site])) / np.float32(meta['white_levels'][site]-meta['black_levels'][site])
                            self.assertEqual(values[row,col], value)
                            rgb[row,col,noise.PATTERNS[pattern][site]] = value
                    self.assertEqual(noise.verify_observed((64,64,rgb.tobytes()), values, colors), 4096)
                    metrics = noise.site_metrics(values, sites, colors)
                    self.assertEqual(set(metrics), {'0','1','2','3'})
                    self.assertEqual([metrics[str(i)]['color'] for i in range(4)], [('R','G','B')[c] for c in noise.PATTERNS[pattern]])
                    rgb[0,0,colors[0,0]] += 1
                    with self.assertRaises(ValueError): noise.verify_observed((64,64,rgb.tobytes()), values, colors)

    def test_invalid_shapes_numbers_levels_and_phases_reject(self):
        for value in (np.ones(64), np.ones((24,64)), np.ones((1025,32)), np.full((64,64),np.nan), np.full((64,64),np.inf)):
            with self.assertRaises(ValueError): noise.plane_metrics(value)
        with self.assertRaises(ValueError): noise.rgb_metrics(np.ones((64,64,2)))
        with self.assertRaises(ValueError): noise.residual(np.ones((64,64)), 2)
        meta = {'pattern':0,'black_levels':[1]*4,'white_levels':[2]*4}
        for change in ({'pattern':True},{'cfa_phase_x':2},{'white_levels':[1]*4}):
            with self.assertRaises(ValueError): noise.normalized_sites(np.ones((64,64),dtype=np.uint16), {**meta,**change}, [0,0,64,64])

    def plan_records(self):
        plan = json.loads((ROOT/'tests/reference/raw/camera_noise_rois_v1.json').read_text(encoding='utf-8'))
        records = {r['id']:{'metadata':{'active_area':[0,0,8288,5520]}} for r in plan['captures']}
        return plan, records

    def test_frozen_plan_and_invalid_budget_roles_coordinates_reject(self):
        plan, records = self.plan_records(); noise.validate_plan(plan, records)
        changes = []
        def changed():
            p=copy.deepcopy(plan); changes.append(p); return p
        changed()['camera_noise_roi_plan_version']=True
        changed()['captures']=[]
        changed()['captures'].append(copy.deepcopy(plan['captures'][0]))
        changed()['captures'][0]['id']='../escape'
        changed()['captures'][0]['rois'][0]['role']='skin'
        changed()['captures'][0]['rois'][0]['description']=''
        for rect in ([True,640,256,256],[1201,640,256,256],[1200,640,255,256],[8200,640,256,256]):
            changed()['captures'][0]['rois'][0]['rect']=rect
        changed()['captures'][0]['rois'][0]['rect']=plan['captures'][0]['rois'][1]['rect']
        for p in changes:
            with self.assertRaises(ValueError): noise.validate_plan(p, records)
        with self.assertRaises(ValueError): noise.validate_plan(plan, {})

    def test_evidence_paths_are_confined_and_existing_output_rejects(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); folder=root/'camera'; folder.mkdir()
            with self.assertRaises(ValueError): noise.fresh_output(folder, root/'outside')
            self.assertFalse((root/'outside').exists())
            _, output=noise.fresh_output(folder, folder/'study')
            (output/'evidence').write_bytes(b'preserve')
            with self.assertRaises(ValueError): noise.fresh_output(folder, output)
            self.assertEqual((output/'evidence').read_bytes(), b'preserve')

    def test_residual_view_has_fixed_scale_and_excluded_gray_margin(self):
        rgb=np.full((64,64,3),.25); rgb[32,32,0] += .02
        image=noise.residual_view(rgb)
        np.testing.assert_array_equal(image[:8],np.full((8,64,3),128,dtype=np.uint8))
        self.assertEqual(int(image[32,32,1]),128)
        self.assertGreater(int(image[32,32,0]),240)
        np.testing.assert_array_equal(rgb[32,32],np.array([.27,.25,.25]))


if __name__ == '__main__': unittest.main()
