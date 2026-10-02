"""Independent rational tonal color balance and saved-graph integration."""
import array
import copy
from fractions import Fraction as F
import itertools
import json
import sys
import unittest
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw
FIELDS=('shadows','midtones','highlights')
WEIGHTS={'prophoto-d50':(.28807112822929337,.00008565396060525903),
    'rec2020-d65':(.26270021201126703,.059301716469861945)}
ZERO={**{k:[0.]*3 for k in FIELDS},'preserve_luminance':True}
MAX=3.4028234663852886e38

def rational(rgb,p,space):
    rgb=list(map(F,rgb));wr,wb=map(F,WEIGHTS[space]);wg=1-wr-wb
    y=lambda v:sum(w*x for w,x in zip((wr,wg,wb),v))
    y0=y(rgb);t=max(F(0),min(F(1),y0))
    # Independent Bernstein basis and exact weighted sum, with no ordered shortcuts.
    weights=((1-t)**2,2*t*(1-t),t**2)
    d=[sum(w*F(p[name][c]) for w,name in zip(weights,FIELDS)) for c in range(3)]
    yd=y(d);offset=[v-yd if p['preserve_luminance'] else v for v in d]
    output=[v+o for v,o in zip(rgb,offset)]
    return output,dict(y=y0,t=t,weights=weights,d=d,yd=yd,offset=offset,target=y0 if p['preserve_luminance'] else y0+yd)

COMBINED=dict(shadows=[.5,-.5,.25],midtones=[-.25,.75,-.5],highlights=[1,-1,.5],preserve_luminance=True)
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

