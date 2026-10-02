"""Optional research-adapter tests; no camera originals or decoder needed.

Run with a NumPy-equipped research Python. Not a product runtime dependency.
"""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_camera_extract as extract
import raw_camera_engine_check as check
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
import raw_session_benchmark as benchmark


class AdapterTests(unittest.TestCase):
    def test_benchmark_stage_follows_dependencies_instead_of_operation_order(self):
        document = {"sources": [{"id": "source"}], "operations": [
            {"id": "a", "inputs": {"image": "z"}},
            {"id": "unused", "inputs": {"image": "missing"}},
            {"id": "z", "inputs": {"image": "source"}}], "output": "unused"}
        stage = benchmark.stage_manifest(document, "a")
        self.assertEqual([op["id"] for op in stage["operations"]], ["a", "z"])
        self.assertEqual(stage["output"], "a")
        self.assertEqual(benchmark.stage_manifest(document, "source")["operations"], [])
        self.assertEqual(document["output"], "unused")

    def test_benchmark_stage_rejects_missing_edges_and_cycles(self):
        for operations in ([{"id": "a", "inputs": {"image": "missing"}}],
                           [{"id": "a", "inputs": {"image": "b"}}, {"id": "b", "inputs": {"image": "a"}}]):
            with self.assertRaises(ValueError):
                benchmark.stage_manifest({"sources": [], "operations": operations}, "a")

    def test_incremental_selection_rejects_missing_duplicate_and_escaping_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("a.NEF", "b.nef", "reference.jpg"):
                (root / name).write_bytes(b"selection fixture")
            self.assertEqual([p.name for p in extract.raw_paths(root, ["b.nef"])], ["b.nef"])
            self.assertEqual([p.name for p in extract.raw_paths(root)], ["a.NEF", "b.nef"])
            for names in ([], ["a.NEF", "a.NEF"], ["missing.NEF"], ["reference.jpg"],
                          ["../a.NEF"], ["sub\\a.NEF"], [str((root / "a.NEF").resolve())]):
                with self.assertRaises(ValueError): extract.raw_paths(root, names)

    def decoder(self):
        pattern = np.array([[2, 3], [1, 0]], dtype=np.uint8)
        return SimpleNamespace(raw_image=np.zeros((8, 10), dtype=np.uint16), raw_pattern=pattern,
            color_desc=b"RGBG", num_colors=3,
            raw_color=lambda y, x: int(pattern[y % 2, x % 2]),
            black_level_per_channel=[4, 5, 6, 7], camera_white_level_per_channel=[100, 200, 300, 400],
            white_level=65535,
            sizes=SimpleNamespace(raw_width=10, raw_height=8, left_margin=1, top_margin=3, width=7, height=5))

    def test_channel_indices_map_to_sensor_sites_and_origin_is_retained(self):
        metadata, name, channels = extract.sensor_metadata(self.decoder())
        self.assertEqual(name, "BGGR")
        self.assertEqual(channels, [2, 3, 1, 0])
        self.assertEqual(metadata["black_levels"], [6, 7, 5, 4])
        self.assertEqual(metadata["white_levels"], [300, 400, 200, 100])
        self.assertEqual(metadata["active_area"], [1, 3, 7, 5])
        self.assertEqual(check.native_metadata(metadata)["active_y"], 3)

    def test_scalar_white_fallback(self):
        decoder = self.decoder(); decoder.camera_white_level_per_channel = None
        self.assertEqual(extract.sensor_metadata(decoder)[0]["white_levels"], [65535] * 4)

    def test_unsupported_sensor_and_invalid_metadata_reject(self):
        for changes in ({"raw_pattern": None}, {"raw_image": np.zeros((8, 10), dtype=np.float32)},
                        {"color_desc": b"CMYG"}, {"num_colors": 4},
                        {"camera_white_level_per_channel": [4, 200, 300, 400]},
                        {"raw_color": lambda y, x: 0}):
            decoder = self.decoder()
            for k, value in changes.items(): setattr(decoder, k, value)
            with self.assertRaises(ValueError): extract.sensor_metadata(decoder)
        decoder = self.decoder(); decoder.sizes.width = 10
        with self.assertRaises(ValueError): extract.sensor_metadata(decoder)

    def test_matrix_direction_and_d65_neutral(self):
        record = {"decoder_rgb_xyz_matrix_uninterpreted": [[1.3705, -.6004, -.14],
                     [-.5464, 1.3568, .2062], [-.094, .1706, .7618], [0, 0, 0]]}
        matrix = np.array(check.camera_matrix(record)).reshape(3, 3)
        np.testing.assert_allclose(matrix @ np.ones(3), [.96422, 1.0, .82521], atol=2e-7)
        for basis in ([[0, 0, 0]] * 4, [[1, 0, 0]] * 4):
            with self.assertRaises(ValueError): check.camera_matrix({**record, "decoder_rgb_xyz_matrix_uninterpreted": basis})

    def test_observed_channel_check_preserves_signed_and_headroom(self):
        meta = {"pattern": 0, "black_levels": [10] * 4, "white_levels": [20] * 4}
        codes = np.array([[0, 20], [10, 30]], dtype=np.uint16)
        rgb = np.zeros((2, 2, 3), dtype=np.float32)
        rgb[0, 0, 0] = -1; rgb[0, 1, 1] = 1; rgb[1, 0, 1] = 0; rgb[1, 1, 2] = 2
        self.assertEqual(check.observed_check((2, 2, rgb.tobytes()), codes, meta, [0, 0, 2, 2]), 4)
        rgb[0, 0, 0] = 0
        with self.assertRaises(ValueError): check.observed_check((2, 2, rgb.tobytes()), codes, meta, [0, 0, 2, 2])

    def test_native_preview_rois_align_relative_to_nonzero_active_origin(self):
        meta = {"active_area": [3, 5, 1001, 777]}
        for x, y, w, h in check.sensor_rois(meta):
            self.assertEqual((x - 3) % 4, 0); self.assertEqual((y - 5) % 4, 0)
            self.assertLessEqual(x + w, 1004); self.assertLessEqual(y + h, 782)

    def test_reviewed_rois_reject_changed_extent_bounds_and_mip_anchor(self):
        meta = {"active_area": [3, 5, 1001, 777]}
        rois = [[3, 5, 256, 256]] * 4
        self.assertEqual(check.sensor_rois(meta, rois), rois)
        for roi in ([4, 5, 256, 256], [3, 5, 255, 256], [3, 600, 256, 256], [True, 5, 256, 256]):
            with self.assertRaises(ValueError): check.sensor_rois(meta, [roi] * 4)


if __name__ == "__main__": unittest.main()
