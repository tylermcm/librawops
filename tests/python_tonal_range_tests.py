"""Independent rational monotone tonal curve and saved integration/lifetime gates."""
import array,copy,gc,itertools,json,math,struct,sys,unittest
from fractions import Fraction as F
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw
FIELDS=('blacks','shadows','highlights','whites')
ZERO=dict.fromkeys(FIELDS,0.)
COMBINED=dict(zip(FIELDS,(.123456789,-.345678901,.567890123,-.789012345)))
SPACES=('prophoto-d50','rec2020-d65')
WEIGHTS={'scene_linear_prophoto_d50':(.28807112822929337,.00008565396060525903),'scene_linear_rec2020_d65':(.26270021201126703,.059301716469861945)}
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
    doc=copy.deepcopy(doc);domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    identity='97000000-0000-0000-0000-'+str(90+sum(o['type']=='rawengine.tonal_range' for o in doc['operations'])).zfill(12)
    node=dict(id=identity,type='rawengine.tonal_range',schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,inputs=dict(image=doc['output']),parameters=copy.deepcopy(p),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=identity;return doc
def rational(rgb,p,domain):
    r,g,b=map(lambda x:F(float(x)),rgb);wr,wb=map(F,WEIGHTS[domain]);y=g+wr*(r-g)+wb*(b-g);v=abs(y)
    for name,(a,b) in zip(FIELDS,((0,F(1,8)),(0,F(1,2)),(F(1,8),2),(F(1,2),8))):
        c=F(p[name])
        if c and a<v<b:
            t=(v-a)/(b-a);v+=4*c*(b-a)*t*t*(1-t)*(1-t)
    gain=F(1) if not y else v/abs(y)
    return [float(x*gain) for x in map(lambda x:F(float(x)),rgb)]
class TonalRangeTests(unittest.TestCase):
    def assert_map(self,session,base,p=COMBINED,req=None,domain=None,tiles=True):
        doc=add(base,p,domain);text=json.dumps(doc);domain=doc['operations'][-1]['input_domain']
        before=session.render_manifest(json.dumps(base),req);out=session.render_manifest(text,req)
        self.assertEqual(out[:2],before[:2]);original=values(before);actual=values(out)
        for i in range(0,len(actual),3):
            expected=rational(original[i:i+3],p,domain)
            for c in range(3):self.assertAlmostEqual(actual[i+c],expected[c],delta=1.5e-7*abs(expected[c])+2**-149)
        if tiles:
            for size in (1,3,64):self.assertEqual(session.render_manifest(text,{**(req or {}),'tile_size':size}),out)
        self.assertEqual(session.required_source_regions(text,req),session.required_source_regions(json.dumps(base),req))
        return text,out
    def test_rational_full_corners_fractional_signed_extreme_and_identity(self):
        points=[(x,x,x) for x in (-8.,-1.,-.25,-.125,-2**-149,-0.,0.,2**-149,.0625,.125,.25,.5,1.,2.,4.,8.,16.)]+[(.9,.1,.025),(-.1,.2,1.4),(0,.5,1),(-1.,1.,-2.),(3.4028234663852886e38,-3.4028234663852886e38,0)]
        data=array.array('f',[x for p in points for x in p]);settings=[dict(zip(FIELDS,c)) for c in itertools.product((-1.,0.,1.),repeat=4)]+[COMBINED]
        for space in SPACES:
            session=raw.RasterSession(data,len(points),1,space)
            try:
                base=source_doc(session)
                for p in settings:self.assert_map(session,base,p,tiles=False)
                original=session.render_manifest(json.dumps(base));self.assertEqual(session.render_manifest(json.dumps(add(base,ZERO))),original)
            finally:session.close()
    def test_geometry_reduced_roi_analysis(self):
        session=raw.RasterSession(array.array('f',((i*37%257-60)/128 for i in range(11*9*3))),11,9,'prophoto-d50')
        try:
            doc=json.loads(session.export_manifest(dict(crop=(1,1,9,7),rotate=90,resize=(9,5),resize_filter='area',output_mode='srgb-preview')))
            base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.resize'))
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final');text,out=self.assert_map(session,base,req=req)
                self.assertEqual(session.render_manifest(text,{**req,'x':1,'y':0,'roi_width':1,'roi_height':1})[2],out[2][12:24])
                tiles=[];session.analyze_local_manifest(text,tiles.append,req,radius=1);self.assertTrue(tiles)
                self.assertEqual(session.histogram_manifest(text,req)['descriptor']['primaries'],'prophoto')
        finally:session.close()
    def test_cache_jobs_history_and_native_lifetimes(self):
        data=array.array('f',[(i%29-10)/8 for i in range(9*7*3)]);session=raw.RasterSession(data,9,7,'rec2020-d65')
        base=source_doc(session);p=copy.deepcopy(COMBINED);text=json.dumps(add(base,p));req=dict(tile_size=3)
        original=session.render_manifest(text,req);p['blacks']=999;data[0]=999;before=session.cache_stats()
        self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original);self.assertEqual(session.cache_stats()['misses'],before['misses'])
        history=session.history(text)
        for key in FIELDS:
            revised_text=json.dumps(add(base,{**COMBINED,key:-COMBINED[key]}));revised=session.render_manifest(revised_text,req)
            self.assertGreater(session.cache_stats()['misses'],before['misses']);self.assertNotEqual(revised,original);before=session.cache_stats()
            revision=history.commit(revised_text);self.assertEqual(history.render(req,revision=revision),revised)
        self.assertEqual(session.submit_manifest_latest('view',revised_text,req).result(timeout=5),revised)
        history.undo();saved=history.save();restored=session.restore_history(saved);expected=restored.render(req)
        job=restored.submit(req);del session,history;gc.collect();self.assertEqual(job.result(timeout=5),expected)
        restored.close()
    def test_calibrated_raw_patterns_spaces_levels(self):
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
    def test_multisource_replacement_pins_history(self):
        a,b='97000000-0000-0000-0000-000000000001','97000000-0000-0000-0000-000000000002'
        session=raw.RasterGraphSession({a:dict(rgb=array.array('f',[-1,.5,2]*9),width=3,height=3,working_space='prophoto-d50'),b:dict(rgb=array.array('f',[.25,.75,1.5]*9),width=3,height=3,working_space='prophoto-d50')})
        try:
            base=json.loads(session.export_manifest(a));domain='scene_linear_prophoto_d50'
            mix=dict(id='97000000-0000-0000-0000-000000000080',type='rawengine.linear_mix',schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,inputs=dict(base=a,layer=b),parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1)
            base.update(operations=[mix],output=mix['id'])
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final');text,out=self.assert_map(session,base,req=req)
                self.assertEqual(set(session.required_source_regions(text,req)),{a,b})
            history=session.history(text)
            session.replace_source(a,dict(rgb=array.array('f',[.125]*27),width=3,height=3,working_space='prophoto-d50'))
            self.assertEqual(history.render(req),out)
            with self.assertRaises(ValueError):session.render_manifest(text,req)
            history.close()
        finally:session.close()
    def test_strict_validation_even_disabled(self):
        session=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(session)
        try:
            invalid=[{},dict(ZERO,extra=0)]+[{k:v for k,v in ZERO.items() if k!=key} for key in FIELDS]
            for key in FIELDS:
                for value in (None,True,'0',[],1.01,-1.01,float('nan'),float('inf')):invalid.append({**ZERO,key:value})
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
