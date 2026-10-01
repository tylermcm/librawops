"""Synthetic admission/coverage tests; these are not real camera samples."""
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import raw_camera_corpus as c


class CorpusTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.record = json.loads((Path(__file__).parent / "reference/raw/camera_corpus_plan_v1.json").read_text())
        self.record["coverage_targets"]["minimum_camera_models"] = 2
        self.proof = self.file("rights.txt", b"Synthetic unit-test data; original test code. Not camera evidence.")

    def file(self, path, data):
        (self.root / path).write_bytes(data)
        return {"path": path, "sha256": hashlib.sha256(data).hexdigest()}

    def asset(self, index, model="test-a", iso=100, light="daylight"):
        return {"id": f"fixture-{index}",
                "original": self.file(f"capture-{index}.opaque", f"synthetic original {index}".encode()),
                "decoded": self.file(f"decoded-{index}.bin", struct.pack("<35H", *range(35))),
                "encoding": "little-endian uint16 Bayer; complete padded sensor buffer",
                "metadata": {"width": 5, "height": 5, "row_stride_samples": 7, "pattern": 0,
                             "cfa_phase_x": 1, "cfa_phase_y": 0, "active_area": [1, 1, 3, 3],
                             "black_levels": [4, 5, 6, 7], "white_levels": [100, 200, 300, 400]},
                "capture": {"make": "unit-test", "model": model, "iso": iso,
                            "iso_band": "low" if iso <= 400 else "high", "illuminant": light,
                            "exposure_seconds": 0.01,
                            "wb_reference": {"method": "camera-as-shot", "camera_rgb_gains": [2, 1, 1.5]}},
                "decode_tool": {"name": "synthetic fixture", "version": "1", "settings": "none"},
                "rights": {"review_status": "reviewed", "identifier": "original test data",
                           "redistribution": "allowed", "evidence": self.proof},
                "content_tags": sorted(c.CONTENT_TAGS), "rois": [[1, 1, 3, 3], [3, 3, 1, 1]]}

    def complete(self):
        for model in ("test-a", "test-b"):
            for iso in (100, 3200):
                for light in ("daylight", "tungsten"):
                    self.record["assets"].append(self.asset(len(self.record["assets"]), model, iso, light))
        return self.record

    def test_empty_plan_cannot_be_reported_ready(self):
        result = c.validate(self.record, self.root)
        self.assertEqual(result["asset_count"], 0)
        self.assertEqual(result["missing_camera_models"], 2)
        self.assertFalse(result["coverage_ready"])
        self.assertFalse(result["all_assets_declared_redistributable"])

    def test_complete_declared_coverage_and_determinism(self):
        result = c.validate(self.complete(), self.root)
        self.assertTrue(result["coverage_ready"])
        self.assertEqual(result["asset_count"], 8)
        self.assertEqual(result["verified_file_records"], 24)
        self.assertEqual(result["camera_model_count"], 2)
        self.assertEqual(result["missing_condition_cells"], [])
        self.assertEqual(result["missing_content_tags"], [])
        self.record["assets"].reverse()
        self.assertEqual(c.validate(self.record, self.root), result)

    def test_missing_condition_content_and_local_rights_remain_explicit(self):
        record = self.complete()
        record["assets"].pop()
        for asset in record["assets"]:
            asset["content_tags"].remove("skin")
        record["assets"][0]["rights"]["redistribution"] = "local-only"
        result = c.validate(record, self.root)
        self.assertFalse(result["coverage_ready"])
        self.assertFalse(result["all_assets_declared_redistributable"])
        self.assertEqual(result["local_only_assets"], ["fixture-0"])
        self.assertEqual(result["missing_content_tags"], ["skin"])
        self.assertEqual(result["missing_condition_cells"], [(("unit-test", "test-b"), "high", "tungsten")])

    def test_file_identity_missing_size_and_path_confinement(self):
        asset = self.asset(0)
        for field in ("original", "decoded"):
            record = copy.deepcopy(asset); record[field]["sha256"] = "0" * 64
            with self.assertRaises(ValueError): c.validate({**self.record, "assets": [record]}, self.root)
        record = copy.deepcopy(asset); record["rights"]["evidence"]["sha256"] = "0" * 64
        with self.assertRaises(ValueError): c.validate({**self.record, "assets": [record]}, self.root)
        for path in ("../outside.raw", "C:/outside.raw", "/outside.raw", "sub\\capture.raw", "missing.raw"):
            record = copy.deepcopy(asset); record["original"]["path"] = path
            with self.assertRaises(ValueError): c.validate({**self.record, "assets": [record]}, self.root)
        (self.root / asset["decoded"]["path"]).write_bytes(b"too short")
        with self.assertRaisesRegex(ValueError, "byte count"): c.validate({**self.record, "assets": [asset]}, self.root)

    def test_duplicate_capture_ids_and_unreviewed_rights_reject(self):
        asset = self.asset(0)
        second = copy.deepcopy(asset)
        with self.assertRaisesRegex(ValueError, "duplicate asset ID"):
            c.validate({**self.record, "assets": [asset, second]}, self.root)
        second["id"] = "different"
        with self.assertRaisesRegex(ValueError, "duplicate original"):
            c.validate({**self.record, "assets": [asset, second]}, self.root)
        for status, redistribution in (("pending", "allowed"), ("reviewed", "unknown")):
            record = copy.deepcopy(asset)
            record["rights"].update(review_status=status, redistribution=redistribution)
            with self.assertRaises(ValueError): c.validate({**self.record, "assets": [record]}, self.root)

    def test_bad_sensor_layout_phase_levels_and_roi_reject(self):
        asset = self.asset(0)
        for changes in ({"width": True}, {"row_stride_samples": 4}, {"pattern": 4}, {"cfa_phase_y": 2},
                        {"active_area": [4, 4, 3, 3]}, {"active_area": [1, 1, 0, 3]},
                        {"black_levels": [0, 0, 0]}, {"white_levels": [4, 200, 300, 400]}):
            record = copy.deepcopy(asset); record["metadata"].update(changes)
            with self.assertRaises(ValueError): c.validate({**self.record, "assets": [record]}, self.root)
        for rois in ([[0, 1, 1, 1]], [[1, 1, 0, 1]], []):
            record = copy.deepcopy(asset); record["rois"] = rois
            with self.assertRaises(ValueError): c.validate({**self.record, "assets": [record]}, self.root)

    def test_invalid_capture_provenance_encoding_and_coverage_reject(self):
        asset = self.asset(0)
        for changes in ({"iso": True}, {"iso": 3200}, {"illuminant": "unknown"},
                        {"exposure_seconds": math.nan}, {"exposure_seconds": 0},
                        {"wb_reference": {"method": "camera-as-shot", "camera_rgb_gains": [1, 0, 1]}}):
            record = copy.deepcopy(asset); record["capture"].update(changes)
            with self.assertRaises(ValueError): c.validate({**self.record, "assets": [record]}, self.root)
        for field, value in (("encoding", "native-endian"), ("content_tags", ["unknown"]),
                             ("decode_tool", {"name": "x", "version": "", "settings": "x"})):
            record = copy.deepcopy(asset); record[field] = value
            with self.assertRaises(ValueError): c.validate({**self.record, "assets": [record]}, self.root)
        for target in ({"minimum_camera_models": True}, {"iso_bands": ["low", "low"]}, {"content_tags": []}):
            record = copy.deepcopy(self.record); record["coverage_targets"].update(target)
            with self.assertRaises(ValueError): c.validate(record, self.root)
        with self.assertRaises(ValueError): c.validate({**self.record, "corpus_schema_version": 2}, self.root)

    def test_unknown_and_duplicate_json_fields_reject(self):
        with self.assertRaises(ValueError): c.validate({**self.record, "unknown": 1}, self.root)
        with self.assertRaises(ValueError): json.loads('{"a": 1, "a": 2}', object_pairs_hook=c.unique_object)


if __name__ == "__main__":
    unittest.main()
