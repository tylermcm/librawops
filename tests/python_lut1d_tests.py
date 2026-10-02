"""Independent exact-rational LUT truth and native saved-graph integration."""
import array
import bisect
import copy
from fractions import Fraction
import json
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw

IDENTITY = dict(input_min=0,input_max=1,channels=[[0,1]]*3)
TABLE = dict(input_min=-1,input_max=1,channels=[[-2,0,1],[2,1,-1],[.25,.25,.25]])

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

def add(doc, parameters=TABLE, domain=None):
    doc=copy.deepcopy(doc)
    domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    node=dict(id='91000000-0000-0000-0000-000000000090',type='rawengine.lut1d',schema_version=1,
        processing_version=2,enabled=True,input_domain=domain,output_domain=domain,
        inputs=dict(image=doc['output']),parameters=copy.deepcopy(parameters),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc

def grid(parameters):
    a,b=parameters['input_min'],parameters['input_max'];n=len(parameters['channels'][0])
    return [a if i==0 else b if i==n-1 else a+(b-a)*(i/(n-1)) for i in range(n)]

def truth(sample, channel, coordinates):
    # Barycentric exact-rational interpolation independent of native slopes and
    # anchor arithmetic. Coordinates use the frozen binary64 grid contract.
    v=Fraction(float(sample));xs=list(map(Fraction,coordinates));ys=list(map(Fraction,channel))
    if xs==ys:return float(sample)
    left=max(0,min(len(xs)-2,bisect.bisect_right(xs,v)-1))
    a,b=xs[left:left+2];y,z=ys[left:left+2]
    return float((y*(b-v)+z*(v-a))/(b-a))

class Lut1DTests(unittest.TestCase):
    def assert_map(self,session,base,parameters=TABLE,request=None,domain=None):
        text=json.dumps(add(base,parameters,domain));original=session.render_manifest(json.dumps(base),request)
        actual=session.render_manifest(text,request);self.assertEqual(actual[:2],original[:2])
        coordinates=grid(parameters)
        for i,(before,after) in enumerate(zip(values(original),values(actual))):
            expected=truth(before,parameters['channels'][i%3],coordinates)
            self.assertAlmostEqual(after,expected,delta=3e-7*max(1,abs(expected)))
        for tile in (1,3,64):
            self.assertEqual(session.render_manifest(text,{**(request or {}),'tile_size':tile}),actual)
        self.assertEqual(session.required_source_regions(text,request),session.required_source_regions(json.dumps(base),request))
        return text,actual

    def test_rational_truth_knots_signed_headroom_and_bounds(self):
        data=array.array('f',[v for x in (-3,-1,-.5,0,.125,.5,1,2,3) for v in (x,x,x)])
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(data,9,1,space)
            try:
                for table in (TABLE,IDENTITY,dict(input_min=0,input_max=1,channels=[[0,65536]]*3),
                    dict(input_min=-2,input_max=3,channels=[[.25,1,-.5,2,1,.25,.5]]*3)):
                    self.assert_map(session,source_doc(session),table)
                output=values(self.assert_map(session,source_doc(session))[1])
                self.assertTrue(any(v<0 for v in output));self.assertTrue(any(v>1 for v in output))
            finally:session.close()

    def test_identity_partial_identity_extreme_bits_and_256_samples(self):
        data=array.array('f',[-0.,0.,-0.,3.4028234663852886e38,-3.4028234663852886e38,2**-149])
        session=raw.RasterSession(data,2,1,'prophoto-d50')
        try:
            base=source_doc(session);original=session.render_manifest(json.dumps(base))
            for n in (2,3,7,256):
                table=dict(input_min=-2,input_max=3,channels=[[0]*n]*3)
                table['channels']=[grid(table)]*3
                self.assertEqual(session.render_manifest(json.dumps(add(base,table))),original)
            partial=copy.deepcopy(IDENTITY);partial['channels'][1]=[1,0]
            result=session.render_manifest(json.dumps(add(base,partial)))[2]
            for p in (0,1):
                self.assertEqual(result[p*12:p*12+4],data.tobytes()[p*12:p*12+4])
                self.assertEqual(result[p*12+8:p*12+12],data.tobytes()[p*12+8:p*12+12])
        finally:session.close()

    def test_geometry_reduced_roi_and_mapping_order(self):
        session=raw.RasterSession(array.array('f',((i*37%257-60)/128 for i in range(11*9*3))),11,9,'prophoto-d50')
        try:
            doc=json.loads(session.export_manifest(dict(crop=(1,1,9,7),rotate=90,resize=(9,5),resize_filter='area',output_mode='srgb-preview')))
            base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.resize'))
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final');text,result=self.assert_map(session,base,request=req)
                roi={**req,'x':1,'y':0,'roi_width':1,'roi_height':1}
                self.assertEqual(session.render_manifest(text,roi)[2],result[2][12:24])
        finally:session.close()
        session=raw.RasterSession(array.array('f',[0,0,0,1,1,1]),2,1,'prophoto-d50')
        try:
            table=dict(input_min=0,input_max=1,channels=[[0,0,1]]*3)
            text,result=self.assert_map(session,source_doc(session),table,dict(mip=1,quality='preview'))
            self.assertEqual(list(values(result)),[0]*3) # Native-before-average would be .5.
        finally:session.close()

    def test_jobs_cache_history_analysis_and_ownership(self):
        data=array.array('f',[(i%29-10)/8 for i in range(9*7*3)])
        session=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(session);table=copy.deepcopy(TABLE);doc=add(base,table);text=json.dumps(doc);req=dict(tile_size=3)
            original=session.render_manifest(text,req);table['channels'][0][0]=999;before=session.cache_stats()
            self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original)
            self.assertEqual(session.cache_stats()['misses'],before['misses'])
            revised_table=copy.deepcopy(TABLE);revised_table['input_min']=-2
            revised_text=json.dumps(add(base,revised_table));revised=session.render_manifest(revised_text,req);after=session.cache_stats()
            self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses'])
            self.assertNotEqual(original,revised)
            sample_edit=copy.deepcopy(TABLE);sample_edit['channels'][0][0]=-1
            self.assertNotEqual(session.render_manifest(json.dumps(add(base,sample_edit)),req),original)
            self.assertEqual(session.submit_manifest_latest('view',revised_text,req).result(timeout=5),revised)
            history=session.history(text);history.commit(revised_text)
            self.assertEqual(history.render(req),revised);self.assertTrue(history.undo());self.assertEqual(history.render(req),original)
            restored=session.restore_history(history.save());self.assertEqual(restored.render(req),original)
            self.assertEqual(session.histogram_manifest(text)['descriptor']['primaries'],'rec2020')
            tiles=[];session.analyze_local_manifest(text,tiles.append,req,radius=1);self.assertTrue(tiles)
            data[0]=999;self.assertEqual(session.render_manifest(text,req),original)
            restored.close();history.close()
        finally:session.close()

    def test_calibrated_raw_both_demosaicers_spaces_and_levels(self):
        data=array.array('H',((i*719)%50000 for i in range(99)))
        meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            session=raw.RawSession(data,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                for space in ('prophoto-d50','rec2020-d65'):
                    doc=json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),working_space=space,output_mode='srgb-preview')))
                    base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
                    for mip in (0,1,2):
                        self.assert_map(session,base,request=dict(mip=mip,quality='preview' if mip else 'final'))
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(source_doc(session),IDENTITY,'camera_linear')))
            finally:session.close()

    def test_multisource_actual_working_conversion(self):
        a,b='91000000-0000-0000-0000-000000000001','91000000-0000-0000-0000-000000000002'
        session=raw.RasterGraphSession({a:dict(rgb=array.array('f',[-1,.5,2]*9),width=3,height=3,working_space='prophoto-d50'),
            b:dict(rgb=array.array('f',[.25,.75,1.5]*9),width=3,height=3,working_space='prophoto-d50')})
        try:
            base=json.loads(session.export_manifest(a))
            mix=dict(id='91000000-0000-0000-0000-000000000080',type='rawengine.linear_mix',schema_version=1,
                processing_version=2,enabled=True,input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_prophoto_d50',
                inputs=dict(base=a,layer=b),parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1)
            base['operations']=[mix];base['output']=mix['id']
            converted=dict(id='91000000-0000-0000-0000-000000000081',type='rawengine.working_space_convert',schema_version=1,
                processing_version=2,enabled=True,input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_rec2020_d65',
                inputs=dict(image=base['output']),parameters={},masks={},blend_mode='normal',opacity=1)
            base['operations'].append(converted);base['output']=converted['id']
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final')
                text,_=self.assert_map(session,base,request=req,domain='scene_linear_rec2020_d65')
                self.assertEqual(set(session.required_source_regions(text,req)),{a,b})
        finally:session.close()

    def test_strict_parameters_versions_disabled_domains_and_overflow(self):
        session=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(session)
        try:
            invalid=[{},dict(IDENTITY,extra=0),dict(IDENTITY,input_min=True),dict(IDENTITY,input_min=1),
                dict(IDENTITY,input_min=2),dict(IDENTITY,input_max=65537),dict(IDENTITY,input_max=1e-300),
                dict(IDENTITY,channels=[]),dict(IDENTITY,channels=[[0,1]]*2),dict(IDENTITY,channels=[0,1,2]),
                dict(IDENTITY,channels=[[0],[0],[0]]),dict(IDENTITY,channels=[[0]*257]*3),
                dict(IDENTITY,channels=[[0,1],[0,.5,1],[0,1]]),dict(IDENTITY,channels=[[True,1]]*3),
                dict(IDENTITY,channels=[[0,65537]]*3),dict(IDENTITY,channels='identity'),
                dict(input_min=1,input_max=1.0000000000000002,channels=[[1,1,1]]*3)]
            for parameters in invalid:
                for enabled in (True,False):
                    doc=add(base,parameters);doc['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    doc=add(base);doc['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for v in (float('nan'),float('inf'),-float('inf')):
                for parameters in (dict(IDENTITY,input_min=v),dict(IDENTITY,channels=[[0,v]]*3)):
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(base,parameters)))
            disabled=add(base);disabled['operations'][-1]['enabled']=False
            self.assertEqual(session.render_manifest(json.dumps(disabled)),session.render_manifest(json.dumps(base)))
            encoded=json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(encoded,IDENTITY,'encoded_srgb')))
        finally:session.close()
        session=raw.RasterSession(array.array('f',[3.4028234663852886e38,0,0]),1,1,'prophoto-d50')
        text=json.dumps(add(source_doc(session),dict(IDENTITY,channels=[[0,2]]*3)))
        with self.assertRaises(ValueError):session.render_manifest(text)
        session.close()
        with self.assertRaises(RuntimeError):session.render_manifest(text)

if __name__=='__main__':unittest.main()
