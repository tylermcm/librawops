"""Working-Y guided-filter mathematical truth and Python manifest acceptance.

Small fixtures use the exact expanded rational kernel. Larger ordinary-scale
integration fixtures use absolute-guide regression with independent fsum means;
neither reference repeats the native centered-difference evaluation. Numerical
tolerances below apply to these selected fixtures, not all finite float32 input.
"""
import array
import copy
import gc
import json
import math
import struct
import sys
import time
import unittest
from fractions import Fraction as F
from pathlib import Path

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw

WEIGHTS = {'prophoto-d50': (.28807112822929337, .00008565396060525903),
           'rec2020-d65': (.26270021201126703, .059301716469861945)}
DEFAULT = dict(radius=3, epsilon=2**-12)
TYPE = 'rawengine.guided_filter_working_y'


def values(result):
    data = array.array('f')
    data.frombytes(result[2])
    return data


def tap(doc, output):
    doc = copy.deepcopy(doc)
    ops = {op['id']: op for op in doc['operations']}
    needed = set()

    def visit(id):
        if id in needed or id not in ops:
            return
        needed.add(id)
        for upstream in ops[id]['inputs'].values():
            visit(upstream)
    visit(output)
    doc.update(output=output, operations=[op for op in doc['operations'] if op['id'] in needed])
    return doc


def source_doc(session):
    doc = json.loads(session.export_manifest())
    return tap(doc, doc['sources'][0]['id'])


def operation(doc, kind, parameters, domain=None, id=90):
    doc = copy.deepcopy(doc)
    domain = domain or ('scene_linear_prophoto_d50' if doc['working_space'] == 'linear_prophoto_d50'
                        else 'scene_linear_rec2020_d65')
    node = dict(id=f'98200000-0000-0000-0000-{id:012d}', type=kind,
                schema_version=1, processing_version=2, enabled=True,
                input_domain=domain, output_domain=domain, inputs=dict(image=doc['output']),
                parameters=copy.deepcopy(parameters), masks={}, blend_mode='normal', opacity=1)
    doc['operations'].append(node)
    doc['output'] = node['id']
    return doc


def add(doc, p=None, domain=None):
    return operation(doc, TYPE, DEFAULT if p is None else p, domain)


def window(i, width, height, radius):
    x, y = i % width, i // width
    return [yy*width+xx for yy in range(max(0, y-radius), min(height, y+radius+1))
            for xx in range(max(0, x-radius), min(width, x+radius+1))]


def rational_float32(value):
    """Round a Fraction directly to binary32 nearest/even, without double rounding."""
    sign = 0x80000000 if value < 0 else 0
    v = abs(value)
    if not v:
        return 0.0
    exponent = v.numerator.bit_length()-v.denominator.bit_length()
    if v < F(2)**exponent:
        exponent -= 1
    units = v / F(2)**max(-149, exponent-23)
    count, remainder = divmod(units.numerator, units.denominator)
    if 2*remainder > units.denominator or (2*remainder == units.denominator and count % 2):
        count += 1
    if exponent < -126:
        bits = count
    else:
        if count == 1 << 24:
            exponent += 1
            count >>= 1
        if exponent > 127:
            raise OverflowError('reference float32 overflow')
        bits = ((exponent+127) << 23) | (count-(1 << 23))
    return struct.unpack('<f', struct.pack('<I', sign | bits))[0]


def exact_kernel_reference(result, p, space):
    width, height = result[:2]
    data = values(result)
    rgb = [list(map(F, data[i:i+3])) for i in range(0, len(data), 3)]
    if p['radius'] == 0:
        return list(data)
    wr, wb = map(F, WEIGHTS[space])
    guide = [wr*r+(1-wr-wb)*g+wb*b for r, g, b in rgb]
    windows = [window(k, width, height, p['radius']) for k in range(width*height)]
    means = [sum((guide[j] for j in ids), F(0))/len(ids) for ids in windows]
    denominators = [sum(((guide[j]-means[k])**2 for j in ids), F(0))/len(ids)+F(p['epsilon'])
                    for k, ids in enumerate(windows)]
    expected = []
    for i, centers in enumerate(windows):
        row = [F(0)]*len(rgb)
        for k in centers:
            for j in windows[k]:
                row[j] += (1+(guide[i]-means[k])*(guide[j]-means[k])/denominators[k])/(len(centers)*len(windows[k]))
        assert sum(row, F(0)) == 1
        expected.extend(rational_float32(sum((weight*pixel[c] for weight, pixel in zip(row, rgb)), F(0)))
                        for c in range(3))
    return expected


