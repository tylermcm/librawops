"""Optional local-viewer integration controls; no decoder/GUI needed."""
import array
from dataclasses import replace
from pathlib import Path
import queue
import sys
import threading
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw
from raw_viewer_backend import ViewRequest, ViewerWorker, manifest, render_request


class Fixture:
    def __init__(self, path='', decoder=None, native=None, stop_event=None):
        self.path = path
        self.closed = False
        self.record = dict(metadata=dict(width=11, height=9, active_area=[1, 1, 9, 7],
            pattern=0, row_stride_samples=11, black_levels=[100]*4, white_levels=[40000]*4),
            camera_whitebalance=[2, 1, 1.5], orientation=1,
            decoder_rgb_xyz_matrix_uninterpreted=[[1.3705, -.6004, -.14],
                [-.5464, 1.3568, .2062], [-.094, .1706, .7618], [0, 0, 0]])
        data = array.array('H', (5000 + i * 219 % 25000 for i in range(99)))
        self.native = raw.RawSession(data, 11, 9, dict(pattern=0, active_x=1, active_y=1,
            active_width=9, active_height=7, black_levels=[100]*4, white_levels=[40000]*4))

    def session(self, algorithm):
        return self.native

    def close(self):
        self.closed = True
        self.native.close()


def pixels(result):
    w, h, data = result
    return np.frombuffer(data, dtype=np.float32).reshape(h, w, 3)


