"""Independent atmospheric formation truth and saved dehaze integration."""
import array
import copy
from fractions import Fraction as F
import itertools
import json
import sys
import unittest
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw
ZERO=dict(amount=0,atmospheric_light=[1,1,1])
COMBINED=dict(amount=.5,atmospheric_light=[.8,1,1.2])
SPACES=('prophoto-d50','rec2020-d65')
MAX=3.4028234663852886e38
def values(result):
    data=array.array('f');data.frombytes(result[2]);return data
def tap(doc,output):
    doc=copy.deepcopy(doc);ops={o['id']:o for o in doc['operations']};needed=set()
    def visit(id):
        if id not in ops or id in needed:return
        needed.add(id)
        for upstream in ops[id]['inputs'].values():visit(upstream)
    visit(output);doc.update(output=output,operations=[o for o in doc['operations'] if o['id'] in needed]);return doc
def source_doc(session):
    doc=json.loads(session.export_manifest());return tap(doc,doc['sources'][0]['id'])
def add(doc,p=COMBINED,domain=None):
    doc=copy.deepcopy(doc)
    domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    id='95000000-0000-0000-0000-'+str(90+sum(o['type']=='rawengine.dehaze' for o in doc['operations'])).zfill(12)
    node=dict(id=id,type='rawengine.dehaze',schema_version=1,processing_version=2,
        enabled=True,input_domain=domain,output_domain=domain,inputs=dict(image=doc['output']),parameters=copy.deepcopy(p),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc
def rational(rgb,p):
    h=F(7,8)*abs(F(p['amount']));t=1-h
    return [float((F(float(x))-h*F(p['atmospheric_light'][c]))/t if p['amount']>0 else t*F(float(x))+h*F(p['atmospheric_light'][c])) for c,x in enumerate(rgb)]
class DehazeTests(unittest.TestCase):
    def assert_map(self,session,base,p=COMBINED,req=None,domain=None):
        text=json.dumps(add(base,p,domain));before=session.render_manifest(json.dumps(base),req);out=session.render_manifest(text,req)
        self.assertEqual(out[:2],before[:2]);original=values(before);actual=values(out)
        for i in range(0,len(actual),3):
            expected=rational(original[i:i+3],p)
            for c in range(3):self.assertAlmostEqual(actual[i+c],expected[c],delta=4e-7*max(1,abs(expected[c])))
        for tile in (1,3,64):self.assertEqual(session.render_manifest(text,{**(req or {}),'tile_size':tile}),out)
        self.assertEqual(session.required_source_regions(text,req),session.required_source_regions(json.dumps(base),req))
        return text,out
    def test_rational_controls_signed_headroom_uniform_haze_truth(self):
        points=list(itertools.product((-2,-.25,0,2**-149,.25,1,4),repeat=3));data=array.array('f',[v for rgb in points for v in rgb])
        for space in SPACES:
            session=raw.RasterSession(data,len(points),1,space)
            try:
                base=source_doc(session)
                for a in (-1,-.5,-2**-40,0,2**-40,.5,1):
                    for A in ([0]*3,[1]*3,[.8,1,1.2],[4]*3):self.assert_map(session,base,dict(amount=a,atmospheric_light=A))
                original=session.render_manifest(json.dumps(base))
                for a in (.125,.5,1):
                    p=dict(amount=-a,atmospheric_light=[.8,1,1.2]);hazed=add(base,p)
                    _,recovered=self.assert_map(session,hazed,dict(p,amount=a))
                    self.assertLess(max(abs(x-y) for x,y in zip(values(original),values(recovered))),4e-6)
            finally:session.close()
    def test_identity_fixed_components_tiny_underflow_overflow(self):
        for space in SPACES:
            session=raw.RasterSession(array.array('f',[-0.,0.,-0.,MAX,-MAX,2**-149]),2,1,space)
            try:
                base=source_doc(session);original=session.render_manifest(json.dumps(base))
                self.assertEqual(session.render_manifest(json.dumps(add(base,ZERO))),original)
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(base,dict(amount=1,atmospheric_light=[1]*3))))
            finally:session.close()
            session=raw.RasterSession(array.array('f',[-0.,0.,-0.,2**-149,2**-149,2**-149,1,1,1]),3,1,space)
            try:
                base=source_doc(session);original=session.render_manifest(json.dumps(base))
                for a in (-1,-.5,.5,1):
                    _,out=self.assert_map(session,base,dict(amount=a,atmospheric_light=[0]*3));self.assertEqual(out[2][:12],original[2][:12])
                _,out=self.assert_map(session,base,dict(amount=2**-40,atmospheric_light=[1]*3));self.assertLess(values(out)[0],0)
                for a in (-1,1):
                    _,out=self.assert_map(session,base,dict(amount=a,atmospheric_light=[1]*3));self.assertEqual(out[2][-12:],original[2][-12:])
            finally:session.close()
    def test_geometry_reduced_roi_convolution_analysis(self):
        session=raw.RasterSession(array.array('f',((i*37%257-60)/128 for i in range(11*9*3))),11,9,'prophoto-d50')
        try:
            doc=json.loads(session.export_manifest(dict(crop=(1,1,9,7),rotate=90,resize=(9,5),resize_filter='area',output_mode='srgb-preview')))
            base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.resize'))
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final');text,out=self.assert_map(session,base,req=req)
                self.assertEqual(session.render_manifest(text,{**req,'x':1,'y':0,'roi_width':1,'roi_height':1})[2],out[2][12:24])
                tiles=[];session.analyze_local_manifest(text,tiles.append,req,radius=1);self.assertTrue(tiles)
            base=source_doc(session);domain='scene_linear_prophoto_d50'
            conv=dict(id='95000000-0000-0000-0000-000000000080',type='rawengine.convolution',schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,inputs=dict(image=base['output']),parameters=dict(width=3,height=1,coefficients=[.25,.5,.25],border='replicate'),masks={},blend_mode='normal',opacity=1)
            base['operations'].append(conv);base['output']=conv['id']
            self.assert_map(session,base)
        finally:session.close()
    def test_jobs_cache_history_owned_controls_input_and_replacement(self):
        data=array.array('f',[(i%29-10)/8 for i in range(9*7*3)]);session=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(session);p=copy.deepcopy(COMBINED);text=json.dumps(add(base,p));req=dict(tile_size=3)
            original=session.render_manifest(text,req);p['atmospheric_light'][0]=999;before=session.cache_stats()
            self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original);self.assertEqual(session.cache_stats()['misses'],before['misses'])
            for revised_p in (dict(COMBINED,amount=-.5),dict(COMBINED,atmospheric_light=[1,1,1])):
                revised_text=json.dumps(add(base,revised_p));revised=session.render_manifest(revised_text,req);after=session.cache_stats()
                self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses']);self.assertNotEqual(original,revised);before=after
            # Airlight still contributes to identity-operation cache keys.
            for A in ([1,1,1],[2,1,1]):session.render_manifest(json.dumps(add(base,dict(ZERO,atmospheric_light=A))),req)
            self.assertGreater(session.cache_stats()['misses'],before['misses'])
            self.assertEqual(session.submit_manifest_latest('view',revised_text,req).result(timeout=5),revised)
            history=session.history(text);history.commit(revised_text);self.assertEqual(history.render(req),revised)
            self.assertTrue(history.undo());self.assertEqual(history.render(req),original)
            restored=session.restore_history(history.save());self.assertEqual(restored.render(req),original)
            self.assertEqual(session.histogram_manifest(text)['descriptor']['primaries'],'rec2020')
            data[0]=999;self.assertEqual(session.render_manifest(text,req),original)
            restored.close();history.close()
        finally:session.close()
    def test_calibrated_raw_demosaicers_spaces_levels(self):
        data=array.array('H',((i*719)%50000 for i in range(99)));meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            session=raw.RawSession(data,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                for space in SPACES:
                    doc=json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),working_space=space,output_mode='srgb-preview')))
                    base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
                    for mip in (0,1,2):self.assert_map(session,base,req=dict(mip=mip,quality='preview' if mip else 'final'))
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(source_doc(session),ZERO,'camera_linear')))
            finally:session.close()
        data=array.array('H',((i*719)%50000 for i in range(29*25)))
        meta=dict(active_x=1,active_y=1,active_width=27,active_height=23,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            session=raw.RawSession(data,29,25,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                doc=json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6))))
                base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
                req=dict(x=10,y=10,roi_width=1,roi_height=1)
                self.assertEqual(session.required_source_regions(json.dumps(add(base)),req),session.required_source_regions(json.dumps(base),req))
            finally:session.close()
    def test_multisource_working_conversion(self):
        a,b='95000000-0000-0000-0000-000000000001','95000000-0000-0000-0000-000000000002'
        session=raw.RasterGraphSession({a:dict(rgb=array.array('f',[-1,.5,2]*9),width=3,height=3,working_space='prophoto-d50'),b:dict(rgb=array.array('f',[.25,.75,1.5]*9),width=3,height=3,working_space='prophoto-d50')})
        try:
            base=json.loads(session.export_manifest(a));domain='scene_linear_prophoto_d50'
            mix=dict(id='95000000-0000-0000-0000-000000000080',type='rawengine.linear_mix',schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,inputs=dict(base=a,layer=b),parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1)
            converted=dict(id='95000000-0000-0000-0000-000000000081',type='rawengine.working_space_convert',schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain='scene_linear_rec2020_d65',inputs=dict(image=mix['id']),parameters={},masks={},blend_mode='normal',opacity=1)
            base.update(operations=[mix,converted],output=converted['id'])
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final');text,_=self.assert_map(session,base,req=req,domain='scene_linear_rec2020_d65')
                self.assertEqual(set(session.required_source_regions(text,req)),{a,b})
            history=session.history(text);pinned=history.render(req)
            session.replace_source(a,dict(rgb=array.array('f',[.125]*27),width=3,height=3,working_space='prophoto-d50'))
            self.assertEqual(history.render(req),pinned)
            with self.assertRaises(ValueError):session.render_manifest(text,req)
            revised=json.loads(text);revised['sources']=json.loads(session.export_manifest(a))['sources']
            self.assertNotEqual(session.render_manifest(json.dumps(revised),req),pinned);history.close()
        finally:session.close()
    def test_strict_validation_even_disabled_and_encoded_domain(self):
        session=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(session)
        try:
            invalid=[{},dict(ZERO,extra=0),dict(amount=0),dict(atmospheric_light=[1]*3)]
            for value in (None,True,'0',[],1.01,-1.01,float('nan'),float('inf')):invalid.append(dict(ZERO,amount=value))
            for value in (None,0,[],[0]*2,[0]*4,[[0]]*3,[True,1,1],['0',1,1],[-.01,1,1],[4.01,1,1],[float('nan'),1,1],[float('inf'),1,1]):invalid.append(dict(ZERO,atmospheric_light=value))
            for p in invalid:
                for enabled in (True,False):
                    doc=add(base,p);doc['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    doc=add(base);doc['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for change in ({'input_domain':'scene_linear_rec2020_d65'},{'output_domain':'scene_linear_rec2020_d65'},{'opacity':.5},{'blend_mode':'multiply'},{'inputs':{}},{'extensions':{'test':1}}):
                doc=add(base);doc['operations'][-1].update(change)
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            disabled=add(base);disabled['operations'][-1]['enabled']=False
            self.assertEqual(session.render_manifest(json.dumps(disabled)),session.render_manifest(json.dumps(base)))
            encoded=json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(encoded,ZERO,'encoded_srgb')))
        finally:session.close()
if __name__=='__main__':unittest.main()
