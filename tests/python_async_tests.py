"""Asynchronous Python result, queue, cancellation and lifetime integration."""
import array
from concurrent.futures import ThreadPoolExecutor
import gc
import math
import sys
import time
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw


class AsyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.large_pixels = array.array("f", [0.3]) * (1024 * 1024 * 3)

    def setUp(self):
        self.pixels = array.array("f", [0.1, 0.5, 1.5]) * (7 * 5)
        self.preview = {"output_mode": "srgb-preview", "quality": "preview", "mip": 1, "tile_size": 2}

    def session(self, **kwargs):
        return raw.RasterSession(self.pixels, 7, 5, "prophoto-d50", **kwargs)

    def large_session(self, **kwargs):
        return raw.RasterSession(self.large_pixels, 1024, 1024, "prophoto-d50", cache_bytes=0, **kwargs)

    def busy_job(self, session, options=None):
        # One million tiny native tiles keep the worker occupied while testing
        # queue transitions. Cancel it as soon as the transition is verified.
        job = session.submit({"tile_size": 1, **(options or {})}, priority="background")
        deadline = time.monotonic() + 5
        while job.progress()["completed_tiles"] == 0 and not job.done():
            if time.monotonic() >= deadline:
                job.cancel()
                self.fail("worker never began its first tile")
            time.sleep(0.001)
        self.assertFalse(job.done())
        return job

    def assert_cancelled(self, job):
        with self.assertRaises(raw.RenderCancelled):
            job.result(timeout=5)
        self.assertTrue(job.done())

    def test_results_progress_and_repeatable_waiters(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            session = raw.RasterSession(self.pixels, 7, 5, space, workers=2)
            for mip in (0, 1, 2):
                options = {**self.preview, "mip": mip,
                           "quality": "final" if mip == 0 else "preview"}
                expected = session.render(options)
                job = session.submit(options)
                self.assertIsInstance(job, raw.RenderJob)
                with ThreadPoolExecutor(max_workers=3) as pool:
                    results = list(pool.map(lambda _: job.result(timeout=5), range(3)))
                self.assertEqual(results, [expected] * 3)
                self.assertTrue(job.done())
                w, h = expected[:2]
                total = ((w + 1) // 2) * ((h + 1) // 2)
                self.assertEqual(job.progress(), {"completed_tiles": total, "total_tiles": total})
                job.cancel()  # Completion is already settled; late cancel changes nothing.
                self.assertEqual(job.result(timeout=0), expected)
            empty = session.submit({**self.preview, "roi_width": 0})
            self.assertEqual(empty.result(timeout=5), (0, 3, b""))
            self.assertEqual(empty.progress(), {"completed_tiles": 0, "total_tiles": 0})
            session.close()

    def test_orientation_running_cancel_and_recovery(self):
        session = self.large_session()
        for mip in (0, 1, 2):
            job = self.busy_job(session, {"rotate": 90, "flip_horizontal": True,
                "output_mode": "srgb-preview", "mip": mip, "quality": "preview" if mip else "final"})
            self.assertEqual(job.progress()["total_tiles"], (1024 // (1 << mip)) ** 2)
            job.cancel()
            self.assert_cancelled(job)
            self.assertLess(job.progress()["completed_tiles"], job.progress()["total_tiles"])
        options = {"rotate": 270, "flip_vertical": True, "crop": (1, 3, 9, 7),
                   "mip": 1, "quality": "preview", "output_mode": "srgb-preview"}
        expected = session.render(options)
        self.assertEqual(session.submit_latest("oriented", options).result(timeout=5), expected)
        session.close()

    def test_resize_running_cancel_and_recovery(self):
        session = self.large_session()
        for mip in (0, 1, 2):
            scale = 1 << mip
            job = self.busy_job(session, {"resize": (1537 * scale - (scale - 1), 1025 * scale - (scale - 1)),
                "resize_filter": "area", "output_mode": "srgb-preview", "mip": mip,
                "quality": "preview" if mip else "final"})
            self.assertEqual(job.progress()["total_tiles"], 1537 * 1025)
            job.cancel()
            self.assert_cancelled(job)
            self.assertLess(job.progress()["completed_tiles"], job.progress()["total_tiles"])
        options = {"resize": (19, 13), "resize_filter": "area", "tile_size": 4,
                   "output_mode": "srgb-preview", "mip": 2, "quality": "preview"}
        expected = session.render(options)
        self.assertEqual(session.submit_latest("resized", options).result(timeout=5), expected)
        session.close()

    def test_timeout_running_cancel_and_recovery(self):
        session = self.large_session()
        job = self.busy_job(session)
        try:
            with self.assertRaises(TimeoutError):
                job.result(timeout=0)
            progress = job.progress()
            self.assertGreater(progress["completed_tiles"], 0)
            self.assertLess(progress["completed_tiles"], progress["total_tiles"])
        finally:
            job.cancel()
        self.assert_cancelled(job)
        self.assert_cancelled(job)  # Exception is repeatable, like successful results.
        self.assertLess(job.progress()["completed_tiles"], job.progress()["total_tiles"])
        expected = session.render(self.preview)
        self.assertEqual(session.submit(self.preview).result(timeout=5), expected)
        session.close()

    def test_queue_budget_latest_group_isolation_and_rejection(self):
        session = self.large_session(max_pending=2)
        busy = self.busy_job(session)
        try:
            obsolete = session.submit_latest("viewport", self.preview)
            other = session.submit_latest("other", {**self.preview, "tone_shoulder": 1.5})
            with self.assertRaises(RuntimeError):
                session.submit(self.preview)
            newest_options = {**self.preview, "tone_shoulder": 0.6,
                              "resize": (7, 5), "resize_filter": "area", "rotate": 90,
                              "flip_vertical": True, "mip": 1, "quality": "preview"}
            newest = session.submit_latest("viewport", newest_options)
            self.assert_cancelled(obsolete)
            self.assertEqual(obsolete.progress()["completed_tiles"], 0)
            with self.assertRaises(ValueError):
                session.submit_latest("viewport", {**self.preview, "mip": 3})
            with self.assertRaises(ValueError):
                session.submit_latest("viewport", {**self.preview, "x": 1000})
            with self.assertRaises(ValueError):
                session.submit_latest("viewport", {**self.preview, "crop": (1024, 0, 1, 1)})
            with self.assertRaises(ValueError):
                session.submit_latest("viewport", {**newest_options, "resize_filter": "magic"})
            with self.assertRaises(ValueError):
                session.submit_latest("viewport", {**newest_options, "resize": (0, 3)})
            with self.assertRaises(ValueError):
                session.submit_latest("viewport", {**newest_options, "rotate": 45})
            with self.assertRaises(TypeError):
                session.submit_latest("viewport", {**newest_options, "flip_horizontal": 1})
        finally:
            busy.cancel()
        self.assert_cancelled(busy)
        self.assertEqual(newest.result(timeout=5), session.render(newest_options))
        self.assertEqual(other.result(timeout=5), session.render({**self.preview, "tone_shoulder": 1.5}))
        session.close()

    def test_queued_latest_manual_cancel_and_close(self):
        session = self.large_session(max_pending=3)
        busy = self.busy_job(session)
        latest = session.submit_latest("viewport", self.preview)
        latest.cancel()
        queued = session.submit(self.preview)
        session.close()
        session.close()
        for job in (busy, latest, queued):
            self.assert_cancelled(job)
        for call in (lambda: session.render(self.preview), lambda: session.submit(self.preview),
                     lambda: session.submit_latest("viewport", self.preview)):
            with self.assertRaisesRegex(RuntimeError, "closed"):
                call()

    def test_session_deletion_job_deletion_and_result_storage(self):
        session = self.session()
        expected = session.render(self.preview)
        job = session.submit(self.preview)
        del session
        gc.collect()
        retained = job.result(timeout=5)
        self.assertEqual(retained, expected)
        del job
        gc.collect()
        self.assertEqual(retained, expected)
        # Dropping an unfinished job requests cancellation without invalidating
        # a live session or requiring Python code on the worker thread.
        session = self.large_session()
        discarded = self.busy_job(session)
        del discarded
        gc.collect()
        self.assertEqual(session.submit(self.preview).result(timeout=5), session.render(self.preview))
        session.close()

    def test_validation(self):
        session = self.session()
        job = session.submit(self.preview)
        for timeout in (-1, math.nan, math.inf):
            with self.assertRaises(ValueError):
                job.result(timeout=timeout)
        with self.assertRaises(OverflowError):
            job.result(timeout=1e300)
        with self.assertRaises(TypeError):
            job.result(timeout="soon")
        for call in (lambda: session.submit(self.preview, priority="urgent"),
                     lambda: session.submit({**self.preview, "quality": "final"}),
                     lambda: session.submit_latest("", self.preview),
                     lambda: session.submit({**self.preview, "workers": 2})):
            with self.assertRaises(ValueError):
                call()
        for call in (lambda: session.submit_latest(None, self.preview),
                     lambda: session.submit([]), lambda: raw.RenderJob()):
            with self.assertRaises(TypeError):
                call()
        for settings in ({"workers": 0}, {"workers": 65}, {"max_pending": 0}):
            with self.assertRaises(ValueError):
                self.session(**settings)
        for settings in ({"workers": -1}, {"max_pending": -1}):
            with self.assertRaises(OverflowError):
                self.session(**settings)
        self.assertEqual(job.result(timeout=5), session.render(self.preview))
        session.close()


if __name__ == "__main__":
    unittest.main()
