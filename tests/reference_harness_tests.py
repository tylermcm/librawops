"""Reference harness tests; no image libraries or Adobe installation required."""

import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import reference_harness as harness


class ReferenceHarnessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.manifest = harness.generate(self.directory)
        self.source = self.directory / "neutral_ramps.tif"

    def test_fixtures_are_deterministic_and_profiled(self):
        self.assertEqual(harness.generate(self.directory), self.manifest)
        self.assertEqual(len(self.manifest["cases"]), 3)
        for case in self.manifest["cases"]:
            path = self.directory / case["file"]
            self.assertEqual(harness.file_digest(path), case["sha256"])
            with harness.Tiff16(path) as image:
                self.assertEqual((image.width, image.height),
                                 (case["width"], case["height"]))
                self.assertEqual(harness.digest(image.profile), harness.PROFILE_SHA256)

    def test_equal_and_one_code_difference(self):
        diff = self.directory / "diff.tif"
        metrics = harness.compare(self.source, self.source, diff)
        self.assertEqual(metrics["different_samples"], 0)
        self.assertEqual(metrics["rmse_code"], 0)
        self.assertIsNone(metrics["psnr_db"])
        self.assertEqual(metrics["comparison_kind"], "unverified image pair")
        with harness.Tiff16(diff) as image:
            self.assertEqual(image.row(0)[0], 0)
            self.assertEqual(image.profile, b"")

        candidate = self.directory / "one_code.tif"
        shutil.copyfile(self.source, candidate)
        with harness.Tiff16(candidate) as image:
            pixel_offset = image.strip_offsets[0]
        with candidate.open("r+b") as output:
            output.seek(pixel_offset)
            output.write(struct.pack("<H", 1))
        metrics = harness.compare(self.source, candidate)
        self.assertEqual(metrics["max_abs_code"], 1)
        self.assertEqual(metrics["different_samples"], 1)
        self.assertAlmostEqual(metrics["mean_abs_code"], 1 / (256 * 64 * 3))
        self.assertGreater(metrics["psnr_db"], 90)

    def test_profile_and_compression_fail_closed(self):
        changed = self.directory / "profile_changed.tif"
        payload = bytearray(self.source.read_bytes())
        offset = payload.find(harness.checked_profile())
        self.assertGreater(offset, 0)
        payload[offset + 100] ^= 1
        changed.write_bytes(payload)
        with self.assertRaisesRegex(ValueError, "ICC bytes"):
            harness.compare(self.source, changed)

        compressed = self.directory / "compressed.tif"
        payload = bytearray(self.source.read_bytes())
        compression_entry = 8 + 2 + harness.TIFF_TAGS.index(259) * 12
        struct.pack_into("<I", payload, compression_entry + 8, 5)
        compressed.write_bytes(payload)
        with self.assertRaisesRegex(ValueError, "unsupported TIFF tag 259"):
            harness.compare(self.source, compressed)

    def test_multiple_strips_are_read_without_full_image_allocation(self):
        multiple = self.directory / "multiple_strips.tif"
        payload = bytearray(self.source.read_bytes())
        with harness.Tiff16(self.source) as image:
            pixels = image.strip_offsets[0]
            row_bytes = image.width * 6
        array_offset = len(payload)
        payload += struct.pack("<2I", pixels, pixels + 32 * row_bytes)
        counts_offset = len(payload)
        payload += struct.pack("<2I", 32 * row_bytes, 32 * row_bytes)
        for tag, count, value in ((273, 2, array_offset), (278, 1, 32),
                                  (279, 2, counts_offset)):
            entry = 8 + 2 + harness.TIFF_TAGS.index(tag) * 12
            struct.pack_into("<I", payload, entry + 4, count)
            struct.pack_into("<I", payload, entry + 8, value)
        multiple.write_bytes(payload)
        metrics = harness.compare(self.source, multiple)
        self.assertEqual(metrics["different_samples"], 0)

    def test_big_endian_tiff_is_compared_by_sample_values(self):
        big = self.directory / "big_endian.tif"
        with harness.Tiff16(self.source) as image, big.open("wb") as output:
            output.write(harness.make_tiff_header(image.width, image.height,
                                                  image.profile, ">"))
            for y in range(image.height):
                row = image.row(y)
                if sys.byteorder == "little":
                    row.byteswap()
                output.write(row.tobytes())
        metrics = harness.compare(self.source, big)
        self.assertEqual(metrics["different_samples"], 0)

    def test_capture_provenance_must_be_complete_and_match_hashes(self):
        template = self.directory / "capture_template.json"
        with self.assertRaisesRegex(ValueError, "incomplete"):
            harness.compare(self.source, self.source,
                            capture_path=template, capture_source=self.source)
        capture = json.loads(template.read_text(encoding="utf-8"))
        capture.update({field: "N/A" for field in harness.METADATA_FIELDS})
        capture.update({"application": "Photoshop", "application_build": "fixture-test",
                        "source_sha256": harness.file_digest(self.source),
                        "reference_sha256": harness.file_digest(self.source),
                        "output_profile_sha256": harness.PROFILE_SHA256,
                        "output_bit_depth": 16, "compression": "none",
                        "pixel_order": "interleaved", "orientation": 1,
                        "byte_order": "little", "all_adjustment_settings": {},
                        "automatic_adjustments": False,
                        "status": "Measured"})
        template.write_text(json.dumps(capture), encoding="utf-8")
        result = harness.compare(self.source, self.source,
                                 capture_path=template, capture_source=self.source)
        self.assertEqual(result["comparison_kind"], "Adobe reference")
        capture["reference_sha256"] = "0" * 64
        template.write_text(json.dumps(capture), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "checksum"):
            harness.compare(self.source, self.source,
                            capture_path=template, capture_source=self.source)


if __name__ == "__main__":
    unittest.main()
