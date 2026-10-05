"""Owned scalar sources, explicit typed sessions/history/jobs and frozen mask bits."""
import array
import copy
from concurrent.futures import ThreadPoolExecutor
import ctypes
import gc
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw

HEADER = (Path(__file__).parent / 'reference/mask_graph_numeric_v1.hpp').read_text(encoding='utf-8')


def words(name):
    values = re.search(r'inline constexpr std::uint32_t '+name+r'\[\] = \{([^}]+)\};', HEADER).group(1)
    return [int(value, 16) for value in re.findall(r'0x([0-9a-f]+)u', values)]


def packed(name):
    values = words(name)
    return struct.pack('='+'I'*len(values), *values)


def floats(name):
    result = array.array('f'); result.frombytes(packed(name)); return result


def uid(n):
    return f'70000000-0000-4000-8000-{n:012d}'


A, B, MA, MB = (uid(n) for n in range(1, 5))


def coverage_spec(name='mask_a', stride=9, x=0, y=0):
    source = floats(name); values = array.array('f', [float('nan')])*(stride*5)
    for row in range(5): values[row*stride:row*stride+7] = source[row*7:row*7+7]
    return dict(coverage=values, width=7, height=5, row_stride_samples=stride, x=x, y=y)


def sources(space='prophoto-d50'):
    return {A: dict(rgb=floats('base_rgb'), width=7, height=5, working_space=space),
            B: dict(rgb=floats('layer_rgb'), width=7, height=5, working_space=space),
            MA: coverage_spec(), MB: coverage_spec('mask_b', 8)}


def operation(n, kind, inputs, parameters=None, domain='coverage', masks=None):
    return dict(id=uid(n), type=kind, schema_version=1, processing_version=2, enabled=True,
                input_domain=domain, output_domain=domain, inputs=inputs, masks=masks or {},
                parameters=parameters or {}, blend_mode='normal', opacity=1)


def documents(session, space='prophoto-d50'):
    manifest = json.loads(session.export_manifest(A))
    domain = 'scene_linear_prophoto_d50' if space == 'prophoto-d50' else 'scene_linear_rec2020_d65'
    invert = operation(5, 'rawengine.mask_invert', {'mask': MA})
    combine = operation(6, 'rawengine.mask_combine', {'base': uid(5), 'layer': MB}, {'mode': 'intersect'})
    mix = operation(7, 'rawengine.masked_mix', {'base': A, 'layer': B}, {'amount': .5}, domain, {'coverage': uid(6)})
    manifest.update(operations=[invert, combine, mix], output=uid(7))
    scalar = copy.deepcopy(manifest); scalar['output'] = uid(6)
    return json.dumps(manifest), json.dumps(scalar)


