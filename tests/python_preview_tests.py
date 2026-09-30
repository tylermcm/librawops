"""Python reduced requests and persistent source/cache integration checks."""
import array
from concurrent.futures import ThreadPoolExecutor
import gc
import math
import sys
import threading
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw


def values(result):
    w, h, data = result
    pixels = array.array("f")
    pixels.frombytes(data)
    assert len(pixels) == w * h * 3
    return pixels


class PreviewTests(unittest.TestCase):
    def setUp(self):
        self.width, self.height, self.stride = 7, 5, 9
        self.pixels = array.array("f", [12345.0] * (self.stride * self.height * 3))
        for y in range(self.height):
            for x in range(self.width):
                i = (y * self.stride + x) * 3
                self.pixels[i:i + 3] = array.array("f", [
                    2.0 if (x + y) % 2 else -0.1,
                    0.01 * x + 0.03 * y,
                    0.1 + 0.2 * x,
                ])

    def settings(self, space, mip=0):
        result = {"working_space": space, "row_stride_pixels": self.stride,
                  "output_mode": "srgb-preview", "exposure_stops": 0.5,
                  "tone_shoulder": 0.8, "tone_gamma": 1.3, "tile_size": 2}
        if mip:
            result.update(mip=mip, quality="preview")
        return result

    def one_shot(self, settings):
        return raw.render_raster(self.pixels, self.width, self.height, settings)

    def session(self, space="prophoto-d50", cache_bytes=1024 * 1024):
        return raw.RasterSession(self.pixels, self.width, self.height, space,
                                 row_stride_pixels=self.stride, cache_bytes=cache_bytes)

    def render_settings(self, settings):
        return {k: v for k, v in settings.items() if k not in ("working_space", "row_stride_pixels")}

    def test_orientation_reference_and_geometry_jobs(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            session = self.session(space)
            for crop in (None, (1, 1, 5, 3)):
                ox, oy, iw, ih = crop or (0, 0, self.width, self.height)
                for turns in range(4):
                    ow, oh = (ih, iw) if turns % 2 else (iw, ih)
                    for horizontal in (False, True):
                        for vertical in (False, True):
                            oriented = array.array("f", [0.0] * (ow * oh * 3))
                            for sy in range(ih):
                                for sx in range(iw):
                                    dx, dy = sx, sy
                                    if turns == 1: dx, dy = ih - 1 - sy, sx
                                    if turns == 2: dx, dy = iw - 1 - sx, ih - 1 - sy
                                    if turns == 3: dx, dy = sy, iw - 1 - sx
                                    if horizontal: dx = ow - 1 - dx
                                    if vertical: dy = oh - 1 - dy
                                    src = ((oy + sy) * self.stride + ox + sx) * 3
                                    dst = (dy * ow + dx) * 3
                                    oriented[dst:dst + 3] = self.pixels[src:src + 3]
                            for mip in (0, 1, 2):
                                settings = self.settings(space, mip)
                                opts = {**settings, "crop": crop, "rotate": turns * 90,
                                        "flip_horizontal": horizontal, "flip_vertical": vertical}
                                reference_opts = {k: v for k, v in settings.items() if k != "row_stride_pixels"}
                                expected = raw.render_raster(oriented, ow, oh, reference_opts)
                                actual = self.one_shot(opts)
                                self.assertEqual(actual, expected)
                                self.assertEqual(actual, self.one_shot({**opts, "tile_size": 1}))
                                session_opts = self.render_settings(opts)
                                self.assertEqual(actual, session.render(session_opts))
                                job = session.submit_latest("oriented", session_opts)
                                self.assertEqual(actual, job.result(timeout=5))
                                w, h = actual[:2]
                                total = ((w + 1) // 2) * ((h + 1) // 2)
                                self.assertEqual(job.progress(), {"completed_tiles": total, "total_tiles": total})
                                roi = {**opts, "x": w - 1, "y": h - 1, "roi_width": 1, "roi_height": 1}
                                self.assertEqual(values(self.one_shot(roi)), values(actual)[-3:])
                                if turns == 1 and horizontal and not vertical:
                                    resized_opts = {**opts, "resize": (9, 7), "resize_filter": "area"}
                                    reference = raw.render_raster(oriented, ow, oh,
                                        {**reference_opts, "resize": (9, 7), "resize_filter": "area"})
                                    self.assertEqual(self.one_shot(resized_opts), reference)
                                    self.assertEqual(session.submit(self.render_settings(resized_opts)).result(timeout=5), reference)
            opts = {**self.settings(space, 2), "rotate": 90, "flip_horizontal": True}
            session.render(self.render_settings(opts))
            before = session.cache_stats()
            self.assertEqual(session.render(self.render_settings(opts)), self.one_shot(opts))
            self.assertEqual(session.cache_stats()["misses"], before["misses"])
            revised = {**opts, "tone_shoulder": 1.5}
            self.assertEqual(session.submit(self.render_settings(revised)).result(timeout=5), self.one_shot(revised))
            self.assertGreater(session.cache_stats()["hits"], before["hits"])
            self.assertEqual(session.render(self.render_settings(self.settings(space))), self.one_shot(self.settings(space)))
            empty = {**self.render_settings(opts), "x": 2, "y": 2, "roi_width": 0, "roi_height": 0}
            job = session.submit(empty)
            self.assertEqual(job.result(timeout=5), (0, 0, b""))
            self.assertEqual(job.progress(), {"completed_tiles": 0, "total_tiles": 0})
            session.close()
        session = self.session()
        for invalid in ({"rotate": 45}, {"rotate": -90}, {"rotate": 360}, {"rotate": 90.0}, {"rotate": True},
                        {"rotate": None}, {"rotate": 2**80}, {"flip_horizontal": 1}, {"flip_vertical": "yes"}):
            for call in (lambda: self.one_shot(invalid), lambda: session.render(invalid),
                         lambda: session.submit(invalid), lambda: session.submit_latest("invalid", invalid)):
                with self.assertRaises((ValueError, TypeError, OverflowError)): call()
        with self.assertRaises(ValueError): raw.render(array.array("H", [0] * 9), 3, 3, {"rotate": 90})
        session.close()

    def test_resize_reference_sessions_and_jobs(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            session = self.session(space)
            for crop in (None, (1, 1, 5, 3)):
                ox, oy, iw, ih = crop or (0, 0, self.width, self.height)
                for method in ("nearest", "bilinear", "area"):
                    for ow, oh in ((9, 8), (3, 2), (1, 1), (1, 7), (iw, ih)):
                        resized = array.array("f")
                        for y in range(oh):
                            py = max(0.0, min(ih - 1, (y + 0.5) * ih / oh - 0.5))
                            for x in range(ow):
                                px = max(0.0, min(iw - 1, (x + 0.5) * iw / ow - 0.5))
                                for c in range(3):
                                    if method == "nearest":
                                        sample = self.pixels[((oy + math.floor(py + 0.5)) * self.stride +
                                                              ox + math.floor(px + 0.5)) * 3 + c]
                                    elif method == "area":
                                        left, right = x * iw / ow, (x + 1) * iw / ow
                                        top, bottom = y * ih / oh, (y + 1) * ih / oh
                                        sample = math.fsum(
                                            max(0.0, min(right, sx + 1) - max(left, sx)) *
                                            max(0.0, min(bottom, sy + 1) - max(top, sy)) *
                                            self.pixels[((oy + sy) * self.stride + ox + sx) * 3 + c]
                                            for sy in range(ih) for sx in range(iw)) / ((right - left) * (bottom - top))
                                    else:
                                        sample = math.fsum(
                                            max(0.0, 1.0 - abs(px - sx)) * max(0.0, 1.0 - abs(py - sy)) *
                                            self.pixels[((oy + sy) * self.stride + ox + sx) * 3 + c]
                                            for sy in range(ih) for sx in range(iw))
                                    resized.append(sample)
                        opts = {**self.settings(space), "resize": (ow, oh), "resize_filter": method, "crop": crop}
                        expected = raw.render_raster(resized, ow, oh,
                            {k: v for k, v in self.settings(space).items() if k != "row_stride_pixels"})
                        actual = self.one_shot(opts)
                        self.assertEqual(actual[:2], (ow, oh))
                        for a, b in zip(values(actual), values(expected)):
                            self.assertAlmostEqual(a, b, delta=1e-6)
                        self.assertEqual(actual, self.one_shot({**opts, "tile_size": 1}))
                        session_opts = self.render_settings(opts)
                        self.assertEqual(actual, session.render(session_opts))
                        job = session.submit(session_opts)
                        self.assertEqual(actual, job.result(timeout=5))
                        self.assertEqual(actual, job.result(timeout=0))
                        total = ((ow + 1) // 2) * ((oh + 1) // 2)
                        self.assertEqual(job.progress(), {"completed_tiles": total, "total_tiles": total})
                        roi = {**opts, "x": ow - 1, "y": oh - 1, "roi_width": 1, "roi_height": 1}
                        self.assertEqual(values(self.one_shot(roi)), values(actual)[-3:])
                        self.assertEqual(self.one_shot(roi), session.submit_latest("resized", self.render_settings(roi)).result(timeout=5))
                        for mip in (1, 2):
                            reduced_opts = {**opts, "mip": mip, "quality": "preview"}
                            reference_opts = {k: v for k, v in self.settings(space, mip).items() if k != "row_stride_pixels"}
                            reference = raw.render_raster(resized, ow, oh, reference_opts)
                            reduced = self.one_shot(reduced_opts)
                            self.assertEqual(reduced[:2], reference[:2])
                            for a, b in zip(values(reduced), values(reference)):
                                self.assertAlmostEqual(a, b, delta=1e-6)
                            self.assertEqual(reduced, self.one_shot({**reduced_opts, "tile_size": 1}))
                            reduced_session_opts = self.render_settings(reduced_opts)
                            self.assertEqual(reduced, session.render(reduced_session_opts))
                            job = session.submit(reduced_session_opts)
                            self.assertEqual(reduced, job.result(timeout=5))
                            rw, rh = reduced[:2]
                            total = ((rw + 1) // 2) * ((rh + 1) // 2)
                            self.assertEqual(job.progress(), {"completed_tiles": total, "total_tiles": total})
                            reduced_roi = {**reduced_opts, "x": rw - 1, "y": rh - 1, "roi_width": 1, "roi_height": 1}
                            self.assertEqual(values(self.one_shot(reduced_roi)), values(reduced)[-3:])
                            self.assertEqual(self.one_shot(reduced_roi), session.submit_latest("resized", self.render_settings(reduced_roi)).result(timeout=5))
            opts = {**self.settings(space), "resize": (4, 3)}
            cold = session.render(self.render_settings(opts))
            before = session.cache_stats()
            self.assertEqual(cold, session.render(self.render_settings(opts)))
            self.assertGreater(session.cache_stats()["hits"], before["hits"])
            self.assertEqual(session.cache_stats()["misses"], before["misses"])
            for revision in ({"resize_filter": "nearest"}, {"resize_filter": "area"}, {"resize": (3, 4)}, {"tone_shoulder": 1.5}):
                changed = {**opts, **revision}
                self.assertEqual(self.one_shot(changed), session.render(self.render_settings(changed)))
            for mip in (1, 2):
                reduced_opts = {**opts, "resize": (9, 7), "resize_filter": "area", "crop": (1, 1, 5, 3),
                                "mip": mip, "quality": "preview"}
                session.render(self.render_settings(reduced_opts))
                before = session.cache_stats()
                self.assertEqual(session.render(self.render_settings(reduced_opts)), self.one_shot(reduced_opts))
                self.assertEqual(session.cache_stats()["misses"], before["misses"])
                revised = {**reduced_opts, "tone_shoulder": 1.4}
                before = session.cache_stats()
                self.assertEqual(session.submit(self.render_settings(revised)).result(timeout=5), self.one_shot(revised))
                self.assertGreater(session.cache_stats()["hits"], before["hits"])
                empty_preview = {**reduced_opts, "x": (9 + (1 << mip) - 1) // (1 << mip), "roi_width": 0}
                job = session.submit(self.render_settings(empty_preview))
                self.assertEqual(job.result(timeout=5)[2], b"")
                self.assertEqual(job.progress(), {"completed_tiles": 0, "total_tiles": 0})
            self.assertEqual(session.render(self.render_settings({**opts, "resize": None})),
                             self.one_shot(self.settings(space)))
            empty = {"resize": (4, 3), "x": 4, "y": 3, "roi_width": 0, "roi_height": 0}
            job = session.submit(empty)
            self.assertEqual(job.result(timeout=5), (0, 0, b""))
            self.assertEqual(job.progress(), {"completed_tiles": 0, "total_tiles": 0})
            session.close()
        session = self.session()
        for invalid in ({"resize": (0, 2)}, {"resize": (-1, 2)}, {"resize": (2.0, 3)},
                        {"resize": (True, 2)}, {"resize": (2,)}, {"resize": (2**40, 2)},
                        {"resize": (2, 2), "resize_filter": "magic"}, {"resize_filter": "nearest"},
                        {"resize": (2, 2), "mip": 1, "quality": "final"},
                        {"resize": (2, 2), "mip": 0, "quality": "preview"}):
            for call in (lambda: self.one_shot(invalid), lambda: session.render(invalid),
                         lambda: session.submit(invalid), lambda: session.submit_latest("invalid", invalid)):
                with self.assertRaises((ValueError, TypeError, OverflowError)):
                    call()
        self.assertEqual(session.render(), self.one_shot({"working_space": "prophoto-d50", "row_stride_pixels": self.stride}))
        with self.assertRaises(ValueError):
            raw.render(array.array("H", [0] * 9), 3, 3, {"resize": (2, 2)})
        session.close()

    def test_linear_reduction_reference_and_roi(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            native = self.one_shot(self.settings(space))
            explicit_native = self.one_shot({**self.settings(space), "mip": 0, "quality": "final"})
            self.assertEqual(native, explicit_native)
            for mip in (1, 2):
                scale = 1 << mip
                w = (self.width + scale - 1) // scale
                h = (self.height + scale - 1) // scale
                averaged = array.array("f")
                for y in range(h):
                    for x in range(w):
                        indices = [(sy * self.stride + sx) * 3
                                   for sy in range(y * scale, min(self.height, (y + 1) * scale))
                                   for sx in range(x * scale, min(self.width, (x + 1) * scale))]
                        averaged.extend(math.fsum(self.pixels[i + c] for i in indices) / len(indices)
                                        for c in range(3))
                expected = raw.render_raster(averaged, w, h,
                    {**self.render_settings(self.settings(space)), "working_space": space})
                actual = self.one_shot(self.settings(space, mip))
                self.assertEqual(actual[:2], (w, h))
                for a, b in zip(values(actual), values(expected)):
                    self.assertLess(abs(a - b), 1e-6)
                    self.assertTrue(math.isfinite(a) and 0 <= a <= 1)
                for tile_size in (1, 3, 8):
                    self.assertEqual(actual, self.one_shot({**self.settings(space, mip), "tile_size": tile_size}))
                crop = self.one_shot({**self.settings(space, mip), "x": 1, "y": 0,
                                      "roi_width": 1, "roi_height": h, "tile_size": 1})
                self.assertEqual(crop[:2], (1, h))
                full, small = values(actual), values(crop)
                for y in range(h):
                    self.assertEqual(small[y * 3:y * 3 + 3], full[(y * w + 1) * 3:(y * w + 2) * 3])

    def test_validation(self):
        settings = self.settings("prophoto-d50", 1)
        for bad in ({"mip": 3}, {"mip": 1, "quality": "final"},
                    {"mip": 0, "quality": "preview"}, {"quality": "draft"},
                    {"output_mode": "legacy"}, {"tile_size": 0},
                    {"x": 4, "roi_width": 1}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.one_shot({**settings, **bad})
        for bad in ({"mip": -1}, {"mip": 2**40}):
            with self.assertRaises(OverflowError):
                self.one_shot({**settings, **bad})
        for bad in ({"mip": 1.5}, {"quality": 1}):
            with self.assertRaises(TypeError):
                self.one_shot({**settings, **bad})
        with self.assertRaises(ValueError):
            self.one_shot({**self.settings("prophoto-d50"), "mip": 1})
        bayer = array.array("H", [1000] * 16)
        for bad in ({"mip": 1, "quality": "preview"}, {"quality": "preview"}):
            with self.assertRaises(ValueError):
                raw.render(bayer, 4, 4, bad)
        # Exception recovery must reacquire the GIL and keep later calls usable.
        self.assertEqual(self.one_shot(settings)[:2], (4, 3))
        self.assertEqual(self.one_shot({**settings, "roi_width": 0}), (0, 3, b""))

    def test_crop_native_preview_reference_and_session(self):
        crop = (1, 1, 5, 3)
        selected = array.array("f")
        for y in range(crop[1], crop[1] + crop[3]):
            for x in range(crop[0], crop[0] + crop[2]):
                i = (y * self.stride + x) * 3
                selected.extend(self.pixels[i:i + 3])
        for space in ("prophoto-d50", "rec2020-d65"):
            session = self.session(space)
            for mip in (0, 1, 2):
                options = {**self.settings(space, mip), "crop": crop}
                expected = raw.render_raster(selected, crop[2], crop[3],
                    {**self.settings(space, mip), "row_stride_pixels": 0})
                self.assertEqual(self.one_shot(options), expected)
                render_options = self.render_settings(options)
                self.assertEqual(session.render(render_options), expected)
                cold = session.cache_stats()
                self.assertEqual(session.render(render_options), expected)
                self.assertGreater(session.cache_stats()["hits"], cold["hits"])
                self.assertEqual(session.cache_stats()["misses"], cold["misses"])
                for tile_size in (1, 3, 8):
                    self.assertEqual(self.one_shot({**options, "tile_size": tile_size}), expected)
                w, h = expected[:2]
                roi = self.one_shot({**options, "x": w - 1, "y": h - 1, "roi_width": 1, "roi_height": 1})
                self.assertEqual(values(roi), values(expected)[-3:])
                job = session.submit_latest("crop-preview", render_options)
                self.assertEqual(job.result(timeout=5), expected)
                self.assertEqual(job.progress()["completed_tiles"], ((w + 1) // 2) * ((h + 1) // 2))
            # Crop is part of each recipe, never sticky session geometry.
            options = self.settings(space, 1)
            self.assertEqual(session.render(self.render_settings(options)), self.one_shot(options))
            self.assertEqual(session.render({**self.render_settings(options), "crop": None}), self.one_shot(options))
            shifted = {**options, "crop": (2, 0, 4, 5)}
            self.assertEqual(session.render(self.render_settings(shifted)), self.one_shot(shifted))
            session.close()
        for bad in ((1, 2, 3), (0, 0, 0, 1), (6, 0, 2, 1), (0, 4, 1, 2)):
            with self.assertRaises(ValueError):
                self.one_shot({**self.settings("prophoto-d50", 1), "crop": bad})
        for bad in ((-1, 0, 1, 1), (2**40, 0, 1, 1)):
            with self.assertRaises(OverflowError):
                self.one_shot({**self.settings("prophoto-d50", 1), "crop": bad})
        for bad in ((1.5, 0, 1, 1), 2):
            with self.assertRaises(TypeError):
                self.one_shot({**self.settings("prophoto-d50", 1), "crop": bad})
        with self.assertRaises(ValueError):
            raw.render(array.array("H", [1000] * 16), 4, 4, {"crop": (0, 0, 2, 2)})

    def test_session_cache_revisions_and_source_ownership(self):
        for space in ("prophoto-d50", "rec2020-d65"):
            session = self.session(space)
            settings = self.settings(space, 1)
            render_options = self.render_settings(settings)
            expected = self.one_shot(settings)
            self.assertEqual(session.render(render_options), expected)
            cold = session.cache_stats()
            self.assertGreater(cold["entries"], 0)
            self.assertLessEqual(cold["used_bytes"], cold["budget_bytes"])
            self.assertEqual(session.render(render_options), expected)
            warm = session.cache_stats()
            self.assertGreater(warm["hits"], cold["hits"])
            self.assertEqual(warm["misses"], cold["misses"])
            for revision in ({"tone_shoulder": 1.6}, {"exposure_stops": 1.5},
                             {"mip": 2}, {"mip": 0, "quality": "final"},
                             {"mip": 0, "quality": "final", "output_mode": "legacy"}):
                before = session.cache_stats()
                actual = session.render({**render_options, **revision})
                self.assertEqual(actual, self.one_shot({**settings, **revision}))
                after = session.cache_stats()
                self.assertGreater(after["misses"], before["misses"])
                if "tone_shoulder" in revision or "exposure_stops" in revision:
                    self.assertGreater(after["hits"], before["hits"])
            original = self.pixels[0]
            self.pixels[0] = 200.0
            session.clear_cache()
            cleared = session.cache_stats()
            self.assertEqual(cleared["entries"], 0)
            self.assertEqual(cleared["used_bytes"], 0)
            self.assertEqual(cleared["hits"], 0)
            self.assertEqual(cleared["misses"], 0)
            self.assertEqual(session.render(render_options), expected)
            owned_result = session.render(render_options)
            self.assertNotEqual(self.one_shot(settings), expected)
            self.pixels[0] = original
            # Returned bytes outlive the session and do not reference its cache.
            del session
            gc.collect()
            self.assertEqual(owned_result, expected)
            self.assertEqual(len(values(owned_result)), 4 * 3 * 3)

    def test_session_budget_and_validation(self):
        settings = self.render_settings(self.settings("prophoto-d50", 1))
        expected = self.one_shot(self.settings("prophoto-d50", 1))
        for budget in (0, 512):
            session = self.session(cache_bytes=budget)
            self.assertEqual(session.render(settings), expected)
            self.assertLessEqual(session.cache_stats()["used_bytes"], budget)
            session.clear_cache()
            self.assertEqual(session.render(settings), expected)
        session = self.session()
        for key in ("working_space", "row_stride_pixels", "cache_bytes", "red_gain"):
            with self.assertRaises(ValueError):
                session.render({**settings, key: 1})
        with self.assertRaises(TypeError):
            session.render([])
        with self.assertRaises(ValueError):
            session.render({**settings, "quality": "final"})
        for call in (lambda: self.session("invalid"),
                     lambda: raw.RasterSession(self.pixels, 7, 5, "prophoto-d50"),
                     lambda: raw.RasterSession(array.array("f", [math.nan] * 3), 1, 1, "prophoto-d50")):
            with self.assertRaises(ValueError):
                call()
        with self.assertRaises(OverflowError):
            self.session(cache_bytes=-1)
        self.assertEqual(session.render(settings), expected)
        # No-argument render is a fresh default native legacy recipe.
        self.assertEqual(session.render(), self.one_shot({"working_space": "prophoto-d50",
                                                         "row_stride_pixels": self.stride}))

    def test_concurrent_session_renders_and_clear(self):
        session = self.session()
        settings = self.render_settings(self.settings("prophoto-d50", 1))
        barrier = threading.Barrier(4)
        def run(index):
            barrier.wait(timeout=10)
            outputs = []
            for step in range(12):
                if index == 0 and step % 3 == 0:
                    session.clear_cache()
                opts = {**settings, "tone_shoulder": 0.5 + index * 0.25,
                        "mip": 1 + step % 2}
                outputs.append((opts, session.render(opts)))
            return outputs
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(run, range(4)))
        for outputs in results:
            for opts, result in outputs:
                self.assertEqual(result, self.one_shot({**opts, "working_space": "prophoto-d50",
                                                       "row_stride_pixels": self.stride}))
        stats = session.cache_stats()
        self.assertLessEqual(stats["used_bytes"], stats["budget_bytes"])


if __name__ == "__main__":
    unittest.main()
