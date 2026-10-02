"""Independent exact-rational matrix truth and native saved-graph integration."""
import array
import copy
from fractions import Fraction
import json
import struct
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw

IDENTITY = [1,0,0,0,1,0,0,0,1]
SWAP = [0,1,0,0,0,1,1,0,0]
MIX = [1.25,-.25,.125,.25,.5,.25,-.5,.25,1.25]
GRAY = [.25,.5,.25]*3

def values(result):
    data=array.array('f');data.frombytes(result[2]);return data

def tap(doc, output):
    doc=copy.deepcopy(doc);ops={o['id']:o for o in doc['operations']};needed=set()
    def visit(id):
        if id not in ops or id in needed:return
        needed.add(id)
        for upstream in ops[id]['inputs'].values():visit(upstream)
    visit(output)
    doc.update(output=output,operations=[o for o in doc['operations'] if o['id'] in needed])
    return doc

def source_doc(session):
    doc=json.loads(session.export_manifest());return tap(doc,doc['sources'][0]['id'])

def add(doc, matrix, domain=None):
    doc=copy.deepcopy(doc)
    domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    node=dict(id='90000000-0000-0000-0000-000000000090',type='rawengine.channel_mixer',
        schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,
        inputs=dict(image=doc['output']),parameters=dict(matrix=list(matrix)),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc

class ChannelMixerTests(unittest.TestCase):
    def assert_map(self, session, base, matrix, request=None):
        text=json.dumps(add(base,matrix))
        original=session.render_manifest(json.dumps(base),request)
        actual=session.render_manifest(text,request)
        self.assertEqual(actual[:2],original[:2])
        before,after=values(original),values(actual)
        for offset in range(0,len(before),3):
            rgb=before[offset:offset+3]
            for row in range(3):
                # Exact sum over binary input/coefficient rationals, independent
                # of the native dot-product evaluation and row bypass strategy.
                exact=sum(Fraction(float(matrix[row*3+c]))*Fraction(float(rgb[c])) for c in range(3))
                expected=float(exact)
                self.assertAlmostEqual(after[offset+row],expected,delta=4e-7*max(1,abs(expected)))
        for tile in (1,3,64):
            self.assertEqual(session.render_manifest(text,{**(request or {}),'tile_size':tile}),actual)
        if matrix==IDENTITY:self.assertEqual(actual,original)
        return text,actual

    def test_exact_truth_signed_headroom_neutral_and_coefficient_limits(self):
        data=array.array('f',[1,0,0,0,1,0,0,0,1,-1,.5,2,3,-2,1,-.25,-.25,-.25])
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(data,6,1,space)
            try:
                for matrix in (IDENTITY,SWAP,MIX,GRAY,[0]*9,[64,-64,0,0,0,0,0,0,-64]):
                    self.assert_map(session,source_doc(session),matrix)
                output=values(session.render_manifest(json.dumps(add(source_doc(session),MIX))))
                self.assertTrue(any(v<0 for v in output));self.assertTrue(any(v>1 for v in output))
                self.assertNotEqual(list(output[-3:]),[-.25]*3) # No implicit row normalization.
            finally:session.close()

    def test_identity_unit_rows_and_permutations_preserve_extreme_bits(self):
        for space in ('prophoto-d50','rec2020-d65'):
            data=array.array('f',[-0.,0.,-0.,3.4028234663852886e38,-3.4028234663852886e38,2**-149,2**-149,-0.,-2**-149])
            session=raw.RasterSession(data,3,1,space)
            try:
                for matrix in (IDENTITY,[1,-0.,0,0,1,-0.,-0.,0,1],SWAP,[0,1,0,0,1,0,0,1,0]):
                    output=session.render_manifest(json.dumps(add(source_doc(session),matrix)))[2]
                    expected=b''.join(data.tobytes()[p*12+c*4:p*12+c*4+4]
                        for p in range(3) for row in range(3) for c in [matrix[row*3:row*3+3].index(1)])
                    self.assertEqual(output,expected)
                matrix=[.5,.25,.25,0,1,0,1,0,0]
                output=session.render_manifest(json.dumps(add(source_doc(session),matrix)))[2]
                for p in range(3):
                    self.assertEqual(output[p*12+4:p*12+8],data.tobytes()[p*12+4:p*12+8])
                    self.assertEqual(output[p*12+8:p*12+12],data.tobytes()[p*12:p*12+4])
            finally:session.close()

    def test_geometry_reduced_roi_and_no_added_footprint(self):
        data=array.array('f',((i*37%257-60)/128 for i in range(11*9*3)))
        session=raw.RasterSession(data,11,9,'prophoto-d50')
        try:
            doc=json.loads(session.export_manifest(dict(crop=(1,1,9,7),rotate=90,resize=(9,5),resize_filter='area',output_mode='srgb-preview')))
            base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.resize'))
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final')
                text,result=self.assert_map(session,base,MIX,req)
                roi={**req,'x':1,'y':0,'roi_width':1,'roi_height':1}
                self.assertEqual(session.render_manifest(text,roi)[2],result[2][12:24])
                self.assertEqual(session.required_source_regions(text,roi),session.required_source_regions(json.dumps(base),roi))
        finally:session.close()

    def test_jobs_cache_history_analysis_and_ownership(self):
        data=array.array('f',[(i%29-10)/8 for i in range(9*7*3)])
        session=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(session);matrix=list(MIX);text=json.dumps(add(base,matrix));req=dict(tile_size=3)
            original=session.render_manifest(text,req);matrix[0]=999;before=session.cache_stats()
            self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original)
            self.assertEqual(session.cache_stats()['misses'],before['misses'])
            revised_text=json.dumps(add(base,SWAP));revised=session.render_manifest(revised_text,req);after=session.cache_stats()
            self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses'])
            self.assertNotEqual(original,revised)
            self.assertEqual(session.submit_manifest_latest('view',revised_text,req).result(timeout=5),revised)
            history=session.history(text);history.commit(revised_text)
            self.assertEqual(history.render(req),revised);self.assertTrue(history.undo());self.assertEqual(history.render(req),original)
            restored=session.restore_history(history.save());self.assertEqual(restored.render(req),original)
            self.assertEqual(session.histogram_manifest(text)['descriptor']['primaries'],'rec2020')
            tiles=[];session.analyze_local_manifest(text,tiles.append,req,radius=1);self.assertTrue(tiles)
            data[0]=999;self.assertEqual(session.render_manifest(text,req),original)
            restored.close();history.close()
        finally:session.close()

    def test_calibrated_raw_and_both_demosaicers(self):
        data=array.array('H',((i*719)%50000 for i in range(99)))
        meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            session=raw.RawSession(data,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                for space in ('prophoto-d50','rec2020-d65'):
                    doc=json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),working_space=space,output_mode='srgb-preview')))
                    base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
                    for mip in (0,1,2):
                        self.assert_map(session,base,MIX,dict(mip=mip,quality='preview' if mip else 'final'))
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(source_doc(session),IDENTITY,domain='camera_linear')))
            finally:session.close()

    def test_multisource_and_actual_upstream_working_conversion(self):
        a,b='90000000-0000-0000-0000-000000000001','90000000-0000-0000-0000-000000000002'
        session=raw.RasterGraphSession({a:dict(rgb=array.array('f',[-1,.5,2]*9),width=3,height=3,working_space='prophoto-d50'),
            b:dict(rgb=array.array('f',[.25,.75,1.5]*9),width=3,height=3,working_space='prophoto-d50')})
        try:
            base=json.loads(session.export_manifest(a))
            mix=dict(id='90000000-0000-0000-0000-000000000080',type='rawengine.linear_mix',schema_version=1,
                processing_version=2,enabled=True,input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_prophoto_d50',
                inputs=dict(base=a,layer=b),parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1)
            base['operations']=[mix];base['output']=mix['id']
            converted=dict(id='90000000-0000-0000-0000-000000000081',type='rawengine.working_space_convert',schema_version=1,
                processing_version=2,enabled=True,input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_rec2020_d65',
                inputs=dict(image=base['output']),parameters={},masks={},blend_mode='normal',opacity=1)
            base['operations'].append(converted);base['output']=converted['id']
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final')
                edited=add(base,MIX,domain='scene_linear_rec2020_d65');text=json.dumps(edited)
                upstream=values(session.render_manifest(json.dumps(base),req));output=values(session.render_manifest(text,req))
                for offset in range(0,len(output),3):
                    for row in range(3):
                        expected=sum(Fraction(MIX[row*3+c])*Fraction(float(upstream[offset+c])) for c in range(3))
                        self.assertAlmostEqual(output[offset+row],float(expected),delta=5e-7)
                self.assertEqual(session.render_manifest(text,{**req,'tile_size':1}),session.render_manifest(text,req))
                regions=session.required_source_regions(text,req)
                self.assertEqual(set(regions),{a,b});self.assertEqual(regions,session.required_source_regions(json.dumps(base),req))
        finally:session.close()

    def test_strict_matrix_versions_domains_disabled_and_overflow(self):
        session=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50')
        base=source_doc(session)
        try:
            invalid=[{},dict(matrix=[]),dict(matrix=IDENTITY[:8]),dict(matrix=IDENTITY+[0]),dict(matrix=[IDENTITY]*3),
                dict(matrix=IDENTITY,extra=0),dict(matrix='identity'),dict(matrix=[True]+IDENTITY[1:]),
                dict(matrix=[64.1]+IDENTITY[1:]),dict(matrix=[-64.1]+IDENTITY[1:])]
            for parameters in invalid:
                for enabled in (True,False):
                    doc=add(base,IDENTITY);doc['operations'][-1].update(parameters=parameters,enabled=enabled)
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    doc=add(base,IDENTITY);doc['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for coefficient in (float('nan'),float('inf'),-float('inf')):
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(base,[coefficient]+IDENTITY[1:])))
            disabled=add(base,MIX);disabled['operations'][-1]['enabled']=False
            self.assertEqual(session.render_manifest(json.dumps(disabled)),session.render_manifest(json.dumps(base)))
            encoded=json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(encoded,IDENTITY,domain='encoded_srgb')))
        finally:session.close()
        for bad in (float('nan'),float('inf'),-float('inf')):
            with self.assertRaises(ValueError):raw.RasterSession(array.array('f',[1,bad,1]),1,1,'prophoto-d50')
        session=raw.RasterSession(array.array('f',[3.4028234663852886e38,0,0]),1,1,'prophoto-d50')
        text=json.dumps(add(source_doc(session),[2]+IDENTITY[1:]))
        with self.assertRaises(ValueError):session.render_manifest(text)
        session.close()
        with self.assertRaises(RuntimeError):session.render_manifest(text)

if __name__=='__main__':unittest.main()
