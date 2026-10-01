"""Native Menon selection, frozen upstream pixels and graph/session integration."""
import array
import copy
import json
import math
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw
import python_raw_preview_tests as color

ROOT = Path(__file__).resolve().parents[1]
POLICY = {"algorithm": "rawengine.menon_base", "processing_version": 1}


def fixtures():
    data = (ROOT / "tests/reference/raw/menon_native_base_v1.bin").read_bytes()
    assert data[:8] == b"LRMENON1"
    count, = struct.unpack_from("<I", data, 8)
    offset, records = 12, []
    for _ in range(count):
        fields = struct.unpack_from("<10I8HI", data, offset); offset += struct.calcsize("<10I8HI")
        width, height, stride, pattern, px, py, x, y, w, h = fields[:10]
        metadata = {"row_stride_samples": stride, "pattern": pattern, "cfa_phase_x": px, "cfa_phase_y": py,
                    "active_x": x, "active_y": y, "active_width": w, "active_height": h,
                    "black_levels": fields[10:14], "white_levels": fields[14:18]}
        size = fields[18] * 2
        samples = data[offset:offset + size]; offset += size
        expected = data[offset:offset + w * h * 12]; offset += w * h * 12
        records.append((width, height, metadata, samples, expected))
    assert offset == len(data) and len(records) == 93
    return records


class MenonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.fixtures = fixtures()

    def session(self, index=0, **kwargs):
        width, height, metadata, samples, _ = self.fixtures[index]
        return raw.RawSession(samples, width, height, metadata, demosaic=copy.deepcopy(POLICY), **kwargs)

    def source_manifest(self, session):
        manifest = json.loads(session.export_manifest({}))
        manifest["operations"] = []; manifest["output"] = manifest["sources"][0]["id"]
        return json.dumps(manifest)

    def test_frozen_pixels_tiles_roi_signed_headroom_and_async(self):
        for index in (0, 7, 15, 31, 32, 33, 34, 35, 92):
            with self.subTest(fixture=index):
                _, _, meta, _, expected = self.fixtures[index]
                session = self.session(index, workers=2)
                text = self.source_manifest(session)
                w, h = meta["active_width"], meta["active_height"]
                full = session.render_manifest(text, {"tile_size": 512})
                self.assertEqual(full, (w, h, expected))
                self.assertEqual(session.render_manifest(text, {"tile_size": 13}), full)
                roi = {"x": meta["active_x"] + 2, "y": meta["active_y"] + 1,
                       "roi_width": 7, "roi_height": 5, "tile_size": 2}
                cropped = b"".join(expected[((1 + y) * w + 2) * 12:((1 + y) * w + 9) * 12] for y in range(5))
                self.assertEqual(session.render_manifest(text, roi), (7, 5, cropped))
                self.assertEqual(session.submit_manifest_latest("raw", text, roi).result(timeout=5), (7, 5, cropped))
                if index == 0:
                    values = array.array("f"); values.frombytes(expected)
                    self.assertLess(min(values), 0); self.assertGreater(max(values), 1)
                session.close()

    def test_calibrated_preview_before_reduction_and_roi_halo(self):
        session = self.session(32)
        _, _, meta, _, expected = self.fixtures[32]
        recipe = {"red_gain": 1.25, "green_gain": 0.8, "blue_gain": 1.6, "exposure_stops": 0.5,
                  "camera_to_xyz_d50": color.RawPreviewTests.matrix, "working_space": "prophoto-d50",
                  "output_mode": "srgb-preview"}
        document = json.loads(session.export_manifest(recipe))
        calibration = next(i for i, op in enumerate(document["operations"]) if op["type"] == "rawengine.camera_to_working")
        document["operations"] = document["operations"][:calibration + 1]
        document["output"] = document["operations"][-1]["id"]
        text = json.dumps(document)
        matrix = [color.f32(v) for v in color.multiply(color.xyz_to_working("prophoto-d50"), color.RawPreviewTests.matrix)]
        pixels = array.array("f"); pixels.frombytes(expected)
        calibrated = array.array("f")
        for i in range(0, len(pixels), 3):
            rgb = [color.f32(color.f32(pixels[i + c] * color.f32(gain)) * color.f32(math.sqrt(2))) for c, gain in enumerate((1.25, 0.8, 1.6))]
            for r in range(3):
                products = [color.f32(matrix[r * 3 + c] * rgb[c]) for c in range(3)]
                calibrated.append(color.f32(color.f32(products[0] + products[1]) + products[2]))
        w, h = meta["active_width"], meta["active_height"]
        reference = color.RawPreviewTests("test_independent_reference_patterns_phases_and_edges")
        for mip in (0, 1, 2):
            request = {"mip": mip, "quality": "preview" if mip else "final", "tile_size": 3}
            values, rw, rh = reference.reduced(calibrated, w, h, mip) if mip else (calibrated, w, h)
            actual = session.render_manifest(text, request)
            self.assertEqual(actual[:2], (rw, rh))
            for a, b in zip(color.values(actual), values): self.assertAlmostEqual(a, b, delta=1e-6)
            self.assertEqual(session.render_manifest(text, {**request, "tile_size": 7}), actual)
        source_text = self.source_manifest(session)
        request = {"x": meta["active_x"] + 10, "y": meta["active_y"] + 11, "roi_width": 2, "roi_height": 3}
        source_id = session.source_info()["id"]
        self.assertEqual(session.required_source_regions(source_text, request),
                         {source_id: (meta["active_x"] + 4, meta["active_y"] + 5, 14, 15)})
        # The normal encoded preview recipe also supports tile-independent jobs.
        preview = {**recipe, "mip": 2, "quality": "preview", "tile_size": 3}
        rendered = session.render(preview)
        self.assertEqual(session.render({**preview, "tile_size": 7}), rendered)
        self.assertEqual(session.submit(preview).result(timeout=5), rendered)
        session.close()

    def test_policy_ownership_replay_history_and_mismatch(self):
        session = self.session()
        text = self.source_manifest(session)
        expected = session.render_manifest(text)
        info = session.source_info(); info["demosaic"]["algorithm"] = "mutated"
        self.assertEqual(session.source_info()["demosaic"], POLICY)
        history = session.history(text)
        restored = session.restore_history(history.save())
        self.assertEqual(restored.render(), expected)
        width, height, metadata, samples, _ = self.fixtures[0]
        baseline = raw.RawSession(samples, width, height, metadata)
        self.assertEqual(baseline.source_info()["content_sha256"], session.source_info()["content_sha256"])
        self.assertNotEqual(baseline.render_manifest(self.source_manifest(baseline)), expected)
        with self.assertRaises(ValueError): baseline.render_manifest(text)
        with self.assertRaises(ValueError): baseline.restore_history(history.save())
        for version in (1, 2):
            legacy = json.loads(text); legacy["format_version"] = version; del legacy["sources"][0]["demosaic"]
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(legacy))
        for invalid in ({"algorithm": "rawengine.menon_base", "processing_version": 0},
                        {"algorithm": "rawengine.menon_base", "processing_version": 2},
                        {"algorithm": "rawengine.menon_refined", "processing_version": 1}):
            with self.assertRaises(ValueError): raw.RawSession(samples, width, height, metadata, demosaic=invalid)
        session.close(); baseline.close(); history.close(); restored.close()


if __name__ == "__main__": unittest.main()
