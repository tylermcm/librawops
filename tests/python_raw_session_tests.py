"""Owned decoded-Bayer recipes, manifests, jobs, history and sensor coordinates."""
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
import python_raw_preview_tests as oracle


class RawSessionTests(unittest.TestCase):
    metadata_fields = {"row_stride_samples", "pattern", "cfa_phase_x", "cfa_phase_y",
                       "active_x", "active_y", "active_width", "active_height",
                       "black_levels", "white_levels"}

    def setUp(self):
        self.reference = oracle.RawPreviewTests("test_independent_reference_patterns_phases_and_edges")
        self.samples = self.reference.samples()
        self.options = self.reference.settings((1, 3, 7, 5), 3, 3, "prophoto-d50")
        self.metadata = {k: v for k, v in self.options.items() if k in self.metadata_fields}
        self.recipe = {k: v for k, v in self.options.items() if k not in self.metadata_fields}

    def session(self, metadata=None, **kwargs):
        return raw.RawSession(self.samples, 11, 10, self.metadata if metadata is None else metadata, **kwargs)

    def expected(self, recipe):
        return raw.render(self.samples, 11, 10, {**self.metadata, **recipe})

    def request(self, mip):
        return {"mip": mip, "quality": "preview" if mip else "final", "tile_size": 2}

    def assert_pixels(self, actual, expected):
        self.assertEqual(actual[:2], expected[:2])
        for a, b in zip(oracle.values(actual), oracle.values(expected)):
            self.assertAlmostEqual(a, b, delta=1e-6)

    def test_independent_reference_native_preview_manifest_and_jobs(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            for pattern in range(4):
                for phase in range(4):
                    options = self.reference.settings((1, 3, 7, 5), pattern, phase, space)
                    metadata = {k: v for k, v in options.items() if k in self.metadata_fields}
                    recipe = {k: v for k, v in options.items() if k not in self.metadata_fields}
                    session = self.session(metadata, workers=2)
                    text = session.export_manifest(recipe)
                    saved = json.loads(text)
                    self.assertEqual(saved["sources"][0]["kind"], "decoded_bayer_u16")
                    self.assertNotIn("working_space", saved["sources"][0])
                    self.assertEqual(text, session.export_manifest({**recipe, "mip": 2, "tile_size": 1}))
                    native = self.reference.native_reference(self.samples, (1, 3, 7, 5), pattern, phase, space)
                    raster_options = {"working_space": space, "output_mode": "srgb-preview", "tone_shoulder": 0.4, "tone_gamma": 1.15}
                    for mip in (0, 1, 2):
                        request = self.request(mip)
                        combined = {**recipe, **request}
                        expected = raw.render(self.samples, 11, 10, {**metadata, **combined})
                        actual = session.render(combined)
                        self.assertEqual(actual, expected)
                        pixels, w, h = self.reference.reduced(native, 7, 5, mip)
                        self.assert_pixels(actual, raw.render_raster(pixels, w, h, raster_options))
                        self.assertEqual(session.render_manifest(text, request), actual)
                        self.assertEqual(session.render_manifest(text, {**request, "tile_size": 1}), actual)
                        job = session.submit(combined)
                        self.assertIsInstance(job, raw.RenderJob)
                        with ThreadPoolExecutor(max_workers=2) as pool:
                            self.assertEqual(list(pool.map(lambda _: job.result(timeout=5), range(2))), [actual] * 2)
                        self.assertEqual(job.result(timeout=0), actual)
                        self.assertTrue(job.done())
                        total = ((w + 1) // 2) * ((h + 1) // 2)
                        self.assertEqual(job.progress(), {"completed_tiles": total, "total_tiles": total})
                        self.assertEqual(session.submit_manifest(text, request).result(timeout=5), actual)
                        self.assertEqual(session.submit_manifest_latest("view", text, request).result(timeout=5), actual)
                        x, y = (w - 1, h - 1) if mip else (7, 7)
                        roi = {**request, "x": x, "y": y, "roi_width": 1, "roi_height": 1}
                        self.assertEqual(oracle.values(session.render({**recipe, **roi})), oracle.values(actual)[-3:])
                        self.assertEqual(session.render_manifest(text, roi), session.render({**recipe, **roi}))
                        self.assertEqual(session.submit_latest("roi", {**recipe, **roi}).result(timeout=5), session.render_manifest(text, roi))
                    session.close()

    def test_metadata_ownership_source_identity_and_empty_defaults(self):
        metadata = copy.deepcopy(self.metadata)
        session = self.session(metadata)
        expected = session.render(self.recipe)
        preview = {**self.recipe, **self.request(1)}
        expected_preview = session.render(preview)
        for space in ("prophoto-d50", "rec2020-d65"):
            legacy = {**self.recipe, "output_mode": "legacy", "working_space": space}
            self.assertEqual(session.render(legacy), self.expected(legacy))
            self.assertEqual(session.render_manifest(session.export_manifest(legacy)), self.expected(legacy))
        info = session.source_info()
        self.assertEqual((info["width"], info["height"], info["row_stride_samples"]), (11, 10, 14))
        self.assertEqual(info["active_area"], (1, 3, 7, 5))
        self.assertEqual(info["black_levels"], self.reference.black)
        self.assertEqual(info["white_levels"], self.reference.white)
        self.assertEqual((info["pattern"], info["cfa_phase_x"], info["cfa_phase_y"]), (3, 1, 1))
        record = json.loads(session.export_manifest(self.recipe))["sources"][0]
        self.assertEqual(record, {k: info[k] for k in ("id", "kind", "content_sha256")})
        metadata["active_x"] = 0
        info["content_sha256"] = "0" * 64
        original_samples = array.array("H", self.samples)
        self.samples[:] = array.array("H", [0]) * len(self.samples)
        self.assertEqual(session.render(self.recipe), expected)
        self.assertEqual(session.render(preview), expected_preview)
        self.assertNotEqual(session.source_info(), info)
        twin = raw.RawSession(original_samples, 11, 10, self.metadata)
        self.assertEqual(twin.render_manifest(session.export_manifest(self.recipe)), expected)
        for mip in (0, 1, 2):
            request = self.request(mip)
            h = (5 + (1 << mip) - 1) // (1 << mip)
            empty = {**self.recipe, **request, "roi_width": 0}
            self.assertEqual(session.render(empty), (0, h, b""))
            job = session.submit(empty)
            self.assertEqual(job.result(timeout=5), (0, h, b""))
            self.assertEqual(job.progress(), {"completed_tiles": 0, "total_tiles": 0})
        session.close()
        self.assertEqual(session.source_info(), twin.source_info())
        twin.close()
        packed = array.array("H", [1000]) * 35
        simple = raw.RawSession(packed, 7, 5)
        self.assertEqual(simple.source_info()["active_area"], (0, 0, 7, 5))
        self.assertEqual(simple.source_info()["row_stride_samples"], 7)
        self.assertEqual(simple.render(), raw.render(packed, 7, 5))
        self.assertEqual(simple.render({"red_gain": 2, "exposure_stops": 1}), raw.render(packed, 7, 5, {"red_gain": 2, "exposure_stops": 1}))
        self.assertEqual(simple.render(), raw.render(packed, 7, 5))  # Recipes reset each call.
        simple.close()

    def test_cache_revisions_budgets_and_concurrent_clear(self):
        session = self.session(workers=2)
        preview = {**self.recipe, **self.request(1), "tile_size": 50}
        expected = session.render(preview)
        cold = session.cache_stats()
        self.assertGreater(cold["entries"], 0)
        self.assertEqual(session.render(preview), expected)
        warm = session.cache_stats()
        self.assertGreater(warm["hits"], cold["hits"])
        self.assertEqual(warm["misses"], cold["misses"])
        changed = {**preview, "tone_shoulder": 0.8}
        self.assertEqual(session.render(changed), self.expected(changed))
        revised = session.cache_stats()
        self.assertEqual(revised["hits"] - warm["hits"], 1)
        self.assertEqual(revised["misses"] - warm["misses"], 2)
        for revision in ({"red_gain": 2}, {"exposure_stops": 1.25}, {"working_space": "rec2020-d65"},
                         {"camera_to_xyz_d50": (0.7, *self.reference.matrix[1:])}):
            opts = {**preview, **revision}
            self.assertEqual(session.render(opts), self.expected(opts))
            self.assertNotEqual(session.render(opts), expected)
        def render(index):
            opts = {**preview, "red_gain": 1 + index / 20, "tone_shoulder": 0.4 + index / 20}
            result = session.render(opts)
            self.assertEqual(result, self.expected(opts))
            return result
        with ThreadPoolExecutor(max_workers=4) as pool:
            futures = [pool.submit(render, i) for i in range(12)]
            for _ in range(12): session.clear_cache()
            for future in futures: future.result()
        for budget in (0, 100, 4096):
            bounded = self.session(cache_bytes=budget)
            self.assertEqual(bounded.render(preview), expected)
            self.assertLessEqual(bounded.cache_stats()["used_bytes"], budget)
            if budget < 1000: self.assertEqual(bounded.cache_stats()["entries"], 0)
            bounded.close()
        session.close()

    def test_saved_manifest_sensor_footprints_and_binding(self):
        session = self.session()
        text = session.export_manifest(self.recipe)
        record = json.loads(text)["sources"][0]
        source = record["id"]
        self.assertEqual(session.required_source_regions(text), {source: (1, 3, 7, 5)})
        for mip in (1, 2):
            request = {**self.request(mip), "x": 1, "y": 0, "roi_width": 1, "roi_height": 1}
            footprint = (2, 3, 4, 3) if mip == 1 else (4, 3, 4, 5)
            self.assertEqual(session.required_source_regions(text, request), {source: footprint})
        native_roi = {"x": 3, "y": 4, "roi_width": 2, "roi_height": 2}
        self.assertEqual(session.required_source_regions(text, native_roi), {source: (2, 3, 4, 4)})
        # Source-only RAW output retains sensor origin and native demosaic values.
        camera = json.loads(text); camera["operations"] = []; camera["output"] = source
        camera_text = json.dumps(camera)
        result = session.render_manifest(camera_text)
        self.assertEqual(result[:2], (7, 5))
        self.assertEqual(session.required_source_regions(camera_text), {source: (1, 3, 7, 5)})
        with self.assertRaises(ValueError): session.render_manifest(camera_text, self.request(1))
        altered = array.array("H", self.samples); altered[3 * 14 + 1] ^= 1
        other = raw.RawSession(altered, 11, 10, self.metadata)
        with self.assertRaises(ValueError): other.render_manifest(text)
        with self.assertRaises(ValueError): other.restore_history(session.history(text).save())
        outside = array.array("H", self.samples); outside[0] ^= 1; outside[13] ^= 1
        equivalent = raw.RawSession(outside, 11, 10, self.metadata)
        self.assertEqual(equivalent.render_manifest(text), session.render(self.recipe))
        for change in ({"pattern": 0}, {"cfa_phase_x": 0}, {"active_x": 0}, {"black_level": 10, "black_levels": (10,) * 4}):
            changed = raw.RawSession(self.samples, 11, 10, {**self.metadata, **change})
            with self.assertRaises(ValueError): changed.render_manifest(text)
            changed.close()
        for bad in ("{", json.dumps({**json.loads(text), "output": "00000000-0000-0000-0000-000000000099"})):
            with self.assertRaises(ValueError): session.render_manifest(bad)
        for opts in ({"red_gain": 2}, {"active_x": 0}, {"crop": (1, 1, 3, 3)}):
            with self.assertRaises(ValueError): session.render_manifest(text, opts)
        self.assertEqual(session.render_manifest(text), session.render(self.recipe))
        equivalent.close(); other.close(); session.close()

    def test_history_replay_comparisons_retention_and_pinned_lifetimes(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            session = self.session()
            recipes = [{**self.recipe, "working_space": space}, {**self.recipe, "working_space": space, "red_gain": 2},
                       {**self.recipe, "working_space": space, "tone_shoulder": 0.8}]
            texts = [session.export_manifest(opts) for opts in recipes]
            history = session.history(texts[0], max_revisions=3)
            history.commit(texts[1]); history.commit(texts[2])
            for mip in (0, 1, 2):
                request = self.request(mip)
                expected = [session.render({**recipe, **request}) for recipe in recipes]
                for revision in (1, 2, 3):
                    self.assertEqual(history.render(request, revision=revision), expected[revision - 1])
                    self.assertEqual(history.submit(request, revision=revision).result(timeout=5), expected[revision - 1])
                self.assertEqual(history.compare(1, 3, request), (expected[0], expected[2]))
                self.assertEqual(history.submit_latest("history", request, revision=2).result(timeout=5), expected[1])
            history.undo()
            saved = history.save()
            restored = session.restore_history(saved)
            self.assertEqual(restored.save(), saved)
            self.assertEqual(restored.stats(), history.stats())
            self.assertEqual(restored.redo(), 3)
            initial = history.snapshot(1)
            bad = json.loads(texts[1]); bad["sources"][0]["content_sha256"] = "0" * 64
            with self.assertRaises(ValueError): history.commit(json.dumps(bad))
            self.assertEqual(history.save(), saved)
            self.assertEqual(history.commit(initial), 4)
            with self.assertRaises(IndexError): history.render(revision=3)
            history.commit(initial)
            with self.assertRaises(IndexError): history.render(revision=1)
            request = self.request(2)
            expected = restored.render(request, revision=3)
            job = restored.submit(request, revision=3)
            session.close()
            self.assertEqual(restored.render(request, revision=3), expected)
            history.close()
            del session, restored
            gc.collect()
            self.assertEqual(job.result(timeout=5), expected)

    def busy(self, session):
        job = session.submit({"tile_size": 1}, priority="background")
        deadline = time.monotonic() + 5
        while not job.progress()["completed_tiles"] and not job.done():
            if time.monotonic() > deadline:
                job.cancel(); self.fail("RAW worker did not start")
            time.sleep(0.001)
        self.assertFalse(job.done())
        return job

    def test_saved_geometry_and_independent_history_extents(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            session = self.session()
            recipe = {**self.recipe, "working_space": space}
            initial = session.export_manifest(recipe)
            document = json.loads(initial)
            camera = next(op for op in document["operations"] if op["type"] == "rawengine.camera_to_working")
            domain = camera["output_domain"]
            def operation(number, kind, parameters, upstream):
                return dict(id=f"70000000-0000-0000-0000-{number:012d}", type=kind, schema_version=1,
                            processing_version=2, enabled=True, input_domain=domain, output_domain=domain,
                            parameters=parameters, inputs={"image": upstream}, masks={}, blend_mode="normal", opacity=1)
            crop = operation(1, "rawengine.crop", {"x": 3, "y": 4, "width": 3, "height": 2}, camera["id"])
            orient = operation(2, "rawengine.orientation", {"quarter_turns": 1, "flip_horizontal": True, "flip_vertical": False}, crop["id"])
            resize = operation(3, "rawengine.resize", {"width": 9, "height": 7, "filter": "area"}, orient["id"])
            next(op for op in document["operations"] if op["type"] == "rawengine.working_to_srgb")["inputs"]["image"] = resize["id"]
            document["operations"].extend((resize, crop, orient))
            saved = json.dumps(document)
            native = self.reference.native_reference(self.samples, (1, 3, 7, 5), 3, 3, space)
            raster_recipe = {"working_space": space, "output_mode": "srgb-preview", "tone_shoulder": 0.4,
                             "tone_gamma": 1.15, "crop": (2, 1, 3, 2), "rotate": 90, "flip_horizontal": True,
                             "resize": (9, 7), "resize_filter": "area"}
            history = session.history(initial)
            history.commit(saved)
            for mip in (0, 1, 2):
                request = self.request(mip)
                actual = session.render_manifest(saved, request)
                expected = raw.render_raster(native, 7, 5, {**raster_recipe, **request})
                self.assert_pixels(actual, expected)
                self.assertEqual(session.submit_manifest(saved, request).result(timeout=5), actual)
                self.assertEqual(history.render(request, revision=2), actual)
                self.assertEqual(history.compare(1, 2, request), (session.render({**recipe, **request}), actual))
                source = document["sources"][0]["id"]
                self.assertEqual(session.required_source_regions(saved, request), {source: (2, 3, 5, 4)})
                w, h = actual[:2]
                roi = {**request, "x": w - 1, "y": h - 1, "roi_width": 1, "roi_height": 1}
                self.assertEqual(oracle.values(session.render_manifest(saved, roi)), oracle.values(actual)[-3:])
            restored = session.restore_history(history.save())
            self.assertEqual(restored.render(self.request(1)), history.render(self.request(1)))
            session.close(); history.close(); restored.close()

    def test_queued_job_source_pinning_and_dropped_running_job(self):
        samples = array.array("H", [2000]) * (1024 * 1024)
        session = raw.RawSession(samples, 1024, 1024, cache_bytes=0)
        busy = self.busy(session)
        options = {**self.recipe, **self.request(2), "roi_width": 1, "roi_height": 1}
        expected = raw.render(samples, 1024, 1024, options)
        queued = session.submit(options)
        self.assertEqual(queued.progress()["completed_tiles"], 0)
        del session
        samples[:] = array.array("H", [0]) * len(samples)
        gc.collect()
        busy.cancel()
        self.assert_cancelled(busy)
        self.assertEqual(queued.result(timeout=5), expected)
        # Dropping a running job cancels it and lets retained work proceed.
        session = raw.RawSession(samples, 1024, 1024, cache_bytes=0)
        discarded = self.busy(session)
        queued = session.submit(options)
        del discarded
        gc.collect()
        self.assertEqual(queued.result(timeout=5), session.render(options))
        session.close()

    def assert_cancelled(self, job):
        with self.assertRaises(raw.RenderCancelled): job.result(timeout=5)
        self.assertTrue(job.done())

    def test_queue_latest_rejection_timeout_cancellation_and_close(self):
        samples = array.array("H", [2000]) * (1024 * 1024)
        session = raw.RawSession(samples, 1024, 1024, cache_bytes=0, max_pending=2)
        busy = self.busy(session)
        text = session.export_manifest(self.recipe)
        roi = {**self.request(2), "roi_width": 1, "roi_height": 1}
        old = session.submit_manifest_latest("view", text, roi)
        other = session.submit_latest("other", {**self.recipe, **roi})
        try:
            with self.assertRaises(TimeoutError): busy.result(timeout=0)
            with self.assertRaises(RuntimeError): session.submit({**self.recipe, **roi})
            latest = session.submit_latest("view", {**self.recipe, **roi, "red_gain": 2})
            self.assert_cancelled(old)
            self.assertEqual(old.progress()["completed_tiles"], 0)
            for invalid in ({"camera_to_xyz_d50": [0] * 9}, {"mip": 3}, {"x": 300}, {"tile_size": 0},
                            {"black_level": 0}, {"output_mode": "legacy"}, {"quality": "final"}):
                with self.assertRaises(ValueError): session.submit_latest("view", {**self.recipe, **roi, **invalid})
            with self.assertRaises(ValueError): session.submit_manifest_latest("view", "{", roi)
            with self.assertRaises(ValueError): session.submit_manifest_latest("view", text, {**roi, "x": 300})
        finally:
            busy.cancel()
        self.assert_cancelled(busy)
        self.assertEqual(latest.result(timeout=5), session.render({**self.recipe, **roi, "red_gain": 2}))
        self.assertEqual(other.result(timeout=5), session.render({**self.recipe, **roi}))
        busy = self.busy(session)
        queued = session.submit_manifest(text, roi)
        session.close(); session.close()
        for job in (busy, queued): self.assert_cancelled(job)
        for call in (lambda: session.render(), lambda: session.submit(), lambda: session.export_manifest(),
                     lambda: session.render_manifest(text), lambda: session.submit_manifest(text),
                     lambda: session.history(text), lambda: session.required_source_regions(text)):
            with self.assertRaisesRegex(RuntimeError, "closed"): call()
        self.assertEqual(session.source_info()["active_area"], (0, 0, 1024, 1024))
        session.clear_cache()
        self.assertEqual(session.cache_stats()["used_bytes"], 0)

    def test_session_job_deletion_and_validation_recovery(self):
        session = self.session()
        preview = {**self.recipe, **self.request(1)}
        expected = session.render(preview)
        job = session.submit(preview)
        del session
        gc.collect()
        self.assertEqual(job.result(timeout=5), expected)
        retained = job.result(timeout=0)
        del job
        gc.collect()
        self.assertEqual(retained, expected)
        session = self.session()
        for invalid in ({"black_level": 0}, {"row_stride_samples": 14}, {"pattern": 0}, {"active_x": 1},
                        {"cache_bytes": 0}, {"workers": 1}, {"crop": (1, 1, 3, 3)}, {"rotate": 90},
                        {"resize": (2, 2)}, {"misspelled": 1}, {"working_space": "wrong"},
                        {"red_gain": 0}, {"tone_gamma": 0}, {"mip": 0, "quality": "preview"},
                        {"mip": 1, "quality": "final"}):
            for call in (lambda: session.render({**preview, **invalid}), lambda: session.submit({**preview, **invalid})):
                with self.assertRaises((ValueError, TypeError)): call()
        for options in ({"mip": 1, "quality": "preview"}, {"working_space": "prophoto-d50"}):
            with self.assertRaises(ValueError): session.render(options)
        for metadata in ({"pattern": 4}, {"cfa_phase_x": 2}, {"row_stride_samples": 10},
                         {"active_x": 10}, {"white_levels": (0,) * 4}, {"black_levels": (0,) * 3},
                         {"camera_to_xyz_d50": self.reference.matrix}, {"width": 11}, {"bad": 1}):
            with self.assertRaises(ValueError): self.session({**self.metadata, **metadata})
        for metadata in ([], {**self.metadata, "pattern": True}, {**self.metadata, "active_x": 1.5}, {1: 0}):
            with self.assertRaises(TypeError): self.session(metadata)
        for kwargs in ({"workers": 0}, {"workers": 65}, {"max_pending": 0}):
            with self.assertRaises(ValueError): self.session(**kwargs)
        for kwargs in ({"cache_bytes": -1}, {"workers": -1}):
            with self.assertRaises(OverflowError): self.session(**kwargs)
        with self.assertRaises(ValueError): raw.RawSession(array.array("H", [0]), 11, 10)
        with self.assertRaises(TypeError): session.render([])
        with self.assertRaises(TypeError): session.render({1: 0})
        self.assertEqual(session.render(preview), expected)
        self.assertEqual(session.render(), self.expected({}))
        session.close()


if __name__ == "__main__":
    unittest.main()
