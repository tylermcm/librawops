"""Saved graph replay, verified single-source binding and Python job integration."""
import array
import copy
from concurrent.futures import ThreadPoolExecutor
import gc
import json
import sys
import time
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw


def uid(number):
    return f"10000000-0000-0000-0000-{number:012d}"


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.pixels = array.array("f")
        for y in range(5):
            for x in range(7):
                self.pixels.extend((x / 8 - 0.25, y / 4, (x + y) / 6))

    def session(self, space="prophoto-d50", **kwargs):
        return raw.RasterSession(self.pixels, 7, 5, space, **kwargs)

    def graph(self, session, **options):
        return json.loads(session.export_manifest({"output_mode": "srgb-preview", **options}))

    def test_source_identity_export_replay_and_cache(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            session = self.session(space)
            info = session.source_info()
            self.assertEqual((info["width"], info["height"]), (7, 5))
            options = dict(output_mode="srgb-preview", crop=(1, 1, 5, 3), rotate=90,
                           flip_horizontal=True, resize=(9, 7), resize_filter="area",
                           exposure_stops=0.5, tone_shoulder=0.8, tone_gamma=1.3)
            text = session.export_manifest(options)
            saved = json.loads(text)
            self.assertEqual(saved["format_version"], 2)
            self.assertEqual(saved["sources"][0], {k: v for k, v in info.items() if k not in ("width", "height")})
            self.assertEqual(text, session.export_manifest({**options, "tile_size": 1, "mip": 2}))
            # Metadata is a copy; mutating it cannot alter source identity.
            info["content_sha256"] = "0" * 64
            self.assertNotEqual(session.source_info(), info)
            for mip in (0, 1, 2):
                request = {"mip": mip, "quality": "final" if mip == 0 else "preview", "tile_size": 2}
                expected = session.render({**options, **request})
                self.assertEqual(session.render_manifest(text, request), expected)
                self.assertEqual(session.render_manifest(json.dumps(saved), {**request, "tile_size": 1}), expected)
                roi = {**request, "x": 1, "y": 1, "roi_width": 1, "roi_height": 1}
                self.assertEqual(session.render_manifest(text, roi), session.render({**options, **roi}))
                job = session.submit_manifest(text, request)
                with ThreadPoolExecutor(max_workers=2) as pool:
                    self.assertEqual(list(pool.map(lambda _: job.result(timeout=5), range(2))), [expected] * 2)
                w, h = expected[:2]
                total = ((w + 1) // 2) * ((h + 1) // 2)
                self.assertEqual(job.progress(), {"completed_tiles": total, "total_tiles": total})
                self.assertEqual(session.submit_manifest_latest("view", text, request).result(timeout=5), expected)
            session.clear_cache()
            session.render_manifest(text, {"mip": 1, "quality": "preview"})
            before = session.cache_stats()
            session.render_manifest(text, {"mip": 1, "quality": "preview"})
            warm = session.cache_stats()
            self.assertGreater(warm["hits"], before["hits"])
            self.assertEqual(warm["misses"], before["misses"])
            changed = copy.deepcopy(saved)
            tone = next(op for op in changed["operations"] if op["type"] == "rawengine.tone_curve")
            tone["parameters"]["shoulder"] = 1.5
            self.assertEqual(session.render_manifest(json.dumps(changed), {"mip": 1, "quality": "preview"}),
                             session.render({**options, "tone_shoulder": 1.5, "mip": 1, "quality": "preview"}))
            self.assertGreater(session.cache_stats()["hits"], warm["hits"])
            # The core reader also migrates v1's explicitly documented source space.
            migrated = copy.deepcopy(saved)
            migrated["format_version"] = 1
            del migrated["sources"][0]["working_space"]
            self.assertEqual(session.render_manifest(json.dumps(migrated)), session.render(options))
            # Equivalent owned sources may bind the same saved graph.
            other = self.session(space)
            self.assertEqual(other.render_manifest(text), session.render(options))
            session.close()
            other.close()

    def test_source_only_and_branched_numeric_output(self):
        session = self.session()
        document = self.graph(session)
        document["operations"] = []
        source = document["sources"][0]
        document["output"] = source["id"]
        source_text = json.dumps(document)
        self.assertEqual(session.render_manifest(source_text), (7, 5, self.pixels.tobytes()))
        self.assertEqual(session.required_source_regions(source_text), {source["id"]: (0, 0, 7, 5)})
        self.assertEqual(session.required_source_regions(source_text, {"mip": 1, "quality": "preview",
                         "x": 1, "y": 1, "roi_width": 1, "roi_height": 1}), {source["id"]: (2, 2, 2, 2)})
        domain = "scene_linear_prophoto_d50"
        def operation(number, kind, parameters, inputs):
            return dict(id=uid(number), type=kind, schema_version=1, processing_version=2,
                        enabled=True, input_domain=domain, output_domain=domain,
                        parameters=parameters, inputs=inputs, masks={}, blend_mode="normal", opacity=1)
        base = operation(1, "rawengine.exposure", {"stops": 1}, {"image": source["id"]})
        layer = operation(2, "rawengine.exposure", {"stops": -1}, {"image": source["id"]})
        mix = operation(3, "rawengine.linear_mix", {"amount": 0.25}, {"base": base["id"], "layer": layer["id"]})
        document.update(operations=[mix, layer, base], output=mix["id"])
        for mip in (0, 1, 2):
            request = {"mip": mip, "quality": "final" if mip == 0 else "preview", "tile_size": 2}
            result = session.render_manifest(json.dumps(document), request)
            native = array.array("f"); native.frombytes(session.render_manifest(source_text, request)[2])
            actual = array.array("f"); actual.frombytes(result[2])
            # Independent double-precision oracle allows float32 intermediates.
            for value, original in zip(actual, native):
                self.assertAlmostEqual(value, 1.625 * original, delta=2e-7 * max(1, abs(value)))
            self.assertEqual(session.submit_manifest(json.dumps(document), request).result(timeout=5), result)
            self.assertEqual(session.render_manifest(json.dumps(document), {**request, "tile_size": 1}), result)
        mix["enabled"] = False
        document["operations"] = [mix, layer, base]
        disabled = session.render_manifest(json.dumps(document))
        self.assertEqual(disabled[2], array.array("f", (2 * v for v in self.pixels)).tobytes())
        empty = session.submit_manifest(source_text, {"roi_width": 0})
        self.assertEqual(empty.result(timeout=5), (0, 5, b""))
        self.assertEqual(empty.progress(), {"completed_tiles": 0, "total_tiles": 0})
        session.close()

    def test_invalid_manifests_requests_and_binding_recovery(self):
        session = self.session()
        document = self.graph(session)
        valid = json.dumps(document)
        mutations = [
            lambda d: d.update(format_version=99),
            lambda d: d.update(processing_version=99),
            lambda d: d["sources"][0].update(content_sha256="0" * 64),
            lambda d: d["sources"][0].update(working_space="linear_rec2020_d65"),
            lambda d: d["sources"][0].update(id=uid(99)),
            lambda d: d["sources"].append({**d["sources"][0], "id": uid(99)}),
            lambda d: d["operations"][0].update(schema_version=99),
            lambda d: d["operations"][0].update(processing_version=1),
            lambda d: d["operations"][0].update(input_domain="camera_linear"),
            lambda d: d["operations"][0].update(type="example.unknown"),
            lambda d: d["operations"][0].update(inputs={"image": uid(99)}),
            lambda d: d["operations"][0].update(inputs={"image": d["output"]}),
            lambda d: d["operations"][0].update(opacity=0.5),
            lambda d: d["operations"][0].update(masks={"mask": d["sources"][0]["id"]}),
        ]
        invalid = ["", "{", valid + "\x00", '{"format_version":2,"format_version":2}']
        for mutate in mutations:
            bad = copy.deepcopy(document); mutate(bad); invalid.append(json.dumps(bad))
        icc = copy.deepcopy(document)
        output = next(op for op in icc["operations"] if op["id"] == icc["output"])
        output.update(type="rawengine.icc_display", output_domain="display_encoded_icc")
        icc["output_profile"] = dict(profile_sha256="a5" * 32, intent="relative_colorimetric",
                                     black_point_compensation=True, engine="LittleCMS", engine_version="2.19.1")
        invalid.append(json.dumps(icc))
        for bad in invalid:
            for call in (lambda: session.render_manifest(bad), lambda: session.submit_manifest(bad),
                         lambda: session.submit_manifest_latest("view", bad)):
                with self.assertRaises(ValueError): call()
        for options in ({"exposure_stops": 1}, {"crop": None}, {"output_mode": "legacy"},
                        {"typo": 1}, {"x\x00": 1}, {"mip": 3}, {"tile_size": 0},
                        {"mip": 1, "quality": "final"}, {"x": 1000}, {"roi_width": 8}):
            for call in (lambda: session.render_manifest(valid, options),
                         lambda: session.submit_manifest(valid, options),
                         lambda: session.submit_manifest_latest("view", valid, options)):
                with self.assertRaises(ValueError): call()
        for bad in (None, b"{}", {}, 1):
            with self.assertRaises(TypeError): session.render_manifest(bad)
            with self.assertRaises(TypeError): session.submit_manifest(bad)
        with self.assertRaises(TypeError): session.render_manifest(valid, [])
        with self.assertRaises(TypeError): session.render_manifest(valid, {1: 0})
        with self.assertRaises(ValueError): session.submit_manifest(valid, priority="urgent")
        with self.assertRaises(ValueError): session.submit_manifest_latest("", valid)
        changed_pixels = array.array("f", self.pixels); changed_pixels[0] += 0.125
        changed = raw.RasterSession(changed_pixels, 7, 5, "prophoto-d50")
        with self.assertRaises(ValueError): changed.render_manifest(valid)
        changed.close()
        legacy = session.export_manifest()
        with self.assertRaises(ValueError):
            session.render_manifest(legacy, {"mip": 1, "quality": "preview"})
        with self.assertRaises(ValueError):
            session.submit_manifest(legacy, {"mip": 1, "quality": "preview"})
        self.assertEqual(session.submit_manifest(valid).result(timeout=5), session.render({"output_mode": "srgb-preview"}))
        session.close()

    def test_supersession_validation_and_owned_job_lifetimes(self):
        pixels = array.array("f", [0.3]) * (1024 * 1024 * 3)
        session = raw.RasterSession(pixels, 1024, 1024, "prophoto-d50", cache_bytes=0, max_pending=1)
        text = session.export_manifest({"output_mode": "srgb-preview"})
        busy = session.submit_manifest(text, {"tile_size": 1}, priority="background")
        deadline = time.monotonic() + 5
        while busy.progress()["completed_tiles"] == 0 and not busy.done():
            if time.monotonic() > deadline:
                busy.cancel(); self.fail("manifest worker did not begin")
            time.sleep(0.001)
        self.assertFalse(busy.done())
        try:
            old = session.submit_latest("view", {"roi_width": 2, "roi_height": 2})
            request = {"roi_width": 3, "roi_height": 2}
            latest = session.submit_manifest_latest("view", text, request)
            with self.assertRaises(raw.RenderCancelled): old.result(timeout=5)
            for invalid, opts in (("{", {}), (text, {"x": 2000}), (text, {"exposure_stops": 1})):
                with self.assertRaises(ValueError): session.submit_manifest_latest("view", invalid, opts)
            with self.assertRaises(RuntimeError): session.submit_manifest(text, request)
        finally:
            busy.cancel()
        with self.assertRaises(raw.RenderCancelled): busy.result(timeout=5)
        expected = session.render_manifest(text, request)
        self.assertEqual(latest.result(timeout=5), expected)
        # Cross-API groups also supersede a running manifest job.
        running = session.submit_manifest_latest("running", text, {"tile_size": 1})
        replacement = session.submit_latest("running", {"roi_width": 1, "roi_height": 1})
        with self.assertRaises(raw.RenderCancelled): running.result(timeout=5)
        replacement.result(timeout=5)
        session.close()
        self.assertEqual(latest.result(timeout=0), expected)
        session = self.session()
        text = session.export_manifest({"output_mode": "srgb-preview"})
        expected = session.render_manifest(text)
        job = session.submit_manifest(text)
        del text, session
        gc.collect()
        self.assertEqual(job.result(timeout=5), expected)

    def test_neighborhood_graph_and_concurrent_requests(self):
        session = self.session()
        document = self.graph(session)
        source = document["sources"][0]
        blur = dict(id=uid(10), type="rawengine.box_blur", schema_version=1, processing_version=2,
                    enabled=True, input_domain="scene_linear_prophoto_d50", output_domain="scene_linear_prophoto_d50",
                    parameters={"radius": 1}, inputs={"image": source["id"]}, masks={}, blend_mode="normal", opacity=1)
        document.update(operations=[blur], output=blur["id"])
        text = json.dumps(document)
        for mip in (0, 1, 2):
            source_doc = {**document, "operations": [], "output": source["id"]}
            request = {"mip": mip, "quality": "preview" if mip else "final", "tile_size": 2}
            w, h, data = session.render_manifest(json.dumps(source_doc), request)
            pixels = array.array("f"); pixels.frombytes(data)
            expected = array.array("f")
            for y in range(h):
                for x in range(w):
                    indices = [yy * w + xx for yy in range(max(0, y-1), min(h, y+2))
                               for xx in range(max(0, x-1), min(w, x+2))]
                    for c in range(3):
                        expected.append(sum(pixels[3*i+c] for i in indices) / len(indices))
            result = (w, h, expected.tobytes())
            with ThreadPoolExecutor(max_workers=3) as pool:
                calls = [pool.submit(session.render_manifest, text, {**request, "tile_size": size})
                         for size in (1, 2, 3)]
                self.assertEqual([call.result(timeout=5) for call in calls], [result] * 3)
            self.assertEqual(session.submit_manifest(text, request).result(timeout=5), result)
        session.close()

    def test_close_and_source_info_copy(self):
        session = self.session()
        text = session.export_manifest()
        info = session.source_info()
        session.close()
        for call in (lambda: session.export_manifest(), lambda: session.render_manifest(text),
                     lambda: session.submit_manifest(text),
                     lambda: session.submit_manifest_latest("view", text)):
            with self.assertRaisesRegex(RuntimeError, "closed"): call()
        self.assertEqual(session.source_info(), info)


if __name__ == "__main__":
    unittest.main()