def add(doc, parameters=COMBINED, domain=None):
    doc=copy.deepcopy(doc)
    domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    node=dict(id='93000000-0000-0000-0000-000000000090',type='rawengine.color_balance',schema_version=1,
        processing_version=2,enabled=True,input_domain=domain,output_domain=domain,
        inputs=dict(image=doc['output']),parameters=copy.deepcopy(parameters),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc

class ColorBalanceTests(unittest.TestCase):
    def assert_map(self,session,base,parameters=COMBINED,request=None,domain=None):
        text=json.dumps(add(base,parameters,domain));original=session.render_manifest(json.dumps(base),request)
        actual=session.render_manifest(text,request);self.assertEqual(actual[:2],original[:2])
        before=values(original);after=values(actual)
        space='rec2020-d65' if domain=='scene_linear_rec2020_d65' or base['working_space']=='linear_rec2020_d65' else 'prophoto-d50'
        for i in range(0,len(before),3):
            expected,_=rational(before[i:i+3],parameters,space)
            for c in range(3):self.assertAlmostEqual(after[i+c],float(expected[c]),delta=4e-7*max(1,abs(float(expected[c]))))
        for tile in (1,3,64):self.assertEqual(session.render_manifest(text,{**(request or {}),'tile_size':tile}),actual)
        self.assertEqual(session.required_source_regions(text,request),session.required_source_regions(json.dumps(base),request))
        return text,actual

    def test_rational_tonal_weights_projection_signed_extremes_and_limits(self):
        points=list(itertools.product((-2,-.25,0,.25,.5,1,3),repeat=3))
        points += [(v,v,v) for v in (-2**-149,-0.,2**-149,.125,.5,1-2**-24,1,1+2**-23,MAX,-MAX)]
        points += list(itertools.product((-MAX,2**-149,MAX),repeat=3))
        points += [(1.578406810760498,2.1256375312805176,-22974),(2.3305158615112305,-.028333187103271484,-10)]
        settings=[ZERO]
        for preserve in (False,True):
            for name in FIELDS:
                p=copy.deepcopy(ZERO);p[name]=[1,-1,.5];p['preserve_luminance']=preserve;settings.append(p)
            settings.append(dict(COMBINED,preserve_luminance=preserve))
            for amount in (-1,.5,1):settings.append({**{k:[amount]*3 for k in FIELDS},'preserve_luminance':preserve})
        for space in WEIGHTS:
            session=raw.RasterSession(array.array('f',[v for rgb in points for v in rgb]),len(points),1,space)
            try:
                base=source_doc(session)
                for p in settings:self.assert_map(session,base,p)
                wr,wb=map(F,WEIGHTS[space]);wg=1-wr-wb
                for rgb in points:
                    expected,info=rational(rgb,COMBINED,space)
                    self.assertEqual(sum(info['weights']),1)
                    self.assertTrue(all(v>=0 for v in info['weights']))
                    self.assertEqual(sum(w*v for w,v in zip((wr,wg,wb),expected)),info['target'])
            finally:session.close()

    def test_full_component_identity_neutral_tint_black_lift_and_tiny_edit(self):
        pixels=array.array('f',[-0.,0.,-0.,MAX,-MAX,2**-149,-MAX,MAX,-2**-149])
        for space in WEIGHTS:
            session=raw.RasterSession(pixels,3,1,space)
            try:
                base=source_doc(session);original=session.render_manifest(json.dumps(base))
                parameters=[ZERO,dict(ZERO,preserve_luminance=False)]
                parameters += [{**{k:[v]*3 for k in FIELDS},'preserve_luminance':True} for v in (-1,.5,1)]
                for p in parameters:self.assertEqual(session.render_manifest(json.dumps(add(base,p))),original)
            finally:session.close()
            for rgb,p in (((-0.,0.,-0.),dict(ZERO,midtones=[1,-1,.5])),
                ((2,2,2),dict(ZERO,midtones=[1,-1,.5])),
                ((.5,.5,.5),dict(shadows=[1,-1,.5],midtones=[-1,1,-.5],highlights=[1,-1,.5],preserve_luminance=False))):
                session=raw.RasterSession(array.array('f',rgb),1,1,space)
                try:
                    base=source_doc(session);self.assertEqual(session.render_manifest(json.dumps(add(base,p))),session.render_manifest(json.dumps(base)))
                finally:session.close()
            session=raw.RasterSession(array.array('f',[-0.,-0.,-0.]),1,1,space)
            try:
                base=source_doc(session)
                _,out=self.assert_map(session,base,dict(ZERO,shadows=[.25,0,0],preserve_luminance=False))
                self.assertEqual(out[2][4:],array.array('f',[-0.,-0.]).tobytes())
                _,tint=self.assert_map(session,base,dict(ZERO,shadows=[1,0,0]))
                self.assertLess(values(tint)[1],0);self.assertGreater(values(tint)[0],0)
                _,tiny=self.assert_map(session,base,dict(ZERO,shadows=[2**-140,0,0],preserve_luminance=False))
                self.assertGreater(values(tiny)[0],0)
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
            p=dict(ZERO,midtones=[1,0,0],preserve_luminance=False);base=source_doc(session)
            _,reduced=self.assert_map(session,base,p,dict(mip=1,quality='preview'))
            self.assertEqual(list(values(reduced)),[1,.5,.5])
            native=values(session.render_manifest(json.dumps(add(base,p))))
            self.assertEqual([(native[c]+native[3+c])/2 for c in range(3)],[.5,.5,.5])
        finally:session.close()

    def test_jobs_cache_history_analysis_and_ownership(self):
        data=array.array('f',[(i%29-10)/8 for i in range(9*7*3)])
        session=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(session);parameters=copy.deepcopy(COMBINED);text=json.dumps(add(base,parameters));req=dict(tile_size=3)
            original=session.render_manifest(text,req);parameters['shadows'][0]=999;before=session.cache_stats()
            self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original)
            self.assertEqual(session.cache_stats()['misses'],before['misses'])
            for key,change in (('shadows',.25),('midtones',-.5),('highlights',-.5),('preserve_luminance',False)):
                p=copy.deepcopy(COMBINED);p[key]=change if key=='preserve_luminance' else [change]*3;revised_text=json.dumps(add(base,p))
                revised=session.render_manifest(revised_text,req);after=session.cache_stats()
                self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses'])
                self.assertNotEqual(original,revised);before=after
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
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(source_doc(session),ZERO,'camera_linear')))
            finally:session.close()

    def test_multisource_actual_working_conversion(self):
        a,b='93000000-0000-0000-0000-000000000001','93000000-0000-0000-0000-000000000002'
        session=raw.RasterGraphSession({a:dict(rgb=array.array('f',[-1,.5,2]*9),width=3,height=3,working_space='prophoto-d50'),
            b:dict(rgb=array.array('f',[.25,.75,1.5]*9),width=3,height=3,working_space='prophoto-d50')})
        try:
            base=json.loads(session.export_manifest(a))
            mix=dict(id='93000000-0000-0000-0000-000000000080',type='rawengine.linear_mix',schema_version=1,
                processing_version=2,enabled=True,input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_prophoto_d50',
                inputs=dict(base=a,layer=b),parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1)
            base['operations']=[mix];base['output']=mix['id']
            converted=dict(id='93000000-0000-0000-0000-000000000081',type='rawengine.working_space_convert',schema_version=1,
                processing_version=2,enabled=True,input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_rec2020_d65',
                inputs=dict(image=base['output']),parameters={},masks={},blend_mode='normal',opacity=1)
            base['operations'].append(converted);base['output']=converted['id']
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final')
                text,_=self.assert_map(session,base,request=req,domain='scene_linear_rec2020_d65')
                self.assertEqual(set(session.required_source_regions(text,req)),{a,b})
        finally:session.close()

    def test_strict_parameters_versions_disabled_domains_and_close(self):
        session=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(session)
        try:
            invalid=[{},dict(ZERO,extra=0)]
            for key in ZERO:
                p=copy.deepcopy(ZERO);del p[key];invalid.append(p)
            for key in FIELDS:
                for value in (None,0,[],[0]*2,[0]*4,[[0]]*3,[True,0,0],['0',0,0],[1.01,0,0],[-1.01,0,0],
                    [float('nan'),0,0],[float('inf'),0,0],[-float('inf'),0,0]):invalid.append(dict(ZERO,**{key:value}))
            for value in (0,1,'true',None,[]):invalid.append(dict(ZERO,preserve_luminance=value))
            for p in invalid:
                for enabled in (True,False):
                    doc=add(base,p);doc['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    doc=add(base);doc['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for change in ({'input_domain':'scene_linear_rec2020_d65'},{'output_domain':'scene_linear_rec2020_d65'},
                {'opacity':.5},{'blend_mode':'multiply'},{'inputs':{}},{'extensions':{'test':1}}):
                doc=add(base);doc['operations'][-1].update(change)
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            disabled=add(base);disabled['operations'][-1]['enabled']=False
            self.assertEqual(session.render_manifest(json.dumps(disabled)),session.render_manifest(json.dumps(base)))
            encoded=json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(encoded,ZERO,'encoded_srgb')))
        finally:session.close()
        session=raw.RasterSession(array.array('f',[3.4028234663852886e38,0,0]),1,1,'prophoto-d50')
        text=json.dumps(add(source_doc(session),dict(ZERO,highlights=[1,-1,1],preserve_luminance=False)))
        self.assertEqual(values(session.render_manifest(text))[0],MAX)
        session.close()
        with self.assertRaises(RuntimeError):session.render_manifest(text)

if __name__=='__main__':unittest.main()
