"""Independent-source graphs, ROI mapping, revisions and owned job snapshots."""
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


def uid(n):
    return f"20000000-0000-0000-0000-{n:012d}"


A, B = uid(1), uid(2)


def spec(w=7, h=5, stride=9, space="prophoto-d50", offset=0):
    pixels = array.array("f", [4321]) * (stride * h * 3)
    for y in range(h):
        for x in range(w):
            i = (y * stride + x) * 3
            pixels[i:i+3] = array.array("f", (offset + x / 8 - 0.5, offset - y / 4, offset + (x+y) / 3))
    return dict(rgb=pixels, width=w, height=h, row_stride_pixels=stride, working_space=space)


def packed(source):
    w, h = source["width"], source["height"]
    stride = source.get("row_stride_pixels", w) or w
    values = array.array("f")
    for y in range(h):
        values.extend(source["rgb"][y*stride*3:(y*stride+w)*3])
    return values


def reduce_pixels(values, w, h, mip):
    scale = 1 << mip
    out = array.array("f")
    for y in range(0, h, scale):
        for x in range(0, w, scale):
            indices = [yy*w+xx for yy in range(y, min(h, y+scale)) for xx in range(x, min(w, x+scale))]
            for c in range(3):
                out.append(sum(values[3*i+c] for i in indices) / len(indices))
    return out


def op(n, kind, inputs, params=None, domain="scene_linear_prophoto_d50", output=None):
    return dict(id=uid(n), type=kind, schema_version=1, processing_version=2, enabled=True,
                input_domain=domain, output_domain=output or domain, inputs=inputs,
                parameters=params or {}, masks={}, blend_mode="normal", opacity=1)


def mix_doc(session, domain="scene_linear_prophoto_d50", amount=0.25):
    doc = json.loads(session.export_manifest(A))
    node = op(10, "rawengine.linear_mix", {"base": A, "layer": B}, {"amount": amount}, domain)
    doc.update(operations=[node], output=node["id"])
    return doc