def regression_reference(result, p, space):
    width, height = result[:2]
    data = values(result)
    if p['radius'] == 0:
        return list(data)
    rgb = [data[i:i+3] for i in range(0, len(data), 3)]
    wr, wb = WEIGHTS[space]
    guide = [math.fsum((wr*r, (1-wr-wb)*g, wb*b)) for r, g, b in rgb]
    windows = [window(k, width, height, p['radius']) for k in range(width*height)]
    coefficients = []
    for ids in windows:
        mu = math.fsum(guide[j] for j in ids)/len(ids)
        means = [math.fsum(rgb[j][c] for j in ids)/len(ids) for c in range(3)]
        variance = math.fsum((guide[j]-mu)**2 for j in ids)/len(ids)
        a = [math.fsum((guide[j]-mu)*(rgb[j][c]-means[c]) for j in ids)/len(ids)/(variance+p['epsilon'])
             for c in range(3)]
        coefficients.append((mu, means, a))
    return [math.fsum(coefficients[k][1][c]+coefficients[k][2][c]*(guide[i]-coefficients[k][0])
                      for k in ids)/len(ids)
            for i, ids in enumerate(windows) for c in range(3)]


class GuidedFilterTests(unittest.TestCase):
    def assert_map(self, session, base, p=None, req=None, space=None, exact=False, tiles=True):
        p = DEFAULT if p is None else p
        space = space or ('prophoto-d50' if base['working_space'] == 'linear_prophoto_d50' else 'rec2020-d65')
        domain = 'scene_linear_prophoto_d50' if space == 'prophoto-d50' else 'scene_linear_rec2020_d65'
        text = json.dumps(add(base, p, domain))
        original = session.render_manifest(json.dumps(base), req)
        actual = session.render_manifest(text, req)
        self.assertEqual(actual[:2], original[:2])
        expected = (exact_kernel_reference if exact else regression_reference)(original, p, space)
        for v, t in zip(values(actual), expected):
            self.assertAlmostEqual(v, t, delta=2e-7*max(abs(v), abs(t))+2e-10)
        if tiles:
            for size in (1, 3, 64):
                self.assertEqual(session.render_manifest(text, {**(req or {}), 'tile_size': size}), actual)
        return text, actual

    def test_exact_expanded_kernel_borders_signed_headroom_and_impulse(self):
        for width, height in ((1, 1), (1, 5), (5, 1), (4, 3)):
            data = array.array('f', ((i*7 % 19-5)/8 for i in range(width*height*3)))
            for space in WEIGHTS:
                session = raw.RasterSession(data, width, height, space)
                try:
                    for radius, epsilon in ((0, 2**-24), (1, 2**-24), (1, 1), (3, 2**-12), (8, 65536)):
                        self.assert_map(session, source_doc(session), dict(radius=radius, epsilon=epsilon), exact=True)
                finally:
                    session.close()
        for space in WEIGHTS:
            session = raw.RasterSession(array.array('f', [0]*6+[1]*3+[0]*6), 5, 1, space)
            try:
                _, result = self.assert_map(session, source_doc(session), dict(radius=1, epsilon=1), exact=True)
                expected = array.array('f', [v for v in (3/22, 2/11, 5/11, 2/11, 3/22) for c in range(3)])
                self.assertEqual(result[2], expected.tobytes())
            finally:
                session.close()

    def test_identity_extremes_constant_bits_and_nonfinite_source(self):
        maximum = 3.4028234663852886e38
        for space in WEIGHTS:
            for rgb in ((-0., 0., -0.), (2**-149,)*3, (maximum, -maximum, maximum), (-.125, .5, 2.)):
                session = raw.RasterSession(array.array('f', rgb*9), 3, 3, space)
                try:
                    base = source_doc(session)
                    original = session.render_manifest(json.dumps(base))
                    for radius in (0, 1, 8):
                        for epsilon in (2**-24, 65536):
                            self.assertEqual(session.render_manifest(json.dumps(add(base, dict(radius=radius, epsilon=epsilon)))), original)
                finally:
                    session.close()
            data = array.array('f', [-0., maximum, -maximum, 2**-149, -2**-149, 0.])
            session = raw.RasterSession(data, 2, 1, space)
            try:
                base = source_doc(session)
                self.assertEqual(session.render_manifest(json.dumps(add(base, dict(radius=0, epsilon=65536))))[2], data.tobytes())
            finally:
                session.close()
        for v in (math.nan, math.inf, -math.inf):
            data = array.array('f', [.25]*15)
            data[0] = v
            # The Python source boundary rejects nonfinite storage before any
            # manifest can run. Native bad-tile/finite-halo checks have their own
            # C++ gate; Python must not relax its stronger whole-source boundary.
            with self.assertRaises(ValueError):
                raw.RasterSession(data, 5, 1, 'prophoto-d50')

    def test_geometry_roi_composed_support_histogram_and_analysis(self):
        session = raw.RasterSession(array.array('f', ((i*37 % 257-60)/128 for i in range(11*9*3))), 11, 9, 'prophoto-d50')
        try:
            base = source_doc(session)
            for kind, p in (('crop', dict(x=2, y=1, width=7, height=7)),
                            ('orientation', dict(quarter_turns=1, flip_horizontal=True, flip_vertical=False)),
                            ('resize', dict(width=7, height=5, filter='bilinear')),
                            ('convolution', dict(width=3, height=3, coefficients=[0, .125, 0, .125, .5, .125, 0, .125, 0], border='replicate'))):
                transformed = operation(base, 'rawengine.'+kind, p, id=80)
                for mip in (0, 1, 2):
                    self.assert_map(session, transformed, dict(radius=1, epsilon=2**-12),
                                    dict(mip=mip, quality='preview' if mip else 'final'))
            text, full = self.assert_map(session, base, dict(radius=1, epsilon=1))
            req = dict(x=4, y=3, roi_width=1, roi_height=1)
            self.assertEqual(session.render_manifest(text, req)[2], full[2][(3*11+4)*12:(3*11+5)*12])
            self.assertEqual(list(session.required_source_regions(text, req).values()), [(2, 1, 5, 5)])
            identity = json.dumps(add(base, dict(radius=0, epsilon=1)))
            self.assertEqual(list(session.required_source_regions(identity, req).values()), [(4, 3, 1, 1)])
            convolution = operation(base, 'rawengine.convolution', dict(width=3, height=3, coefficients=[1/9]*9, border='replicate'), id=80)
            composed = json.dumps(add(convolution, dict(radius=1, epsilon=1)))
            self.assertEqual(list(session.required_source_regions(composed, req).values()), [(1, 0, 7, 7)])
            pieces = []
            session.analyze_local_manifest(text, pieces.append, req, radius=1)
            self.assertEqual(len(pieces), 1)
            means = array.array('d')
            means.frombytes(pieces[0]['mean_f64'])
            data = values(full)
            for c in range(3):
                self.assertAlmostEqual(means[c], math.fsum(data[(y*11+x)*3+c] for y in range(2, 5) for x in range(3, 6))/9, delta=1e-12)
            self.assertEqual(session.histogram_manifest(text)['descriptor']['primaries'], 'prophoto')
        finally:
            session.close()

    def test_actual_output_overflow_propagates_and_jobs_recover(self):
        # Frozen strict fixture488: finite signed float32 data can produce an
        # actual out-of-range correction because kernel weights need not be
        # positive. Radius0 accepts exactly these source bits; radius1 rejects.
        bits = [2130706431, 2139095039, 4286578687,
                4286578687, 2139095039, 2130706431,
                4278190079, 4286578687, 2130706431,
                2130706431, 2130706431, 2139095039,
                2130706431, 4286578687, 4278190079]
        data = array.array('f')
        data.frombytes(struct.pack('<15I', *bits))
        session = raw.RasterSession(data, 5, 1, 'prophoto-d50')
        try:
            base = source_doc(session)
            identity = json.dumps(add(base, dict(radius=0, epsilon=2**-24)))
            active = json.dumps(add(base, dict(radius=1, epsilon=2**-24)))
            self.assertEqual(session.render_manifest(identity)[2], data.tobytes())
            with self.assertRaises(ValueError):
                session.render_manifest(active)
            job = session.submit_manifest(active)
            with self.assertRaisesRegex(RuntimeError, 'guided filter float32 output overflow'):
                job.result(timeout=5)
            self.assertEqual(session.submit_manifest(identity).result(timeout=5)[2], data.tobytes())
        finally:
            session.close()

    def test_requested_reduction_order_odd_extents_and_levels(self):
        for space in WEIGHTS:
            session = raw.RasterSession(array.array('f', ((i*37 % 257-60)/128 for i in range(9*7*3))), 9, 7, space)
            try:
                base = source_doc(session)
                for p in (dict(radius=0, epsilon=1), dict(radius=1, epsilon=2**-24), DEFAULT, dict(radius=8, epsilon=65536)):
                    text, native = self.assert_map(session, base, p, tiles=False)
                    filtered = raw.RasterSession(values(native), 9, 7, space)
                    try:
                        for mip in (1, 2):
                            req = dict(mip=mip, quality='preview')
                            before = session.render_manifest(json.dumps(base), req)
                            _, actual = self.assert_map(session, base, p, req)
                            self.assertEqual(actual[:2], ((9+(1 << mip)-1) >> mip, (7+(1 << mip)-1) >> mip))
                            reduced = raw.RasterSession(values(before), *before[:2], space)
                            try:
                                self.assertEqual(reduced.render_manifest(json.dumps(add(source_doc(reduced), p)))[2], actual[2])
                            finally:
                                reduced.close()
                            after = filtered.render_manifest(json.dumps(source_doc(filtered)), req)
                            if p['radius']:
                                self.assertNotEqual(after[2], actual[2])
                            else:
                                self.assertEqual(after[2], actual[2])
                    finally:
                        filtered.close()
                for req in (dict(mip=1, quality='final'), dict(mip=3, quality='preview'), dict(x=9, roi_width=1)):
                    with self.assertRaises(ValueError):
                        session.render_manifest(text, req)
            finally:
                session.close()

    def test_all_parameters_cache_history_jobs_and_owner_lifetimes(self):
        data = array.array('f', ((i*17 % 53-10)/32 for i in range(9*7*3)))
        session = raw.RasterSession(data, 9, 7, 'rec2020-d65', workers=2)
        try:
            base = source_doc(session)
            p = dict(radius=1, epsilon=2**-12)
            text = json.dumps(add(base, p))
            req = dict(tile_size=3)
            original = session.render_manifest(text, req)
            before = session.cache_stats()
            data[0] = 999
            p.clear()
            self.assertEqual(session.submit_manifest(text, req).result(timeout=5), original)
            self.assertEqual(session.cache_stats()['misses'], before['misses'])
            history = session.history(text)
            for controls in (dict(radius=3, epsilon=2**-12), dict(radius=1, epsilon=1),
                             dict(radius=0, epsilon=2**-12), dict(radius=0, epsilon=1)):
                revised_text = json.dumps(add(base, controls))
                revised = session.render_manifest(revised_text, req)
                if controls['radius']:
                    self.assertNotEqual(revised, original)
                after = session.cache_stats()
                self.assertGreater(after['misses'], before['misses'])
                self.assertEqual(session.render_manifest(revised_text, req), revised)
                self.assertEqual(session.cache_stats()['misses'], after['misses'])
                revision = history.commit(revised_text)
                self.assertEqual(history.render(req, revision=revision), revised)
                before = session.cache_stats()
            self.assertEqual(session.submit_manifest_latest('view', revised_text, req).result(timeout=5), revised)
            self.assertEqual([j.result(timeout=5) for j in [session.submit_manifest(text, req) for _ in range(4)]], [original]*4)
            disabled = add(base, dict(radius=8, epsilon=65536))
            disabled['operations'][-1]['enabled'] = False
            disabled_text = json.dumps(disabled)
            upstream = session.render_manifest(json.dumps(base), req)
            before = session.cache_stats()
            self.assertEqual(session.render_manifest(disabled_text, req), upstream)
            self.assertEqual(session.cache_stats()['misses'], before['misses'])
            history.commit(disabled_text)
            restored = session.restore_history(history.save())
            self.assertEqual(restored.render(req), upstream)
            saved_manifest = json.loads(restored.save())['revisions'][-1]['manifest']
            self.assertFalse(json.loads(saved_manifest)['operations'][-1]['enabled'])
            job = restored.submit(req)
            history.close()
            del history
            del session
            gc.collect()
            self.assertEqual(job.result(timeout=5), upstream)
            restored.close()
        finally:
            if 'session' in locals():
                session.close()

    def test_running_cancellation_latest_replacement_and_recovery(self):
        session = raw.RasterSession(array.array('f', [.2, .4, .6])*(128*128), 128, 128, 'prophoto-d50',
                                    workers=1, max_pending=2, cache_bytes=0)
        try:
            text = json.dumps(add(source_doc(session), dict(radius=1, epsilon=1)))
            busy = session.submit_manifest(text, dict(tile_size=1), priority='background')
            deadline = time.monotonic()+5
            while busy.progress()['completed_tiles'] == 0 and not busy.done():
                if time.monotonic() >= deadline:
                    busy.cancel()
                    self.fail('guided-filter worker did not reach its first tile')
                time.sleep(.001)
            self.assertFalse(busy.done())
            previous = session.submit_manifest_latest('view', text, dict(x=1, y=1, roi_width=3, roi_height=3))
            request = dict(x=2, y=2, roi_width=3, roi_height=3)
            newest = session.submit_manifest_latest('view', text, request)
            try:
                with self.assertRaises(raw.RenderCancelled):
                    previous.result(timeout=5)
                self.assertEqual(previous.progress()['completed_tiles'], 0)
            finally:
                busy.cancel()
            with self.assertRaises(raw.RenderCancelled):
                busy.result(timeout=5)
            self.assertLess(busy.progress()['completed_tiles'], busy.progress()['total_tiles'])
            self.assertEqual(newest.result(timeout=5), session.render_manifest(text, request))
        finally:
            session.close()

    def test_calibrated_raw_both_demosaicers_spaces_and_source_footprints(self):
        data = array.array('H', ((i*719) % 50000 for i in range(99)))
        meta = dict(active_x=1, active_y=1, active_width=9, active_height=7,
                    black_levels=[1000]*4, white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear', 'rawengine.menon_base'):
            session = raw.RawSession(data, 11, 9, meta, demosaic=dict(algorithm=algorithm, processing_version=1))
            try:
                for space in WEIGHTS:
                    doc = json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6, .2, .1, .2, .6, .1, .1, .1, .6), working_space=space, output_mode='srgb-preview')))
                    base = tap(doc, next(op['id'] for op in doc['operations'] if op['type'] == 'rawengine.camera_to_working'))
                    for mip in (0, 1, 2):
                        req = dict(mip=mip, quality='preview' if mip else 'final')
                        text, out = self.assert_map(session, base, dict(radius=1, epsilon=2**-12), req, space)
                        self.assertEqual(session.submit_manifest(text, req).result(timeout=5), out)
                        self.assertEqual(session.history(text).render(req), out)
                    # 2r reduced support maps through active-origin native bounds;
                    # demosaic support then clips to the true9x7 active area.
                    req = dict(x=2, y=1, roi_width=1, roi_height=1, mip=1, quality='preview')
                    self.assertEqual(list(session.required_source_regions(text, req).values()), [(1, 1, 9, 7)])
                with self.assertRaises(ValueError):
                    session.render_manifest(json.dumps(add(source_doc(session), DEFAULT, 'camera_linear')))
            finally:
                session.close()

    def test_multisource_conversion_replacement_and_pinned_history(self):
        a, b = '98200000-0000-0000-0000-000000000001', '98200000-0000-0000-0000-000000000002'
        spec = dict(rgb=array.array('f', ((i % 19)/16 for i in range(7*5*3))), width=7, height=5, working_space='prophoto-d50')
        session = raw.RasterGraphSession({a: spec, b: {**spec, 'rgb': array.array('f', [.5]*105)}})
        try:
            base = tap(json.loads(session.export_manifest(a)), a)
            mixed = operation(base, 'rawengine.linear_mix', dict(amount=.25), id=80)
            mixed['operations'][-1]['inputs'] = dict(base=a, layer=b)
            converted = operation(mixed, 'rawengine.working_space_convert', {}, id=81)
            converted['operations'][-1]['output_domain'] = 'scene_linear_rec2020_d65'
            for mip in (0, 1, 2):
                req = dict(mip=mip, quality='preview' if mip else 'final')
                text, original = self.assert_map(session, converted, dict(radius=1, epsilon=1), req, 'rec2020-d65')
                self.assertEqual(set(session.required_source_regions(text, req)), {a, b})
            history = session.history(text)
            pinned = history.render(req)
            session.replace_source(a, {**spec, 'rgb': array.array('f', [.125]*105)})
            self.assertEqual(history.render(req), pinned)
            with self.assertRaises(ValueError):
                session.render_manifest(text, req)
            revised = json.loads(text)
            revised['sources'] = json.loads(session.export_manifest(a))['sources']
            self.assertNotEqual(session.render_manifest(json.dumps(revised), req), pinned)
            history.close()
        finally:
            session.close()

    def test_strict_parameters_versions_domains_and_disabled_validation(self):
        session = raw.RasterSession(array.array('f', [.2, .4, .6]), 1, 1, 'prophoto-d50')
        base = source_doc(session)
        try:
            invalid = [{}, dict(DEFAULT, extra=0), dict(radius=3), dict(epsilon=1)]
            invalid += [dict(DEFAULT, radius=v) for v in (True, None, '1', [], {}, -1, 1.5, 9, 2**40, math.nan, math.inf)]
            invalid += [dict(DEFAULT, epsilon=v) for v in (False, None, '1', [], {}, 0, -0., -1, math.nextafter(2**-24, 0), math.nextafter(65536., math.inf), math.nan, math.inf)]
            for p in invalid:
                for enabled in (True, False):
                    doc = add(base, p)
                    doc['operations'][-1]['enabled'] = enabled
                    with self.assertRaises(ValueError):
                        session.render_manifest(json.dumps(doc))
            changes = [{'schema_version': 2}, {'processing_version': 1}, {'processing_version': 3},
                       {'input_domain': 'scene_linear_rec2020_d65'}, {'output_domain': 'scene_linear_rec2020_d65'},
                       {'opacity': .5}, {'blend_mode': 'multiply'}, {'inputs': {}},
                       {'inputs': dict(image=base['output'], other=base['output'])},
                       {'masks': dict(test=base['output'])}, {'extensions': dict(test=1)}]
            for change in changes:
                for enabled in (True, False):
                    doc = add(base)
                    doc['operations'][-1].update(change, enabled=enabled)
                    with self.assertRaises(ValueError):
                        session.render_manifest(json.dumps(doc))
            for radius in (0, 0., 1., 8.):
                session.render_manifest(json.dumps(add(base, dict(radius=radius, epsilon=1))))
            disabled = add(base)
            disabled['operations'][-1]['enabled'] = False
            self.assertEqual(session.render_manifest(json.dumps(disabled)), session.render_manifest(json.dumps(base)))
            encoded = json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):
                session.render_manifest(json.dumps(add(encoded, DEFAULT, 'encoded_srgb')))
            bad = json.dumps(add(base, dict(radius=9, epsilon=1)))
            for call in (lambda: session.submit_manifest(bad), lambda: session.submit_manifest_latest('view', bad), lambda: session.history(bad)):
                with self.assertRaises(ValueError):
                    call()
        finally:
            session.close()
        with self.assertRaises(RuntimeError):
            session.render_manifest(json.dumps(add(base)))

    @unittest.skipUnless(raw.icc_available(), 'optional ICC backend disabled')
    def test_real_icc_output_jobs_history_and_analysis(self):
        profile = raw.create_icc_profile((Path(__file__).parent/'reference/profiles/sRGB2014.icc').read_bytes())
        for space in WEIGHTS:
            session = raw.RasterSession(array.array('f', ((i % 23-5)/16 for i in range(5*3*3))), 5, 3, space, output_profile=profile)
            try:
                point = add(source_doc(session), dict(radius=1, epsilon=1))['operations'][-1]

                def output(mode):
                    doc = json.loads(session.export_manifest(dict(output_mode=mode)))
                    source = doc['sources'][0]['id']
                    for op in doc['operations']:
                        for key, target in op['inputs'].items():
                            if target == source:
                                op['inputs'][key] = point['id']
                    doc['operations'].insert(0, point)
                    return json.dumps(doc)
                linear = session.render_manifest(json.dumps(add(source_doc(session), dict(radius=1, epsilon=1))))
                icc, srgb = output('icc-display'), output('srgb-preview')
                actual = session.render_manifest(icc)
                self.assertEqual(actual, raw.render_raster(values(linear), 5, 3, dict(working_space=space, output_mode='icc-display', output_profile=profile)))
                for tile in (1, 3, 64):
                    self.assertEqual(session.render_manifest(icc, dict(tile_size=tile)), actual)
                self.assertEqual(session.submit_manifest(icc).result(timeout=5), actual)
                self.assertEqual(session.histogram_manifest(icc)['descriptor']['profile_sha256'], raw.icc_profile_info(profile)['profile_sha256'])
                pieces = []
                session.analyze_local_manifest(icc, pieces.append)
                self.assertTrue(pieces)
                history = session.history(srgb)
                first = history.stats()['current_id']
                second = history.commit(icc)
                self.assertEqual(history.compare(first, second), (session.render_manifest(srgb), actual))
                restored = session.restore_history(history.save())
                self.assertEqual(restored.render(), actual)
                self.assertEqual(restored.render(revision=first), session.render_manifest(srgb))
                restored.close()
                history.close()
                with self.assertRaises(ValueError):
                    session.render_manifest(icc, dict(mip=1, quality='preview'))
            finally:
                session.close()


if __name__ == '__main__':
    unittest.main()
