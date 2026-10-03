"""Independent scene-linear saturation truth and saved-graph integration."""
import array
import copy
from fractions import Fraction
import json
import math
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw


def weights(space):
    # Solve primary scaling exactly from the engine's declared chromaticities,
    # rather than duplicating its green-anchored Y arithmetic/constants.
    coords, white = ((('.7347','.2653','.1596','.8404','.0366','.0001'),('.3457','.3585'))
                     if space == 'prophoto-d50' else
                     (('.708','.292','.170','.797','.131','.046'),('.3127','.3290')))
    p = list(map(Fraction, coords)); wx, wy = map(Fraction, white)
    matrix = [[p[2*c]/p[2*c+1] for c in range(3)], [Fraction(1)]*3,
              [(1-p[2*c]-p[2*c+1])/p[2*c+1] for c in range(3)]]
    result = [wx/wy, Fraction(1), (1-wx-wy)/wy]
    for k in range(3):
        pivot = matrix[k][k]
        matrix[k] = [v/pivot for v in matrix[k]]; result[k] /= pivot
        for i in range(3):
            if i != k:
                factor = matrix[i][k]
                matrix[i] = [a-factor*b for a,b in zip(matrix[i], matrix[k])]
                result[i] -= factor*result[k]
    return result


def values(result):
    data = array.array('f'); data.frombytes(result[2]); return data


def tap(doc, output):
    doc = copy.deepcopy(doc); ops = {o['id']:o for o in doc['operations']}; needed = set()
    def visit(id):
        if id not in ops or id in needed: return
        needed.add(id)
        for upstream in ops[id]['inputs'].values(): visit(upstream)
    visit(output)
    doc.update(output=output, operations=[o for o in doc['operations'] if o['id'] in needed])
    return doc


def source_doc(session):
    doc = json.loads(session.export_manifest())
    return tap(doc, doc['sources'][0]['id'])


def add(doc, amount, id='70000000-0000-0000-0000-000000000090', domain=None):
    doc = copy.deepcopy(doc)
    domain = domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    doc['operations'].append(dict(id=id, type='rawengine.saturation', schema_version=1, processing_version=2,
        enabled=True, input_domain=domain, output_domain=domain, inputs=dict(image=doc['output']),
        parameters=dict(amount=amount), masks={}, blend_mode='normal', opacity=1))
    doc['output'] = id
    return doc


