"""Owned optional profiles and native ICC source/output graph integration."""
import array
import copy
import gc
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import unittest

if __name__ == "__main__":
    sys.path.insert(0, sys.argv.pop(1))
    profile_path = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else None
    reference_exe = sys.argv.pop(1) if len(sys.argv) > 1 else None
else:
    profile_path = reference_exe = None
import rawengine_native as raw

AVAILABLE = raw.icc_available()
INTENTS = ("perceptual", "relative_colorimetric", "saturation", "absolute_colorimetric")
SPACES = ("prophoto-d50", "rec2020-d65")
A = "99000000-0000-0000-0000-000000000001"
B = "99000000-0000-0000-0000-000000000002"


def floats():
    return array.array("f", (((i * 19 % 37) - 7) / 16 for i in range(9 * 7 * 3)))


def encoded():
    result = array.array("H", [65535] * (11 * 7 * 3))
    for y in range(7):
        for x in range(9):
            for c in range(3):
                result[(y * 11 + x) * 3 + c] = ((y * 9 + x) * 3 + c) * 7919 % 65536
    return result


def settings(space, profile=None):
    result = dict(working_space=space, output_mode="icc-display", exposure_stops=.123456789,
                  tone_shoulder=.456789123, tone_gamma=1.23456789, tile_size=3)
    if profile is not None:
        result["output_profile"] = profile
    return result


def recipe(options):
    return {key: value for key, value in options.items() if key not in
            ("working_space", "input_profile", "output_profile", "row_stride_pixels")}


class IccTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if AVAILABLE:
            cls.bytes = profile_path.read_bytes()
            cls.reference = json.loads(subprocess.check_output([reference_exe, str(profile_path)]))

    def profile(self, intent="relative_colorimetric", bpc=False):
        return raw.create_icc_profile(self.bytes, intent=intent, black_point_compensation=bpc)

    def test_availability_and_handle_errors(self):
        self.assertIs(type(AVAILABLE), bool)
        for bad in (None, {}, b"profile", "profile", 1):
            with self.assertRaises(TypeError):
                raw.icc_profile_info(bad)
        if not AVAILABLE:
            with self.assertRaisesRegex(RuntimeError, "RAWENGINE_WITH_LCMS"):
                raw.create_icc_profile(b"x" * 128)
            with self.assertRaises(TypeError):
                raw.RasterSession(floats(), 9, 7, "prophoto-d50", output_profile={})
            with self.assertRaises(ValueError):
                raw.render_raster(floats(), 9, 7, settings("prophoto-d50"))

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_profile_policy_digest_and_byte_ownership(self):
        for intent in INTENTS:
            for bpc in (False, True):
                source = bytearray(self.bytes)
                profile = raw.create_icc_profile(source, intent=intent, black_point_compensation=bpc)
                source[:] = b"\0" * len(source)
                info = raw.icc_profile_info(profile)
                self.assertEqual(info, dict(profile_sha256=hashlib.sha256(self.bytes).hexdigest(),
                    intent=intent, black_point_compensation=bpc, engine="lcms2-core", engine_version="2.19.1",
                    profile_bytes=len(self.bytes)))
                info["intent"] = "changed"
                self.assertEqual(raw.icc_profile_info(profile)["intent"], intent)

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_profile_validation(self):
        for bad in (b"", b"x" * 127, b"x" * 128, b"x" * (16 * 1024 * 1024 + 1),
                    memoryview(self.bytes)[::2], array.array("H", [0] * 128)):
            with self.assertRaises((ValueError, TypeError, BufferError)):
                raw.create_icc_profile(bad)
        nonrgb = bytearray(self.bytes)
        nonrgb[16:20] = b"CMYK"
        with self.assertRaises(ValueError):
            raw.create_icc_profile(nonrgb)
        for intent in ("", "Relative", "relative_colorimetric\0bad", 2):
            with self.assertRaises((ValueError, TypeError)):
                raw.create_icc_profile(self.bytes, intent=intent)
        for bpc in (0, 1, "false", None):
            with self.assertRaises(TypeError):
                raw.create_icc_profile(self.bytes, black_point_compensation=bpc)

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_exact_direct_cpp_parity_all_policies_and_spaces(self):
        for ref in self.reference:
            profile = self.profile(ref["intent"], ref["bpc"])
            options = settings(ref["space"], profile)
            actual = raw.render_raster(floats(), 9, 7, options)
            self.assertEqual(actual, (9, 7, bytes.fromhex(ref["float"])))
            imported = {**options, "row_stride_pixels": 11, "input_profile": profile}
            self.assertEqual(raw.render_raster(encoded(), 9, 7, imported), (9, 7, bytes.fromhex(ref["icc"])))
            pixels = array.array("H", (i * 197 % 4096 for i in range(9 * 7)))
            camera = {**options, "black_levels": (64, 96, 32, 80), "white_levels": (4095,) * 4,
                      "red_gain": 1.25, "green_gain": .8, "blue_gain": 1.6,
                      "camera_to_xyz_d50": (.55, .22, .13, .17, .68, .09, .03, .11, .73)}
            self.assertEqual(raw.render(pixels, 9, 7, camera), (9, 7, bytes.fromhex(ref["raw"])))
            session = raw.RasterSession(encoded(), 9, 7, ref["space"], 11, input_profile=profile, output_profile=profile)
            self.assertEqual(session.source_info()["content_sha256"], ref["source_sha256"])
            saved = session.export_manifest(recipe(options))
            self.assertEqual(session.render_manifest(saved), (9, 7, bytes.fromhex(ref["icc"])))
            session.close()

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_sessions_cache_roi_jobs_analysis_and_level_gates(self):
        profile = self.profile()
        for space in SPACES:
            for use_import in (False, True):
                samples = encoded() if use_import else floats()
                session = raw.RasterSession(samples, 9, 7, space, 11 if use_import else 0,
                    input_profile=profile if use_import else None, output_profile=profile, workers=2)
                options = settings(space, profile)
                if use_import:
                    options.update(input_profile=profile, row_stride_pixels=11)
                expected = raw.render_raster(samples, 9, 7, options)
                saved = session.export_manifest(recipe(options))
                samples[:] = array.array(samples.typecode, [0] * len(samples))
                self.assertEqual(session.render_manifest(saved), expected)
                before = session.cache_stats()
                self.assertEqual(session.render_manifest(saved), expected)
                self.assertEqual(session.cache_stats()["misses"], before["misses"])
                self.assertEqual(session.submit_manifest(saved).result(timeout=5), expected)
                self.assertEqual(session.submit_latest("view", recipe(options)).result(timeout=5), expected)
                self.assertEqual(session.submit_manifest_latest("view", saved).result(timeout=5), expected)
                for tile in (1, 3, 64):
                    self.assertEqual(session.render_manifest(saved, {"tile_size": tile}), expected)
                request = dict(x=8, y=6, roi_width=1, roi_height=1, tile_size=1)
                self.assertEqual(session.render_manifest(saved, request), (1, 1, expected[2][-12:]))
                source = json.loads(saved)["sources"][0]["id"]
                self.assertEqual(session.required_source_regions(saved, request), {source: (8, 6, 1, 1)})
                histogram = session.histogram_manifest(saved)
                self.assertEqual(histogram["descriptor"]["profile_sha256"], raw.icc_profile_info(profile)["profile_sha256"])
                tiles = []
                session.analyze_local_manifest(saved, tiles.append, {"tile_size": 3})
                self.assertTrue(tiles)
                for mip in (1, 2):
                    req = dict(mip=mip, quality="preview")
                    for action in (session.render_manifest, session.required_source_regions, session.submit_manifest):
                        with self.assertRaises(ValueError):
                            action(saved, req)
                for key in ("input_profile", "output_profile"):
                    with self.assertRaises(ValueError):
                        session.render({key: profile})
                session.close()

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_mixed_output_history_replay_and_lifetimes(self):
        profile = self.profile()
        session = raw.RasterSession(floats(), 9, 7, "prophoto-d50", output_profile=profile)
        icc = session.export_manifest(recipe(settings("prophoto-d50")))
        srgb = session.export_manifest(dict(output_mode="srgb-preview"))
        expected_icc, expected_srgb = session.render_manifest(icc), session.render_manifest(srgb)
        history = session.history(srgb)
        first = history.stats()["current_id"]
        second = history.commit(icc)
        self.assertEqual(history.compare(first, second), (expected_srgb, expected_icc))
        self.assertEqual(history.undo(), first)
        self.assertEqual(history.redo(), second)
        restored = session.restore_history(history.save())
        self.assertEqual(restored.render(), expected_icc)
        self.assertEqual(restored.render(revision=first), expected_srgb)
        job = restored.submit_latest("icc")
        del profile, session, history
        gc.collect()
        self.assertEqual(job.result(timeout=5), expected_icc)
        self.assertEqual(restored.submit(revision=first).result(timeout=5), expected_srgb)
        restored.close()

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_geometry_import_native_quality_and_request_validation(self):
        profile = self.profile()
        for space in SPACES:
            session = raw.RasterSession(encoded(), 9, 7, space, 11, input_profile=profile, output_profile=profile)
            for geometry in (dict(crop=(1, 1, 7, 5)), dict(rotate=90, flip_horizontal=True),
                             dict(resize=(7, 5), resize_filter="area"),
                             dict(crop=(1, 1, 7, 5), rotate=270, resize=(5, 3), resize_filter="bilinear")):
                options = {**settings(space, profile), **geometry, "input_profile": profile, "row_stride_pixels": 11}
                expected = raw.render_raster(encoded(), 9, 7, options)
                saved = session.export_manifest(recipe(options))
                self.assertEqual(session.render_manifest(saved, {"tile_size": 1}), expected)
                self.assertEqual(session.submit_manifest(saved).result(timeout=5), expected)
                self.assertEqual(session.history(saved).render(), expected)
            for quality in (dict(mip=0, quality="preview"), dict(mip=1, quality="final")):
                with self.assertRaises(ValueError):
                    session.render({**recipe(settings(space)), **quality})
            session.close()

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_binding_policy_mismatch_and_recovery(self):
        profile = self.profile()
        session = raw.RasterSession(encoded(), 9, 7, "prophoto-d50", 11, input_profile=profile, output_profile=profile)
        saved = json.loads(session.export_manifest(recipe(settings("prophoto-d50"))))
        expected = session.render_manifest(json.dumps(saved))
        for path in ("source", "output"):
            for field in ("profile_sha256", "intent", "black_point_compensation", "engine_version"):
                bad = copy.deepcopy(saved)
                identity = bad["sources"][0]["icc_input"] if path == "source" else bad["output_profile"]
                identity[field] = {"profile_sha256": "0" * 64, "intent": "perceptual",
                                   "black_point_compensation": True, "engine_version": "0"}[field]
                with self.assertRaises(ValueError):
                    session.render_manifest(json.dumps(bad))
        wrong = raw.RasterSession(encoded(), 9, 7, "prophoto-d50", 11, input_profile=profile)
        with self.assertRaises(ValueError):
            wrong.restore_history(session.history(json.dumps(saved)).save())
        self.assertEqual(session.render_manifest(json.dumps(saved)), expected)
        session.close(); wrong.close()

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_multisource_import_replacement_and_pinned_history(self):
        profile = self.profile()
        space = "prophoto-d50"
        def spec():
            return dict(rgb=encoded(), width=9, height=7, row_stride_pixels=11, working_space=space, input_profile=profile)
        session = raw.RasterGraphSession({A: spec(), B: dict(rgb=floats(), width=9, height=7, working_space=space)}, output_profile=profile)
        doc = json.loads(session.export_manifest(A))
        mix = dict(id="99000000-0000-0000-0000-000000000090", type="rawengine.linear_mix", schema_version=1,
                   processing_version=2, enabled=True, input_domain="scene_linear_prophoto_d50",
                   output_domain="scene_linear_prophoto_d50", parameters={"amount": .25}, inputs={"base": A, "layer": B},
                   masks={}, blend_mode="normal", opacity=1.0)
        doc.update(operations=[mix], output=mix["id"])
        text = json.dumps(doc)
        expected = session.render_manifest(text)
        history = session.history(text)
        before = session.source_info()
        with self.assertRaises(ValueError):
            session.replace_source(A, {**spec(), "rgb": array.array("H", [0])})
        self.assertEqual(session.source_info(), before)
        replacement = spec(); replacement["rgb"][0] ^= 4095
        session.replace_source(A, replacement)
        with self.assertRaises(ValueError):
            session.render_manifest(text)
        self.assertEqual(history.render(), expected)
        del profile, session
        gc.collect()
        self.assertEqual(history.submit().result(timeout=5), expected)
        history.close()

    @unittest.skipUnless(AVAILABLE, "optional ICC backend disabled")
    def test_input_format_and_output_selection_validation(self):
        profile = self.profile()
        for bad in (floats(), array.array("I", [0] * 189), encoded()[:-1], memoryview(encoded())[::2]):
            with self.assertRaises((ValueError, BufferError)):
                raw.RasterSession(bad, 9, 7, "prophoto-d50", 11, input_profile=profile)
        self.assertEqual(raw.RasterSession(encoded().tobytes(), 9, 7, "prophoto-d50", 11, input_profile=profile).source_info()["kind"], "icc_raster_u16")
        for mode in ("legacy", "srgb-preview"):
            with self.assertRaises(ValueError):
                raw.render_raster(floats(), 9, 7, dict(working_space="prophoto-d50", output_mode=mode, output_profile=profile))
        with self.assertRaises(ValueError):
            raw.render(array.array("H", [0] * 63), 9, 7, dict(output_mode="icc-display", output_profile=profile))
        session = raw.RawSession(array.array("H", (i * 197 % 4096 for i in range(63))), 9, 7, output_profile=profile)
        camera = dict(output_mode="icc-display", camera_to_xyz_d50=(.55,.22,.13,.17,.68,.09,.03,.11,.73), working_space="prophoto-d50")
        saved = session.export_manifest(camera)
        self.assertEqual(session.render(camera), session.render_manifest(saved))
        self.assertEqual(session.submit_manifest(saved).result(timeout=5), session.render(camera))
        for mip in (1, 2):
            with self.assertRaises(ValueError):
                session.render({**camera, "mip": mip, "quality": "preview"})
        session.close()


if __name__ == "__main__":
    unittest.main()
