"""Immutable revision navigation, budgets, pinned sources and Python jobs."""
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


class HistoryTests(unittest.TestCase):
    def setUp(self):
        self.pixels = array.array("f")
        for y in range(5):
            for x in range(7):
                self.pixels.extend((x/8-0.25, -y/4, (x+y)/3))

    def session(self, space="prophoto-d50", **kwargs):
        return raw.RasterSession(self.pixels, 7, 5, space, **kwargs)

    def test_geometry_navigation_comparison_and_replay(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            session = self.session(space)
            options = {"output_mode": "srgb-preview"}
            initial = session.export_manifest(options)
            history = session.history(initial)
            self.assertIsInstance(history, raw.EditHistory)
            first = history.snapshot()
            self.assertEqual(history.stats()["current_id"], 1)
            with self.assertRaises(IndexError): history.undo()
            two_opts = {**options, "exposure_stops": 1}
            two = history.commit(session.export_manifest(two_opts))
            three_opts = {**two_opts, "crop": (1, 1, 5, 3), "rotate": 90, "flip_vertical": True}
            three = history.commit(session.export_manifest(three_opts))
            self.assertEqual((two, three), (2, 3))
            for mip in (0, 1, 2):
                request = {"mip": mip, "quality": "preview" if mip else "final", "tile_size": 2}
                expected = [session.render({**opts, **request}) for opts in (options, two_opts, three_opts)]
                for revision, result in enumerate(expected, 1):
                    self.assertEqual(history.render(request, revision=revision), result)
                    self.assertEqual(history.submit(request, revision=revision).result(timeout=5), result)
                self.assertEqual(history.compare(1, 3, request), (expected[0], expected[2]))
                self.assertEqual(history.compare(2, 2, request), (expected[1], expected[1]))
            self.assertEqual(history.undo(), 2)
            self.assertEqual(history.render(), session.render(two_opts))
            saved = history.save()
            restored = session.restore_history(saved)
            self.assertEqual(restored.save(), saved)
            self.assertEqual(restored.stats(), history.stats())
            self.assertEqual(restored.redo(), 3)
            self.assertEqual(history.snapshot(1), first)
            self.assertEqual(history.stats()["current_id"], 2)
            branched = history.commit(initial)
            self.assertEqual(branched, 4)
            self.assertEqual(history.stats()["revision_ids"], [1, 2, 4])
            with self.assertRaises(IndexError): history.snapshot(3)
            with self.assertRaises(IndexError): history.redo()
            history.close(); restored.close(); session.close()

    def test_count_byte_budgets_and_failed_commit_recovery(self):
        session = self.session()
        initial = session.export_manifest({"output_mode": "srgb-preview"})
        history = session.history(initial, max_revisions=2)
        history.commit(initial); history.commit(initial)
        self.assertEqual(history.stats()["revision_ids"], [2, 3])
        with self.assertRaises(IndexError): history.render(revision=1)
        history.undo()
        before = history.save()
        bad = json.loads(initial); bad["operations"][0]["type"] = "example.unknown"
        for text in ("{", json.dumps(bad)):
            with self.assertRaises(ValueError): history.commit(text)
            self.assertEqual(history.save(), before)
        self.assertEqual(history.redo(), 3)
        history.undo()
        self.assertEqual(history.commit(initial), 4)
        self.assertEqual(history.stats()["revision_ids"], [2, 4])
        self.assertEqual(history.commit(initial), 5)
        self.assertEqual(history.stats()["revision_ids"], [4, 5])
        history.close()
        size = len(initial.encode("utf-8"))
        bounded = session.history(initial, max_revisions=100, max_manifest_bytes=2*size)
        bounded.commit(initial); bounded.commit(initial)
        self.assertEqual(bounded.stats()["manifest_bytes"], 2*size)
        self.assertEqual(bounded.stats()["revision_ids"], [2, 3])
        tight = session.history(initial, max_manifest_bytes=size)
        oversized = session.export_manifest({"output_mode": "srgb-preview", "crop": (1, 1, 5, 3)})
        with self.assertRaises(ValueError): tight.commit(oversized)
        self.assertEqual(tight.commit(initial), 2)
        self.assertEqual(tight.stats()["revision_ids"], [2])
        for limits in ({"max_revisions": 0}, {"max_revisions": 100001}, {"max_manifest_bytes": 0},
                       {"max_manifest_bytes": size-1}, {"max_manifest_bytes": 16777217}):
            with self.assertRaises(ValueError): session.history(initial, **limits)
        bounded.close(); tight.close(); session.close()

    def test_cache_reuse_and_independent_histories(self):
        session = self.session()
        initial = session.export_manifest({"output_mode": "srgb-preview"})
        history = session.history(initial)
        independent = session.history(initial)
        request = {"mip": 1, "quality": "preview"}
        session.clear_cache()
        original = history.render(request)
        before = session.cache_stats()
        self.assertEqual(history.render(request), original)
        warm = session.cache_stats()
        self.assertEqual(warm["misses"], before["misses"])
        self.assertGreater(warm["hits"], before["hits"])
        changed = json.loads(initial)
        next(op for op in changed["operations"] if op["type"] == "rawengine.tone_curve")["parameters"]["shoulder"] = 1.5
        history.commit(json.dumps(changed))
        current = history.render(request)
        after = session.cache_stats()
        self.assertEqual(after["hits"]-warm["hits"], 1)
        self.assertEqual(after["misses"]-warm["misses"], 2)
        history.undo()
        self.assertEqual(history.render(request), original)
        history.redo()
        self.assertEqual(history.render(request), current)
        self.assertEqual(independent.stats()["revision_ids"], [1])
        self.assertEqual(independent.render(request), original)
        session.close()
        self.assertEqual(history.render(request), current)  # Independent pinned ownership.
        history.close(); independent.close()

    def test_pinned_multi_source_and_restore_binding(self):
        a, b, mix = (f"50000000-0000-0000-0000-{n:012d}" for n in (1, 2, 3))
        base = dict(rgb=self.pixels, width=7, height=5, working_space="prophoto-d50")
        layer = {**base, "rgb": array.array("f", [0.5])*105}
        session = raw.RasterGraphSession({a: base, b: layer})
        document = json.loads(session.export_manifest(a))
        document.update(output=mix, operations=[dict(id=mix, type="rawengine.linear_mix", schema_version=1,
            processing_version=2, enabled=True, input_domain="scene_linear_prophoto_d50", output_domain="scene_linear_prophoto_d50",
            inputs={"base": a, "layer": b}, parameters={"amount": 0.5}, masks={}, blend_mode="normal", opacity=1)])
        history = session.history(json.dumps(document))
        expected = history.render()
        saved = history.save()
        session.replace_source(b, {**layer, "rgb": array.array("f", [1.5])*105})
        document["sources"] = json.loads(session.export_manifest(a))["sources"]
        with self.assertRaises(ValueError): history.commit(json.dumps(document))
        with self.assertRaises(ValueError): session.restore_history(saved)
        self.assertEqual(history.render(), expected)
        equivalent = raw.RasterGraphSession({a: base, b: layer})
        restored = equivalent.restore_history(saved)
        self.assertEqual(restored.render(), expected)
        session.close(); equivalent.close()
        self.assertEqual(history.submit().result(timeout=5), expected)
        history.close(); restored.close()

    def test_restore_validation_ids_and_request_failures(self):
        session = self.session()
        history = session.history(session.export_manifest({"output_mode": "srgb-preview"}))
        history.commit(history.snapshot()); history.undo()
        saved = json.loads(history.save())
        mutations = [lambda d: d.update(history_format_version=99), lambda d: d.update(current_id=99),
                     lambda d: d.update(next_id=2), lambda d: d.update(max_revisions=1),
                     lambda d: d.update(max_manifest_bytes=1), lambda d: d.update(unknown=1),
                     lambda d: d["revisions"][1].update(id=1), lambda d: d["revisions"][1].update(id=0),
                     lambda d: d["revisions"][1].update(manifest="{"),
                     lambda d: d["revisions"][1].update(unknown=True)]
        for mutate in mutations:
            bad = copy.deepcopy(saved); mutate(bad)
            with self.assertRaises(ValueError): session.restore_history(json.dumps(bad))
        for value in (True, "one", 1.5):
            with self.assertRaises(TypeError): history.render(revision=value)
        with self.assertRaises(ValueError): history.snapshot(0)
        with self.assertRaises(IndexError): history.snapshot(99)
        with self.assertRaises(TypeError): history.compare(None, 1)
        with self.assertRaises(TypeError): raw.EditHistory()
        for request in ({"exposure_stops": 1}, {"mip": 3}, {"x": 100}, {"tile_size": 0}):
            with self.assertRaises(ValueError): history.render(request)
            with self.assertRaises(ValueError): history.submit(request)
            with self.assertRaises(ValueError): history.compare(1, 2, request)
        history.close()
        before = history.stats()
        self.assertIsInstance(history.snapshot(), str)
        self.assertIsInstance(history.save(), str)
        for call in (lambda: history.commit(history.snapshot()), lambda: history.undo(), lambda: history.redo(),
                     lambda: history.render(), lambda: history.submit(), lambda: history.compare(1, 2)):
            with self.assertRaisesRegex(RuntimeError, "closed"): call()
        self.assertEqual(history.stats(), before)
        session.close()

    def test_jobs_survive_truncation_eviction_and_close(self):
        pixels = array.array("f", [0.25])*(1024*1024*3)
        session = raw.RasterSession(pixels, 1024, 1024, "prophoto-d50", cache_bytes=0, max_pending=2)
        document = json.loads(session.export_manifest())
        document.update(operations=[], output=document["sources"][0]["id"])
        initial = json.dumps(document)
        history = session.history(initial, max_revisions=2)
        busy = history.submit({"tile_size": 1}, priority="background")
        deadline = time.monotonic()+5
        while busy.progress()["completed_tiles"] == 0 and not busy.done():
            if time.monotonic()>deadline: busy.cancel(); self.fail("history worker did not start")
            time.sleep(0.001)
        request = {"roi_width": 1, "roi_height": 1}
        old = history.submit(request, revision=1)
        exposure = dict(id="50000000-0000-0000-0000-000000000002", type="rawengine.exposure", schema_version=1,
            processing_version=2, enabled=True, input_domain="scene_linear_prophoto_d50", output_domain="scene_linear_prophoto_d50",
            inputs={"image": document["output"]}, parameters={"stops": 1}, masks={}, blend_mode="normal", opacity=1)
        document.update(operations=[exposure], output=exposure["id"])
        history.commit(json.dumps(document))
        latest = history.submit_latest("view", request, revision=2)
        try:
            with self.assertRaises(IndexError): history.submit_latest("view", request, revision=99)
            with self.assertRaises(ValueError): history.submit_latest("view", {"tile_size": 0}, revision=2)
            with self.assertRaises(RuntimeError): history.submit(request)
            history.undo()
            history.commit(initial)  # ID 3 discards revision 2; its job is pinned.
            history.commit(initial)  # ID 4 evicts revision 1; its job is pinned.
            with self.assertRaises(IndexError): history.render(request, revision=2)
        finally: busy.cancel()
        with self.assertRaises(raw.RenderCancelled): busy.result(timeout=5)
        self.assertEqual(old.result(timeout=5)[2], array.array("f", [0.25]*3).tobytes())
        self.assertEqual(latest.result(timeout=5)[2], array.array("f", [0.5]*3).tobytes())
        self.assertEqual(latest.progress(), {"completed_tiles": 1, "total_tiles": 1})
        running = history.submit({"tile_size": 1})
        queued = history.submit(request)
        history.close()
        for job in (running, queued):
            with self.assertRaises(raw.RenderCancelled): job.result(timeout=5)
        session.close()

    def test_concurrent_commits_and_native_job_lifetimes(self):
        session = self.session()
        initial = session.export_manifest({"output_mode": "srgb-preview"})
        history = session.history(initial)
        expected = history.render()
        def commit(stops):
            document = json.loads(initial)
            next(op for op in document["operations"] if op["type"] == "rawengine.exposure")["parameters"]["stops"] = stops
            return history.commit(json.dumps(document))
        with ThreadPoolExecutor(max_workers=4) as pool:
            ids = list(pool.map(commit, [i/8 for i in range(12)]))
        self.assertEqual(sorted(ids), list(range(2, 14)))
        self.assertEqual(history.stats()["revision_ids"], list(range(1, 14)))
        self.assertEqual(history.render(revision=1), expected)
        job = history.submit(revision=1)
        del session, history
        gc.collect()
        with ThreadPoolExecutor(max_workers=2) as pool:
            self.assertEqual(list(pool.map(lambda _: job.result(timeout=5), range(2))), [expected, expected])


if __name__ == "__main__":
    unittest.main()