class SaturationTests(unittest.TestCase):
    def test_saved_amount_keeps_double_precision_near_identity(self):
        amount=1-1e-8
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(array.array('f',[1,0,0]),1,1,space)
            try:
                base=source_doc(session);text=json.dumps(add(base,amount))
                actual=session.render_manifest(text);identity=session.render_manifest(json.dumps(add(base,1)))
                self.assertNotEqual(actual,identity)
                y=weights(space)[0];expected=float((1-Fraction(amount))*y)
                self.assertAlmostEqual(values(actual)[1],expected,delta=1e-16)
                for mip in (0,1,2):
                    req=dict(mip=mip,quality='preview' if mip else 'final')
                    self.assertEqual(session.render_manifest(text,req),actual)
                self.assertEqual(session.submit_manifest(text).result(timeout=5),actual)
                history=session.history(text);history.commit(json.dumps(add(base,1)));self.assertTrue(history.undo());self.assertEqual(history.render(),actual);history.close()
            finally:session.close()

    def assert_map(self, session, base, amount, space, request=None):
        text = json.dumps(add(base, amount))
        original = session.render_manifest(json.dumps(base), request)
        actual = session.render_manifest(text, request)
        self.assertEqual(actual[:2], original[:2])
        before, after = values(original), values(actual)
        w = weights(space)
        for offset in range(0, len(before), 3):
            rgb = before[offset:offset+3]
            y = sum(weight*Fraction(float(sample)) for weight,sample in zip(w, rgb))
            for c in range(3):
                expected = float((1-Fraction(amount))*y+Fraction(amount)*Fraction(float(rgb[c])))
                self.assertAlmostEqual(after[offset+c], expected, delta=4e-7*max(1,abs(expected)))
            actual_y = sum(float(weight)*after[offset+c] for c,weight in enumerate(w))
            self.assertAlmostEqual(actual_y, float(y), delta=8e-7*max(1,max(map(abs,rgb))))
            if amount == 0: self.assertEqual(after[offset],after[offset+1]); self.assertEqual(after[offset],after[offset+2])
        for tile in (1, 3, 64):
            self.assertEqual(session.render_manifest(text, {**(request or {}), 'tile_size':tile}), actual)
        if amount == 1: self.assertEqual(actual,original)
        return text,actual

    def test_working_space_truth_signed_headroom_neutrals_and_identity(self):
        data = array.array('f',[1,0,0,0,1,0,0,0,1,-1,.5,2,3,-2,1,-.25,-.25,-.25])
        outputs = []
        for space in ('prophoto-d50','rec2020-d65'):
            session = raw.RasterSession(data,6,1,space)
            try:
                for amount in (0,.5,1,1.75,4):
                    text,result = self.assert_map(session,source_doc(session),amount,space)
                    if amount == 0: outputs.append(result[2])
                    if amount == 4: self.assertTrue(any(v<0 for v in values(result))); self.assertTrue(any(v>1 for v in values(result)))
            finally: session.close()
            extreme = array.array('f',[-0.0,0.0,-0.0,3.4028234663852886e38,-3.4028234663852886e38,1e-30])
            session = raw.RasterSession(extreme,2,1,space)
            self.assertEqual(session.render_manifest(json.dumps(add(source_doc(session),1)))[2],extreme.tobytes())
            session.close()
            neutral = array.array('f',[-0.0,0.0,-0.0,1e30,1e30,1e30,-1e30,-1e30,-1e30])
            session = raw.RasterSession(neutral,3,1,space)
            for amount in (0,.5,4): self.assertEqual(session.render_manifest(json.dumps(add(source_doc(session),amount)))[2],neutral.tobytes())
            session.close()
        self.assertNotEqual(outputs[0],outputs[1])

    def test_geometry_reduction_composition_and_footprints(self):
        data = array.array('f',((i*37%257-60)/128 for i in range(11*9*3)))
        session = raw.RasterSession(data,11,9,'prophoto-d50')
        try:
            doc = json.loads(session.export_manifest(dict(crop=(1,1,9,7),rotate=90,resize=(9,5),resize_filter='area',output_mode='srgb-preview')))
            base = tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.resize'))
            for mip in (0,1,2):
                req = dict(mip=mip, quality='preview' if mip else 'final')
                text,result = self.assert_map(session,base,1.75,'prophoto-d50',req)
                roi = {**req,'x':1,'y':0,'roi_width':1,'roi_height':1}
                self.assertEqual(session.render_manifest(text,roi)[2],result[2][12:24])
                self.assertEqual(session.required_source_regions(text,roi),session.required_source_regions(json.dumps(base),roi))
            # The requested-level map is defined over float32 reduced inputs;
            # comparisons do not assume native-map-before-reduction is bit exact.
            self.assert_map(session,source_doc(session),0,'prophoto-d50',dict(mip=2,quality='preview'))
        finally: session.close()

    def test_jobs_cache_history_analysis_and_copied_ownership(self):
        data = array.array('f',[(i%29-10)/8 for i in range(9*7*3)])
        session = raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base = source_doc(session); text = json.dumps(add(base,.5)); req = dict(tile_size=3)
            original = session.render_manifest(text,req); before = session.cache_stats()
            self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original)
            self.assertEqual(session.cache_stats()['misses'],before['misses'])
            revised_text = json.dumps(add(base,1.75)); revised = session.render_manifest(revised_text,req)
            after = session.cache_stats()
            self.assertGreater(after['hits'],before['hits']); self.assertGreater(after['misses'],before['misses'])
            self.assertNotEqual(original,revised)
            self.assertEqual(session.submit_manifest_latest('view',revised_text,req).result(timeout=5),revised)
            history = session.history(text); history.commit(revised_text)
            self.assertEqual(history.render(req),revised); self.assertTrue(history.undo()); self.assertEqual(history.render(req),original)
            saved = history.save(); restored = session.restore_history(saved); self.assertEqual(restored.render(req),original)
            self.assertEqual(session.histogram_manifest(text)['descriptor']['primaries'],'rec2020')
            tiles=[]; session.analyze_local_manifest(text,tiles.append,req,radius=1); self.assertTrue(tiles)
            data[0]=999; self.assertEqual(session.render_manifest(text,req),original)
            restored.close(); history.close()
        finally: session.close()

    def test_calibrated_raw_both_demosaicers_and_working_conversion(self):
        data = array.array('H',((i*719)%50000 for i in range(99)))
        meta = dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            session = raw.RawSession(data,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                for space in ('prophoto-d50','rec2020-d65'):
                    doc = json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),working_space=space,output_mode='srgb-preview')))
                    base = tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
                    for mip in (0,1,2):
                        self.assert_map(session,base,1.75,space,dict(mip=mip,quality='preview' if mip else 'final'))
                with self.assertRaises(ValueError): session.render_manifest(json.dumps(add(source_doc(session),0,domain='camera_linear')))
            finally: session.close()

    def test_multisource_mix_and_working_space_conversion(self):
        a,b = '70000000-0000-0000-0000-000000000001','70000000-0000-0000-0000-000000000002'
        session = raw.RasterGraphSession({a:dict(rgb=array.array('f',[-1,.5,2]*9),width=3,height=3,working_space='prophoto-d50'),
            b:dict(rgb=array.array('f',[.25,.75,1.5]*9),width=3,height=3,working_space='prophoto-d50')})
        try:
            base = json.loads(session.export_manifest(a))
            mix = dict(id='70000000-0000-0000-0000-000000000080',type='rawengine.linear_mix',schema_version=1,
                processing_version=2,enabled=True,input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_prophoto_d50',
                inputs=dict(base=a,layer=b),parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1)
            base['operations']=[mix]; base['output']=mix['id']
            converted = dict(id='70000000-0000-0000-0000-000000000081',type='rawengine.working_space_convert',schema_version=1,
                processing_version=2,enabled=True,input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_rec2020_d65',
                inputs=dict(image=base['output']),parameters={},masks={},blend_mode='normal',opacity=1)
            base['operations'].append(converted); base['output']=converted['id']
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final')
                edited=add(base,.5,domain='scene_linear_rec2020_d65'); text=json.dumps(edited)
                upstream=values(session.render_manifest(json.dumps(base),req)); output=values(session.render_manifest(text,req))
                w=weights('rec2020-d65')
                for offset in range(0,len(output),3):
                    rgb=upstream[offset:offset+3]; y=sum(float(weight)*sample for weight,sample in zip(w,rgb))
                    for c in range(3): self.assertAlmostEqual(output[offset+c],.5*y+.5*rgb[c],delta=5e-7)
                self.assertEqual(session.render_manifest(text,{**req,'tile_size':1}),session.render_manifest(text,req))
                regions=session.required_source_regions(text,req)
                self.assertEqual(set(regions),{a,b})
                self.assertEqual(regions,session.required_source_regions(json.dumps(base),req))
        finally: session.close()

    def test_strict_parameters_versions_domains_nonfinite_and_overflow(self):
        session = raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50')
        base = source_doc(session)
        try:
            mutations = [dict(parameters=p) for p in ({},{'amount':-.1},{'amount':4.1},{'amount':True},{'amount':[1]}, {'amount':1,'extra':0})]
            mutations += [dict(schema_version=2),dict(processing_version=1),dict(processing_version=3)]
            for mutation in mutations:
                for enabled in (True,False):
                    doc = add(base,1); doc['operations'][-1].update(mutation,enabled=enabled)
                    with self.assertRaises(ValueError): session.render_manifest(json.dumps(doc))
            disabled = add(base,4); disabled['operations'][-1]['enabled']=False
            self.assertEqual(session.render_manifest(json.dumps(disabled)),session.render_manifest(json.dumps(base)))
            encoded = json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(add(encoded,0,domain='encoded_srgb')))
        finally: session.close()
        for bad in (float('nan'),float('inf'),-float('inf')):
            with self.assertRaises(ValueError):
                raw.RasterSession(array.array('f',[1,bad,1]),1,1,'prophoto-d50')
        session = raw.RasterSession(array.array('f',[3.4028234663852886e38,-3.4028234663852886e38,0]),1,1,'prophoto-d50')
        with self.assertRaises(ValueError): session.render_manifest(json.dumps(add(source_doc(session),4)))
        text = json.dumps(add(source_doc(session),1)); session.close()
        with self.assertRaises(RuntimeError): session.render_manifest(text)


if __name__ == '__main__':
    unittest.main()