class ViewerTests(unittest.TestCase):
    def test_exif_transforms_and_nonzero_active_origin_match_native_pixels(self):
        fixture = Fixture()
        try:
            doc, extent = manifest(fixture.native, fixture.record, ViewRequest('fixture'))
            original = pixels(fixture.native.render_manifest(doc))
            # Independent array transforms in EXIF order, on already encoded pixels.
            expected = {1: original, 2: original[:, ::-1], 3: original[::-1, ::-1],
                        4: original[::-1], 5: original.transpose(1, 0, 2),
                        6: np.rot90(original, -1), 7: original.transpose(1, 0, 2)[::-1, ::-1],
                        8: np.rot90(original, 1)}
            for orientation in range(1, 9):
                fixture.record['orientation'] = orientation
                doc, extent = manifest(fixture.native, fixture.record, ViewRequest('fixture'))
                result = fixture.native.render_manifest(doc)
                np.testing.assert_array_equal(pixels(result), expected[orientation])
                self.assertEqual(result[:2], extent)
                request, roi = render_request(ViewRequest('fixture', detail=True, size=(3, 2), center=(1, 0)), extent)
                cropped = pixels(fixture.native.render_manifest(doc, request))
                x, y, w, h = roi
                np.testing.assert_array_equal(cropped, expected[orientation][y:y+h, x:x+w])
        finally:
            fixture.close()

    def test_before_bypasses_all_adjustments_and_identity_preserves_recipe(self):
        from raw_camera_engine_check import camera_matrix
        fixture = Fixture()
        try:
            view = ViewRequest('fixture')
            doc, _ = manifest(fixture.native, fixture.record, view)
            base = fixture.native.render_manifest(doc)
            expected = fixture.native.render(dict(red_gain=2, green_gain=1, blue_gain=1.5,
                camera_to_xyz_d50=camera_matrix(fixture.record), working_space='prophoto-d50', output_mode='srgb-preview'))
            self.assertEqual(base, expected)
            edited = replace(view, exposure=0.8, red=0.8, blue=1.2, saturation=1.75, vibrance=.75, midpoint=0.7, black=0.02, white=1.2,
                hue_shift=(15.0,) * 8, saturation_delta=(.25,) * 8, luminance_delta=(.125,) * 8)
            changed, _ = manifest(fixture.native, fixture.record, edited)
            self.assertNotEqual(fixture.native.render_manifest(changed), base)
            before, _ = manifest(fixture.native, fixture.record, replace(edited, before=True))
            self.assertEqual(fixture.native.render_manifest(before), base)
            req, _ = render_request(view, (9, 7))
            w, h, data = fixture.native.render_manifest(doc, req)
            self.assertEqual((w, h), (3, 2))
        finally:
            fixture.close()

    def test_native_view_clips_at_all_edges_without_scaling(self):
        for center in ((0, 0), (1, 0), (0, 1), (1, 1), (0.5, 0.5)):
            req, roi = render_request(ViewRequest('fixture', detail=True, size=(300, 200), center=center), (1000, 777))
            x, y, w, h = roi
            self.assertEqual((w, h), (300, 200))
            self.assertGreaterEqual(x, 0)
            self.assertGreaterEqual(y, 0)
            self.assertLessEqual(x+w, 1000)
            self.assertLessEqual(y+h, 777)
            self.assertEqual((req['mip'], req['quality']), (0, 'final'))

    def test_saturation_matches_explicit_engine_graph_at_native_and_mips(self):
        import json
        fixture = Fixture()
        try:
            for orientation in (1,6):
                fixture.record['orientation']=orientation
                baseline, _=manifest(fixture.native,fixture.record,ViewRequest('fixture'))
                baseline_doc=json.loads(baseline)
                saturation=next(o for o in baseline_doc['operations'] if o['type']=='rawengine.saturation')
                self.assertEqual(saturation['parameters'],dict(amount=1))
                self.assertEqual(next(o for o in baseline_doc['operations'] if o['id']==saturation['inputs']['image'])['type'],'rawengine.curves')
                for mip in (0,1,2):
                    request=dict(mip=mip,quality='preview' if mip else 'final',tile_size=3)
                    original=fixture.native.render_manifest(baseline,request)
                    for amount in (0,.5,1,1.75,4):
                        doc=json.loads(baseline)
                        next(o for o in doc['operations'] if o['type']=='rawengine.saturation')['parameters']['amount']=amount
                        expected=fixture.native.render_manifest(json.dumps(doc),request)
                        view=replace(ViewRequest('fixture'),saturation=amount)
                        text,_=manifest(fixture.native,fixture.record,view)
                        self.assertEqual(fixture.native.render_manifest(text,request),expected)
                        if amount==1:self.assertEqual(expected,original)
                        if amount==0:
                            image=pixels(expected)
                            self.assertLess(float(np.max(np.ptp(image,axis=2))),2e-6)
                        before,_=manifest(fixture.native,fixture.record,replace(view,before=True))
                        self.assertEqual(fixture.native.render_manifest(before,request),original)
        finally:fixture.close()

    def test_vibrance_matches_explicit_engine_graph_at_native_and_mips(self):
        import json
        fixture = Fixture()
        try:
            for orientation in (1,6):
                fixture.record['orientation']=orientation
                baseline, _=manifest(fixture.native,fixture.record,ViewRequest('fixture'))
                doc=json.loads(baseline)
                vibrance=next(o for o in doc['operations'] if o['type']=='rawengine.vibrance')
                self.assertEqual(vibrance['parameters'],dict(amount=0))
                self.assertEqual(next(o for o in doc['operations'] if o['id']==vibrance['inputs']['image'])['type'],'rawengine.saturation')
                for mip in (0,1,2):
                    request=dict(mip=mip,quality='preview' if mip else 'final',tile_size=3)
                    original=fixture.native.render_manifest(baseline,request)
                    for amount in (-1,-.5,0,.5,1):
                        expected_doc=json.loads(baseline)
                        next(o for o in expected_doc['operations'] if o['type']=='rawengine.vibrance')['parameters']['amount']=amount
                        expected=fixture.native.render_manifest(json.dumps(expected_doc),request)
                        view=replace(ViewRequest('fixture'),vibrance=amount)
                        text,_=manifest(fixture.native,fixture.record,view)
                        self.assertEqual(fixture.native.render_manifest(text,request),expected)
                        if amount==0:self.assertEqual(expected,original)
                        before,_=manifest(fixture.native,fixture.record,replace(view,before=True))
                        self.assertEqual(fixture.native.render_manifest(before,request),original)
        finally:fixture.close()

    def test_color_mixer_bands_match_explicit_native_graph_and_own_arrays(self):
        import json
        fixture = Fixture()
        try:
            bands = [15.0] + [0.0] * 7
            snapshot = ViewRequest('fixture', hue_shift=bands)
            bands[0] = 60
            self.assertEqual(snapshot.hue_shift, (15.0,) + (0.0,) * 7)
            for orientation in (1, 6):
                fixture.record['orientation'] = orientation
                baseline, _ = manifest(fixture.native, fixture.record, ViewRequest('fixture'))
                doc = json.loads(baseline)
                mixer = next(o for o in doc['operations'] if o['type'] == 'rawengine.color_mixer')
                self.assertTrue(all(values == [0.0] * 8 for values in mixer['parameters'].values()))
                self.assertEqual(next(o for o in doc['operations'] if o['id'] == mixer['inputs']['image'])['type'], 'rawengine.vibrance')
                self.assertTrue(any(o['inputs'].get('image') == mixer['id'] for o in doc['operations']))
                for band in range(8):
                    parameters = {name: [0.0] * 8 for name in mixer['parameters']}
                    for name, amount in (('hue_shift', (-1) ** band * 60),
                        ('saturation_delta', (-1) ** band), ('luminance_delta', -(-1) ** band)):
                        parameters[name][band] = amount
                    view = ViewRequest('fixture', **parameters)
                    text, _ = manifest(fixture.native, fixture.record, view)
                    expected_doc = json.loads(baseline)
                    next(o for o in expected_doc['operations'] if o['type'] == 'rawengine.color_mixer')['parameters'] = parameters
                    before, _ = manifest(fixture.native, fixture.record, replace(view, before=True))
                    for mip in (0, 1, 2):
                        req = dict(mip=mip, quality='preview' if mip else 'final', tile_size=3)
                        self.assertEqual(fixture.native.render_manifest(text, req),
                            fixture.native.render_manifest(json.dumps(expected_doc), req))
                        self.assertEqual(fixture.native.render_manifest(before, req),
                            fixture.native.render_manifest(baseline, req))
        finally:
            fixture.close()

    def test_worker_coalesces_pending_views_and_rejects_stale_load(self):
        started, release = threading.Event(), threading.Event()
        fixtures = []
        def load(*args):
            started.set()
            self.assertTrue(release.wait(5))
            fixture = Fixture(*args)
            fixtures.append(fixture)
            return fixture
        worker = ViewerWorker('unused', raw, loader=load)
        try:
            worker.submit(ViewRequest('fixture'))
            self.assertTrue(started.wait(5))
            worker.submit(ViewRequest('fixture', exposure=0.3))
            latest = worker.submit(ViewRequest('fixture', exposure=0.9, saturation=1.75, hue_shift=(15.0,) * 8))
            release.set()
            revision, kind, payload = worker.events.get(timeout=10)
            self.assertEqual((revision, kind), (latest, 'image'))
            self.assertEqual(payload['view'].exposure, 0.9)
            self.assertEqual(payload['view'].saturation, 1.75)
            self.assertEqual(payload['view'].hue_shift, (15.0,) * 8)
            self.assertEqual(payload['result'][:2], (3, 2))
            self.assertEqual(len(fixtures), 1)
            with self.assertRaises(queue.Empty):
                worker.events.get_nowait()
        finally:
            release.set()
            worker.close()
            worker.thread.join(5)
        self.assertFalse(worker.thread.is_alive())
        self.assertTrue(fixtures[0].closed)

    def test_close_during_load_closes_source_without_publishing(self):
        started, release = threading.Event(), threading.Event()
        fixtures = []
        def load(*args):
            started.set()
            release.wait(5)
            fixture = Fixture(*args)
            fixtures.append(fixture)
            return fixture
        worker = ViewerWorker('unused', raw, loader=load)
        worker.submit(ViewRequest('fixture'))
        self.assertTrue(started.wait(5))
        worker.close()
        release.set()
        worker.thread.join(5)
        self.assertFalse(worker.thread.is_alive())
        self.assertTrue(fixtures[0].closed)
        self.assertTrue(worker.events.empty())
        with self.assertRaises(RuntimeError):
            worker.submit(ViewRequest('fixture'))

    def test_load_error_is_reported_and_next_request_can_recover(self):
        def load(path, *args):
            if path == 'bad':
                raise ValueError('Unsupported sensor')
            return Fixture(path, *args)
        worker = ViewerWorker('unused', raw, loader=load)
        try:
            revision = worker.submit(ViewRequest('bad'))
            self.assertEqual(worker.events.get(timeout=5), (revision, 'error', 'Unsupported sensor'))
            revision = worker.submit(ViewRequest('good'))
            result_revision, kind, payload = worker.events.get(timeout=5)
            self.assertEqual((result_revision, kind), (revision, 'image'))
        finally:
            worker.close()
            worker.thread.join(5)


if __name__ == '__main__':
    unittest.main()