class MultiSourceTests(unittest.TestCase):
    def assert_pixels(self, result, expected):
        actual = array.array("f"); actual.frombytes(result[2])
        self.assertEqual(len(actual), len(expected))
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, delta=3e-7 * max(1, abs(a), abs(b)))

    def test_mix_numeric_tiles_roi_and_jobs_both_spaces(self):
        for space, domain in (("prophoto-d50", "scene_linear_prophoto_d50"),
                              ("rec2020-d65", "scene_linear_rec2020_d65")):
            base, layer = spec(space=space), spec(stride=8, space=space, offset=2)
            session = raw.RasterGraphSession({A: base, B: layer}, workers=2)
            doc = mix_doc(session, domain)
            info = session.source_info()
            self.assertEqual(set(info), {A, B})
            self.assertEqual(doc["sources"], [{k: v for k, v in info[id].items() if k not in ("width", "height")} for id in (A, B)])
            before = copy.deepcopy(info)
            info[A]["content_sha256"] = "0" * 64
            self.assertEqual(session.source_info(), before)
            for mip in (0, 1, 2):
                request = {"mip": mip, "quality": "preview" if mip else "final", "tile_size": 2}
                x = reduce_pixels(packed(base), 7, 5, mip)
                y = reduce_pixels(packed(layer), 7, 5, mip)
                expected = [0.75*a + 0.25*b for a, b in zip(x, y)]
                text = json.dumps(doc)
                result = session.render_manifest(text, request)
                self.assert_pixels(result, expected)
                self.assertEqual(session.render_manifest(text, {**request, "tile_size": 1}), result)
                self.assertEqual(session.render_manifest(text, {**request, "tile_size": 64}), result)
                roi = {**request, "x": 1, "y": 1, "roi_width": 1, "roi_height": 1}
                self.assertEqual(session.render_manifest(text, roi)[2], result[2][(result[0]+1)*12:(result[0]+2)*12])
                job = session.submit_manifest_latest("view", text, request)
                with ThreadPoolExecutor(max_workers=2) as pool:
                    self.assertEqual(list(pool.map(lambda _: job.result(timeout=5), range(2))), [result, result])
                total = ((result[0]+1)//2) * ((result[1]+1)//2)
                self.assertEqual(job.progress(), {"completed_tiles": total, "total_tiles": total})
                self.assertEqual(set(session.required_source_regions(text, request)), {A, B})
            for amount, source in ((0, base), (1, layer)):
                doc["operations"][0]["parameters"]["amount"] = amount
                self.assertEqual(session.render_manifest(json.dumps(doc))[2], packed(source).tobytes())
            doc["operations"][0]["enabled"] = False
            text = json.dumps(doc)
            self.assertEqual(session.render_manifest(text)[2], packed(base).tobytes())
            self.assertEqual(session.required_source_regions(text), {A: (0, 0, 7, 5)})
            # Selecting a subset does not require unused owned sources in the document.
            subset = json.loads(session.export_manifest(A)); subset["sources"] = [subset["sources"][0]]
            self.assertEqual(session.render_manifest(json.dumps(subset))[2], packed(base).tobytes())
            session.close()

    def test_branch_geometry_source_footprints_and_resize(self):
        base, layer = spec(), spec(w=8, h=8, stride=10, offset=3)
        session = raw.RasterGraphSession({A: base, B: layer})
        doc = mix_doc(session)
        crop_a = op(11, "rawengine.crop", {"image": A}, dict(x=1, y=1, width=5, height=3))
        turn = op(12, "rawengine.orientation", {"image": crop_a["id"]}, dict(quarter_turns=1, flip_horizontal=False, flip_vertical=False))
        crop_b = op(13, "rawengine.crop", {"image": B}, dict(x=1, y=1, width=6, height=5))
        resize = op(14, "rawengine.resize", {"image": crop_b["id"]}, dict(width=3, height=5, filter="area"))
        mix = doc["operations"][0]; mix["inputs"] = {"base": turn["id"], "layer": resize["id"]}
        doc["operations"] = [mix, resize, crop_b, turn, crop_a]  # Saved order is not evaluation order.
        oriented = array.array("f", [0]) * 45
        for sy in range(3):
            for sx in range(5):
                target = (sx*3 + 2-sy) * 3
                source = ((sy+1)*9 + sx+1) * 3
                oriented[target:target+3] = base["rgb"][source:source+3]
        resized = array.array("f")
        for y in range(5):
            for x in range(3):
                index = ((y+1)*10 + 1+2*x) * 3
                resized.extend((layer["rgb"][index+c]+layer["rgb"][index+3+c])/2 for c in range(3))
        text = json.dumps(doc)
        for mip in (0, 1, 2):
            request = {"mip": mip, "quality": "preview" if mip else "final", "tile_size": 1}
            a = reduce_pixels(oriented, 3, 5, mip); b = reduce_pixels(resized, 3, 5, mip)
            result = session.render_manifest(text, request)
            self.assert_pixels(result, [0.75*x+0.25*y for x, y in zip(a, b)])
            self.assertEqual(session.submit_manifest(text, request).result(timeout=5), result)
        self.assertEqual(session.required_source_regions(text, {"mip": 1, "quality": "preview", "roi_width": 1, "roi_height": 1}),
                         {A: (1, 2, 2, 2), B: (1, 1, 4, 2)})
        self.assertEqual(session.required_source_regions(text, {"mip": 2, "quality": "preview", "roi_width": 1, "roi_height": 1}),
                         {A: (1, 1, 4, 3), B: (1, 1, 6, 4)})
        self.assertEqual(session.required_source_regions(text), {A: (1, 1, 5, 3), B: (1, 1, 6, 5)})
        # A disabled mix needs only its transformed base.
        mix["enabled"] = False
        self.assertEqual(session.required_source_regions(json.dumps(doc)), {A: (1, 1, 5, 3)})
        session.close()

    def test_source_revision_cache_and_failed_replacement(self):
        base, layer = spec(), spec(offset=2)
        session = raw.RasterGraphSession({A: base, B: layer})
        doc = mix_doc(session); text = json.dumps(doc)
        old = session.render_manifest(text)
        session.render_manifest(text)
        before = session.cache_stats()
        info = session.source_info()
        self.assertIsNone(session.replace_source(B, spec(offset=4)))
        self.assertEqual(session.source_info()[A], info[A])
        self.assertNotEqual(session.source_info()[B]["content_sha256"], info[B]["content_sha256"])
        with self.assertRaises(ValueError): session.render_manifest(text)
        new = copy.deepcopy(doc)
        new["sources"] = json.loads(session.export_manifest(A))["sources"]
        current = session.render_manifest(json.dumps(new))
        self.assertNotEqual(old, current)
        after = session.cache_stats()
        self.assertEqual(after["hits"]-before["hits"], 1)  # Unchanged base survives.
        self.assertEqual(after["misses"]-before["misses"], 2)  # New layer and mix.
        for bad in ({**layer, "width": 0}, {**layer, "rgb": array.array("f", [float("nan")])*135}):
            with self.assertRaises(ValueError): session.replace_source(B, bad)
        with self.assertRaises(ValueError): session.replace_source(uid(99), layer)
        self.assertEqual(session.render_manifest(json.dumps(new)), current)
        session.clear_cache()
        self.assertEqual(session.cache_stats()["entries"], 0)
        self.assertEqual(session.render_manifest(json.dumps(new)), current)
        # Caller-owned buffer writes cannot modify either owned source.
        base["rgb"][0] = 111
        self.assertEqual(session.render_manifest(json.dumps(new)), current)
        session.close()

    def test_color_domains_alignment_and_validation(self):
        session = raw.RasterGraphSession({A: spec(), B: spec(space="rec2020-d65")})
        doc = mix_doc(session)
        with self.assertRaises(ValueError): session.render_manifest(json.dumps(doc))
        convert = op(11, "rawengine.working_space_convert", {"image": B}, {},
                     "scene_linear_rec2020_d65", "scene_linear_prophoto_d50")
        doc["operations"][0]["inputs"]["layer"] = convert["id"]
        doc["operations"].append(convert)
        text = json.dumps(doc)
        # Each endpoint has the explicit branch descriptor; mix numerics remain linear.
        for mip in (0, 1, 2):
            request = {"mip": mip, "quality": "preview" if mip else "final"}
            branch_a = {**doc, "output": A}; branch_b = {**doc, "output": convert["id"]}
            a = array.array("f"); a.frombytes(session.render_manifest(json.dumps(branch_a), request)[2])
            b = array.array("f"); b.frombytes(session.render_manifest(json.dumps(branch_b), request)[2])
            self.assert_pixels(session.render_manifest(text, request), [0.75*x+0.25*y for x, y in zip(a, b)])
        bad = copy.deepcopy(doc); bad["sources"][1]["content_sha256"] = "a5"*32
        with self.assertRaises(ValueError): session.submit_manifest(json.dumps(bad))
        bad = copy.deepcopy(doc); bad["sources"].append({**bad["sources"][0], "id": uid(99)})
        with self.assertRaises(ValueError): session.submit_manifest(json.dumps(bad))
        session.replace_source(B, spec(w=8, h=5, stride=8, space="rec2020-d65"))
        doc["sources"] = json.loads(session.export_manifest(A))["sources"]
        with self.assertRaises(ValueError): session.render_manifest(json.dumps(doc))  # Extents differ.
        session.close()

    def test_concurrent_replacement_and_graph_snapshots(self):
        base, variants = spec(), (spec(offset=2), spec(offset=4))
        expected = {}
        for variant in variants:
            reference = raw.RasterSession(variant["rgb"], 7, 5, "prophoto-d50", row_stride_pixels=9)
            expected[reference.source_info()["content_sha256"]] = packed(variant).tobytes()
            reference.close()
        session = raw.RasterGraphSession({A: base, B: variants[0]}, cache_bytes=0)
        def update():
            for i in range(30):
                session.replace_source(B, variants[i % 2])
        def render():
            for _ in range(30):
                text = session.export_manifest(B)
                digest = next(s for s in json.loads(text)["sources"] if s["id"] == B)["content_sha256"]
                try:
                    result = session.render_manifest(text, {"tile_size": 1})
                except ValueError as error:
                    # A source revision between export and binding invalidates
                    # the saved fingerprint. It cannot create a mixed frame.
                    self.assertIn("source binding", str(error))
                else:
                    self.assertEqual(result[2], expected[digest])
        with ThreadPoolExecutor(max_workers=3) as pool:
            futures = [pool.submit(update), pool.submit(render), pool.submit(render)]
            for future in futures: future.result(timeout=5)
        final = session.export_manifest(B)
        self.assertEqual(session.render_manifest(final)[2], packed(variants[1]).tobytes())
        session.close()

    def test_constructor_and_spec_rejection(self):
        good = spec()
        for sources in ({}, {uid(i): good for i in range(65)}, {"not-a-uuid": good},
                        {"AAAAAAAA-0000-0000-0000-000000000001": good}):
            with self.assertRaises(ValueError): raw.RasterGraphSession(sources)
        for sources in ([], {1: good}, {A: []}):
            with self.assertRaises(TypeError): raw.RasterGraphSession(sources)
        for bad in ({**good, "typo": 1}, {**good, "width": 0}, {**good, "row_stride_pixels": 6},
                    {**good, "working_space": "prophoto-d50\x00"}, {**good, "rgb": b""}):
            with self.assertRaises(ValueError): raw.RasterGraphSession({A: bad})
        for bad in ({**good, "width": True}, {**good, "height": 1.2}, {**good, "working_space": None}):
            with self.assertRaises(TypeError): raw.RasterGraphSession({A: bad})
        for settings in ({"workers": 0}, {"workers": 65}, {"max_pending": 0}):
            with self.assertRaises(ValueError): raw.RasterGraphSession({A: good}, **settings)

    def test_job_snapshot_supersession_close_and_lifetimes(self):
        values = array.array("f", [0.25]) * (1024*1024*3)
        large = dict(rgb=values, width=1024, height=1024, working_space="prophoto-d50")
        session = raw.RasterGraphSession({A: large, B: large}, cache_bytes=0, max_pending=2)
        source = session.export_manifest(A)
        mixed = mix_doc(session); text = json.dumps(mixed)
        busy = session.submit_manifest(source, {"tile_size": 1}, priority="background")
        deadline = time.monotonic()+5
        while busy.progress()["completed_tiles"] == 0 and not busy.done():
            if time.monotonic() > deadline:
                busy.cancel(); self.fail("worker did not start")
            time.sleep(0.001)
        request = {"roi_width": 1, "roi_height": 1}
        old = session.submit_manifest(text, request)
        previous = session.submit_manifest_latest("view", text, request)
        try:
            replacement = {**large, "rgb": array.array("f", [0.75]) * len(values)}
            session.replace_source(B, replacement)
            # Old saved fingerprints reject before touching the valid group job.
            with self.assertRaises(ValueError): session.submit_manifest_latest("view", text, request)
            self.assertFalse(previous.done())
            mixed["sources"] = json.loads(session.export_manifest(A))["sources"]
            newest = session.submit_manifest_latest("view", json.dumps(mixed), request)
            with self.assertRaises(raw.RenderCancelled): previous.result(timeout=5)
            with self.assertRaises(RuntimeError): session.submit_manifest(json.dumps(mixed), request)
        finally:
            busy.cancel()
        with self.assertRaises(raw.RenderCancelled): busy.result(timeout=5)
        self.assertEqual(old.result(timeout=5)[2], array.array("f", [0.25]*3).tobytes())
        self.assertEqual(newest.result(timeout=5)[2], array.array("f", [0.375]*3).tobytes())
        # Close cancels running and queued graph jobs, including old source snapshots.
        running = session.submit_manifest(session.export_manifest(A), {"tile_size": 1})
        queued = session.submit_manifest(json.dumps(mixed), request)
        session.close(); session.close()
        for job in (running, queued):
            with self.assertRaises(raw.RenderCancelled): job.result(timeout=5)
        for call in (lambda: session.replace_source(A, large), lambda: session.export_manifest(A),
                     lambda: session.render_manifest(json.dumps(mixed)),
                     lambda: session.submit_manifest(json.dumps(mixed)),
                     lambda: session.required_source_regions(json.dumps(mixed))):
            with self.assertRaisesRegex(RuntimeError, "closed"): call()
        self.assertEqual(set(session.source_info()), {A, B})
        session = raw.RasterGraphSession({A: spec(), B: spec(offset=2)})
        text = json.dumps(mix_doc(session)); expected = session.render_manifest(text)
        job = session.submit_manifest(text)
        del session, text
        gc.collect()
        self.assertEqual(job.result(timeout=5), expected)


if __name__ == "__main__":
    unittest.main()