class CoverageDeliveryTests(unittest.TestCase):
    def test_frozen_chain_spaces_levels_roi_cache_jobs_and_caller_ownership(self):
        for space in ('prophoto-d50', 'rec2020-d65'):
            inputs = sources(space); session = raw.RasterGraphSession(inputs, workers=2)
            rgb, scalar = documents(session, space)
            self.assertEqual(json.loads(rgb)['format_version'], 4)
            for spec in inputs.values():
                buffer = spec.get('coverage', spec.get('rgb')); buffer[:] = array.array('f', [99])*len(buffer)
            for mip, quality in ((0, 'final'), (0, 'preview'), (1, 'preview'), (2, 'preview')):
                dims = ((7, 5), (4, 3), (2, 2))[mip]
                expected_rgb = (*dims, packed('mixed_mip'+str(mip)))
                expected_scalar = (*dims, packed('coverage_mip'+str(mip)))
                request = dict(mip=mip, quality=quality, tile_size=2)
                for size in (1, 2, 8):
                    self.assertEqual(session.render_manifest(rgb, {**request, 'tile_size': size}), expected_rgb)
                    self.assertEqual(session.render_coverage_manifest(scalar, {**request, 'tile_size': size}), expected_scalar)
                for y in range(dims[1]):
                    for x in range(dims[0]):
                        roi = {**request, 'x': x, 'y': y, 'roi_width': 1, 'roi_height': 1}
                        offset = y*dims[0]+x
                        self.assertEqual(session.render_coverage_manifest(scalar, roi)[2], expected_scalar[2][offset*4:(offset+1)*4])
                        scale = 1 << mip
                        footprint = (x*scale, y*scale, min(scale, 7-x*scale), min(scale, 5-y*scale))
                        self.assertEqual(session.required_source_regions(scalar, roi), {MA: footprint, MB: footprint})
                        self.assertEqual(session.required_source_regions(rgb, roi), {key: footprint for key in (A, B, MA, MB)})
                job = session.submit_coverage_manifest_latest('view', scalar, request)
                self.assertEqual(job.output_kind(), 'coverage')
                with ThreadPoolExecutor(max_workers=2) as pool:
                    self.assertEqual(list(pool.map(lambda _: job.result(timeout=5), range(2))), [expected_scalar]*2)
                self.assertTrue(job.done())
                total = ((dims[0]+1)//2)*((dims[1]+1)//2)
                self.assertEqual(job.progress(), dict(completed_tiles=total, total_tiles=total))
                rgb_job = session.submit_manifest(rgb, request)
                self.assertEqual(rgb_job.output_kind(), 'rgb'); self.assertEqual(rgb_job.result(timeout=5), expected_rgb)
            session.render_coverage_manifest(scalar); before = session.cache_stats()
            session.render_coverage_manifest(scalar); after = session.cache_stats()
            self.assertEqual(before['misses'], after['misses']); self.assertGreater(after['hits'], before['hits'])
            self.assertEqual(session.render_coverage_manifest(scalar, dict(x=7, y=5, roi_width=0, roi_height=0)), (0, 0, b''))
            session.clear_cache(); self.assertEqual(session.cache_stats()['entries'], 0)
            session.close()

    def test_independent_identity_padding_origin_and_typed_buffers(self):
        spec = coverage_spec(stride=10, x=9, y=11)
        session = raw.RasterGraphSession({MA: spec})
        info = session.source_info()[MA]
        digest = hashlib.sha256(b'librawops.source.coverage.f32.v1\0'+struct.pack('<4I', 9, 11, 7, 5)+
                                struct.pack('<35I', *words('mask_a'))).hexdigest()
        self.assertEqual(info, dict(id=MA, kind='coverage_raster_f32', content_sha256=digest, width=7, height=5, x=9, y=11))
        text = session.export_manifest(MA)
        self.assertEqual(session.render_coverage_manifest(text), (7, 5, packed('mask_a')))
        self.assertEqual(session.required_source_regions(text, dict(mip=2, quality='preview', x=1, y=1, roi_width=1, roi_height=1)),
                         {MA: (13, 15, 3, 1)})
        info['content_sha256'] = '0'*64; self.assertEqual(session.source_info()[MA]['content_sha256'], digest)
        for value in (packed('mask_a'), memoryview(floats('mask_a')), (ctypes.c_float*35)(*floats('mask_a'))):
            typed = raw.RasterGraphSession({MA: dict(coverage=value, width=7, height=5)})
            self.assertEqual(typed.render_coverage_manifest(typed.export_manifest(MA))[2], packed('mask_a')); typed.close()
        session.close()

    def test_admission_payload_and_request_guards(self):
        good = coverage_spec()
        for change in ({'rgb': b''}, {'working_space': 'prophoto-d50'}, {'input_profile': None}, {'typo': 1},
                       {'row_stride_pixels': 9}, {'width': 0}, {'height': 0}, {'row_stride_samples': 6},
                       {'coverage': b''}, {'coverage': array.array('I', [0]*45)}, {'coverage': array.array('d', [0]*45)},
                       {'x': 2**32-1}):
            with self.assertRaises((ValueError, OverflowError)): raw.RasterGraphSession({MA: {**good, **change}})
        for change in ({'width': True}, {'height': 1.5}, {'x': False}, {'row_stride_samples': None}):
            with self.assertRaises(TypeError): raw.RasterGraphSession({MA: {**good, **change}})
        with self.assertRaises((BufferError, ValueError)): raw.RasterGraphSession({MA: {**good, 'coverage': memoryview(good['coverage'])[::2]}})
        for value in (float('nan'), float('inf'), -.01, 1.01):
            bad = coverage_spec(); bad['coverage'][0] = value
            with self.assertRaises(ValueError): raw.RasterGraphSession({MA: bad})
        endian = (ctypes.c_float.__ctype_be__*35)(*floats('mask_a')) if sys.byteorder == 'little' else (ctypes.c_float.__ctype_le__*35)(*floats('mask_a'))
        with self.assertRaises(ValueError): raw.RasterGraphSession({MA: dict(coverage=endian, width=7, height=5)})
        session = raw.RasterGraphSession(sources()); rgb, scalar = documents(session)
        for call in (lambda: session.render_manifest(scalar), lambda: session.submit_manifest(scalar),
                     lambda: session.submit_manifest_latest('view', scalar), lambda: session.render_coverage_manifest(rgb),
                     lambda: session.submit_coverage_manifest(rgb), lambda: session.submit_coverage_manifest_latest('view', rgb),
                     lambda: session.histogram_manifest(scalar),
                     lambda: session.analyze_local_manifest(scalar, lambda *_: None)):
            with self.assertRaises(ValueError): call()
        for options in ({'tile_size': 0}, {'mip': 1, 'quality': 'final'}, {'x': 7, 'roi_width': 1}, {'exposure_stops': 1}):
            with self.assertRaises(ValueError): session.render_coverage_manifest(scalar, options)
        with self.assertRaises(ValueError): session.submit_coverage_manifest_latest('', scalar)
        job = session.submit_coverage_manifest(scalar)
        for timeout in (-1, float('nan'), float('inf')):
            with self.assertRaises(ValueError): job.result(timeout=timeout)
        self.assertEqual(job.result(timeout=5)[2], packed('coverage_mip0')); session.close()

    def test_concurrent_source_replacements_are_whole_snapshots(self):
        variants = [coverage_spec(stride=7), coverage_spec(stride=7)]
        variants[1]['coverage'][0] = 1
        session = raw.RasterGraphSession({MA: variants[0]}, workers=2)
        expected = {}
        for variant in variants:
            session.replace_source(MA, variant)
            expected[session.source_info()[MA]['content_sha256']] = variant['coverage'].tobytes()
        def update():
            for i in range(24): session.replace_source(MA, variants[i % 2])
        def render():
            for _ in range(24):
                text = session.export_manifest(MA)
                digest = json.loads(text)['sources'][0]['content_sha256']
                try: result = session.render_coverage_manifest(text, {'tile_size': 1})
                except ValueError as error: self.assertIn('source binding', str(error))
                else: self.assertEqual(result[2], expected[digest])
        with ThreadPoolExecutor(max_workers=3) as pool:
            futures = [pool.submit(update), pool.submit(render), pool.submit(render)]
            for future in futures: future.result(timeout=5)
        session.close()

    def test_replacement_history_pinning_restore_and_lifetime(self):
        session = raw.RasterGraphSession(sources()); rgb, scalar = documents(session)
        history = session.history(rgb, max_revisions=4)
        self.assertEqual(history.render()[2], packed('mixed_mip0'))
        second = history.commit(scalar); self.assertEqual(second, 2)
        for mip in (0, 1, 2):
            options = dict(mip=mip, quality='preview' if mip else 'final', tile_size=2)
            expected = history.render_coverage(options)
            self.assertEqual(expected[2], packed('coverage_mip'+str(mip)))
            self.assertEqual(history.submit_coverage(options).result(timeout=5), expected)
            self.assertEqual(history.submit_coverage_latest('history', options).result(timeout=5), expected)
            self.assertEqual(history.compare_coverage(2, 2, options), (expected, expected))
        for call in (lambda: history.render(), lambda: history.submit(), lambda: history.compare(1, 2),
                     lambda: history.render_coverage(revision=1), lambda: history.submit_coverage(revision=1),
                     lambda: history.compare_coverage(1, 2)):
            with self.assertRaises(ValueError): call()
        saved = history.save(); restored = session.restore_history(saved)
        self.assertEqual(restored.save(), saved)
        self.assertEqual(history.undo(), 1); self.assertEqual(history.redo(), 2)
        old_job = session.submit_coverage_manifest(scalar)
        before = session.source_info(); bad = coverage_spec(); bad['coverage'][0] = 2
        with self.assertRaises(ValueError): session.replace_source(MA, bad)
        self.assertEqual(session.source_info(), before)
        changed = coverage_spec(); changed['coverage'][0] = 1
        session.replace_source(MA, changed)
        with self.assertRaises(ValueError): session.render_coverage_manifest(scalar)
        with self.assertRaises(ValueError): session.restore_history(saved)
        newer = json.loads(scalar); newer['sources'] = json.loads(session.export_manifest(MA))['sources']
        with self.assertRaises(ValueError): history.commit(json.dumps(newer))
        changed_result = session.render_coverage_manifest(json.dumps(newer))
        self.assertNotEqual(changed_result[2], packed('coverage_mip0'))
        self.assertEqual(old_job.result(timeout=5)[2], packed('coverage_mip0'))
        session.close(); self.assertEqual(history.render_coverage()[2], packed('coverage_mip0'))
        pinned = history.submit_coverage(); del session, history, restored; gc.collect()
        self.assertEqual(pinned.result(timeout=5)[2], packed('coverage_mip0'))

    def test_cancellation_close_and_metadata_after_close(self):
        size = 512; spec = dict(coverage=array.array('f', [.25])*(size*size), width=size, height=size)
        session = raw.RasterGraphSession({MA: spec}, cache_bytes=0)
        text = session.export_manifest(MA); history = session.history(text)
        running = session.submit_coverage_manifest(text, {'tile_size': 1})
        running.cancel()
        with self.assertRaises(raw.RenderCancelled): running.result(timeout=5)
        large = history.submit_coverage({'tile_size': 1}); large.cancel()
        with self.assertRaises(raw.RenderCancelled): large.result(timeout=5)
        session.close(); history.close(); session.close(); history.close()
        for call in (lambda: session.render_coverage_manifest(text), lambda: session.submit_coverage_manifest(text),
                     lambda: session.export_manifest(MA), lambda: session.replace_source(MA, spec),
                     lambda: history.render_coverage(), lambda: history.submit_coverage()):
            with self.assertRaisesRegex(RuntimeError, 'closed'): call()
        self.assertEqual(session.source_info()[MA]['kind'], 'coverage_raster_f32')


if __name__ == '__main__':
    unittest.main()
