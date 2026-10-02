"""Independent mathematics/truth/halo controls; optional NumPy, no native module."""
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
import raw_noise_control as nr


def oracle(value, radius, intensity, color):
    h, w = value.shape[:2]
    result = np.empty_like(value)
    for y in range(h):
        for x in range(w):
            def components(r, g, b): return ((r+2*g+b)/4, r-g, b-g)
            center = components(*(float(v) for v in value[y, x]))
            neighbors = [components(*(float(v) for v in value[yy, xx]))
                for yy in range(max(0, y-radius), min(h, y+radius+1))
                for xx in range(max(0, x-radius), min(w, x+radius+1))]
            transformed = [p+s*(math.fsum(n[c] for n in neighbors)/len(neighbors)-p)
                           for c, (p, s) in enumerate(zip(center, (intensity, color, color)))]
            l, cr, cb = transformed; g = l-(cr+cb)/4
            result[y, x] = [g+cr, g, g+cb]
    return result


class NoiseControlTests(unittest.TestCase):
    def test_bypass_preserves_exact_bytes_including_signed_zero_without_aliasing(self):
        a = np.array([[[-0., .2, 1.5], [-.1, 0., 2.]]], dtype=np.float32)
        before = a.tobytes(); b = nr.control(a)
        self.assertEqual(b.tobytes(), before)
        b[0, 0] = 1
        self.assertEqual(a.tobytes(), before)

    def test_explicit_neighborhood_oracle_corners_singletons_and_nondyadic_values(self):
        rng = np.random.Generator(np.random.PCG64(71))
        for shape in ((1, 1, 3), (1, 11, 3), (13, 1, 3), (11, 13, 3)):
            value = rng.uniform(-.2, 1.5, size=shape).astype(np.float32)
            for radius in (1, 4):
                for intensity, color in ((.5, 0), (0, .5), (.5, .5), (1, 1)):
                    np.testing.assert_allclose(nr.control(value, radius, intensity, color),
                                               oracle(value, radius, intensity, color), rtol=0, atol=1e-6)

    def test_impulse_coefficients_independently_predict_noise_variance(self):
        for radius in (1, 4):
            n = (2*radius+1)**2
            for strength in (.5, 1):
                p = np.zeros((33, 33, 3)); p[16, 16, 0] = 1
                value = nr.reconstruct(p).astype(np.float32)
                out = nr.proxies(nr.control(value, radius, strength, 0))[..., 0]
                expected = np.zeros((33, 33))
                expected[16-radius:17+radius, 16-radius:17+radius] = strength/n
                expected[16, 16] += 1-strength
                np.testing.assert_allclose(out, expected, atol=1e-7, rtol=0)
                expected_factor = (1-strength+strength/n)**2 + (n-1)*(strength/n)**2
                self.assertAlmostEqual(nr.variance_factor(radius, strength), expected_factor, places=14)
                self.assertAlmostEqual(float(np.sum(out*out)), expected_factor, places=7)

    def test_independent_proxy_controls_and_signed_headroom_constant(self):
        y, x = np.mgrid[:48, :48]
        p = np.stack((.2+.01*((x+y)%2), .02*((x//2)%2), -.02*((y//2)%2)), axis=-1)
        value = nr.reconstruct(p).astype(np.float32); original = nr.proxies(value)
        intensity_only = nr.proxies(nr.control(value, 1, .5, 0))
        color_only = nr.proxies(nr.control(value, 1, 0, .5))
        np.testing.assert_allclose(intensity_only[..., 1:], original[..., 1:], atol=1e-7, rtol=0)
        np.testing.assert_allclose(color_only[..., 0], original[..., 0], atol=1e-7, rtol=0)
        constant = np.full((48, 48, 3), [-.1, .3, 1.5], dtype=np.float32)
        np.testing.assert_array_equal(nr.control(constant, 4, 1, 1), constant)

    def test_affine_is_preserved_interior_but_real_boundary_uses_clipped_samples(self):
        y, x = np.mgrid[:48, :52]
        value = np.stack((-.03+x/256, .2+y/256, 1+(x-y)/512), axis=-1).astype(np.float32)
        out = nr.control(value, 4, 1, 1)
        np.testing.assert_allclose(out[4:-4, 4:-4], value[4:-4, 4:-4], atol=1e-7, rtol=0)
        np.testing.assert_allclose(out[:1, :1], value[:5, :5].astype(np.float64).mean(axis=(0, 1))[None, None], atol=1e-7, rtol=0)
        self.assertGreater(float(np.max(np.abs(out[0, 0]-value[0, 0]))), .001)

    def test_tiles_compose_radius_halos_at_nonzero_origins_and_true_borders(self):
        value = np.random.Generator(np.random.PCG64(17)).uniform(-1, 2, (53, 61, 3)).astype(np.float32)
        settings = {"radius": 4, "intensity_strength": .5, "color_strength": 1}
        full = nr.control(value, **settings); tiled = np.empty_like(full)
        for y in range(0, 53, 13):
            for x in range(0, 61, 17):
                w, h = min(17, 61-x), min(13, 53-y)
                tiled[y:y+h, x:x+w] = nr.tile(value, [x, y, w, h], **settings)
        np.testing.assert_allclose(tiled, full, atol=1e-6, rtol=0)
        missing_halo = nr.control(value[13:26, 17:34], **settings)
        self.assertGreater(float(np.max(np.abs(missing_halo-full[13:26, 17:34]))), .01)

    def test_native_filter_and_reduction_order_counterexample(self):
        config = nr.load_config(nr.CONFIG)
        _, truth, noisy = nr.truth_fixtures(config)[2]
        settings = {"radius": 4, "intensity_strength": 1, "color_strength": 1}
        native_first = nr.reduce_native(nr.control(noisy, **settings))
        reduced_first = nr.control(nr.reduce_native(noisy), **settings)
        self.assertGreater(float(np.max(np.abs(native_first-reduced_first))), .03)
        np.testing.assert_array_equal(nr.reduce_native(nr.control(truth)), nr.reduce_native(truth))

    def test_known_noise_response_and_known_edge_texture_damage_are_separate(self):
        fixtures = nr.truth_fixtures(nr.load_config(nr.CONFIG))
        for name, truth, noisy in fixtures:
            clean = nr.control(truth, 4, 1, 1); filtered = nr.control(noisy, 4, 1, 1)
            error = nr.error_metrics(filtered, truth)
            bias = nr.error_metrics(clean, truth)
            response = nr.error_metrics(filtered, clean)
            if name in ("flat", "affine"):
                self.assertLess(bias["RGB"]["maximum_absolute"], 1e-6)
                for measured, sigma in zip(response["L_Cr_Cb"]["rms"], [.02, .015, .015]):
                    self.assertAlmostEqual(measured, sigma/9, delta=sigma/9*.15)
            else:
                self.assertGreater(bias["RGB"]["joint_rms"], .005)
                self.assertGreater(error["RGB"]["joint_rms"], response["RGB"]["joint_rms"])

    def test_camera_common_centers_do_not_depend_on_artificial_crop_borders(self):
        value = np.random.Generator(np.random.PCG64(8)).uniform(-.1, 1, (256, 256, 3)).astype(np.float32)
        full = nr.control(value, 4, 1, 1)
        valid = full[4:-4, 4:-4]
        self.assertEqual(valid.shape, (248, 248, 3))
        # Independent filtering with genuine four-pixel context gives the same common centers.
        smaller = nr.control(value[4:-4, 4:-4], 4, 1, 1)
        np.testing.assert_allclose(smaller[8:-8, 8:-8], valid[8:-8, 8:-8], atol=1e-6, rtol=0)
        self.assertEqual(valid[8:-8, 8:-8].shape[:2], (232, 232))

    def test_invalid_input_settings_tiles_and_reduction_reject(self):
        a = np.ones((32, 32, 3), dtype=np.float32)
        for value in (a.astype(np.float64), a[..., :2], a[0], a[:0], np.full_like(a, np.nan),
                      np.full_like(a, np.inf), a*9, np.ones((1025, 1, 3), dtype=np.float32)):
            with self.assertRaises(ValueError): nr.control(value)
        for radius, intensity, color in ((True, 0, 0), (2, 0, 0), (1, True, 0), (1, -.1, 0), (4, 1.1, 0), (1, 0, float('nan'))):
            with self.assertRaises(ValueError): nr.control(a, radius, intensity, color)
        for rect in ([True, 0, 2, 2], [-1, 0, 2, 2], [0, 0, 33, 2], [0, 0, 0, 2], [1, 2, 3]):
            with self.assertRaises(ValueError): nr.tile(a, rect)
        for scale in (1, 3):
            with self.assertRaises(ValueError): nr.reduce_native(a, scale)
        with self.assertRaises(ValueError): nr.reduce_native(a[:31])

    def test_frozen_configuration_rejects_boolean_tuning_and_extra_presets(self):
        config = nr.load_config(nr.CONFIG)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"config.json"
            for changed in ({**config, "noise_control_version": True}, {**config, "seed": 3},
                            {**config, "presets": config["presets"]*2}):
                path.write_text(json.dumps(changed), encoding="utf-8")
                with self.assertRaises(ValueError): nr.load_config(path)

    def test_input_study_requires_complete_fixed_hashed_confined_buffers(self):
        plan_path = ROOT/"tests/reference/raw/camera_noise_rois_v1.json"
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp); buffer = folder/"rgb.bin"
            buffer.write_bytes(np.full((256, 256, 3), .25, dtype=np.float32).tobytes())
            report = {"raw_noise_characterization_version": 1, "complete": True,
                      "plan_sha256": nr.sha256_file(plan_path), "cases": []}
            for capture in plan["captures"]:
                case = {"id": capture["id"], "rois": []}
                for roi in capture["rois"]:
                    case["rois"].append({**roi, "algorithms": {algorithm: {"calibrated_path": "rgb.bin",
                        "metrics": {"domain": "native scene-linear prophoto-d50; diagnostic camera matrix"},
                        "calibrated_f32_sha256": nr.sha256_file(buffer)} for algorithm in ("rawengine.bilinear", "rawengine.menon_base")}})
                report["cases"].append(case)
            path = folder/"report.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            inputs, _ = nr.input_buffers(folder, path); self.assertEqual(len(inputs), 24)
            for field, value in (("complete", False), ("plan_sha256", "bad"), ("raw_noise_characterization_version", True)):
                altered = {**report, field: value}; path.write_text(json.dumps(altered), encoding="utf-8")
                with self.assertRaises(ValueError): nr.input_buffers(folder, path)
            altered = copy.deepcopy(report)
            altered["cases"][0]["rois"][0]["algorithms"]["rawengine.bilinear"]["calibrated_path"] = "../escape.bin"
            path.write_text(json.dumps(altered), encoding="utf-8")
            with self.assertRaises(ValueError): nr.input_buffers(folder, path)
            path.write_text(json.dumps(report), encoding="utf-8"); buffer.write_bytes(b'corrupt')
            with self.assertRaises(ValueError): nr.input_buffers(folder, path)


if __name__ == "__main__": unittest.main()
