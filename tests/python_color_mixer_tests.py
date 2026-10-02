"""Exact-rational color mixer truth and saved-graph integration."""
import array
import bisect
import copy
from fractions import Fraction as F
import json
import math
import sys
import unittest
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw
CENTERS=[0,30,60,120,180,240,270,300,360]
WEIGHTS={'prophoto-d50':(.28807112822929337,.00008565396060525903),
         'rec2020-d65':(.26270021201126703,.059301716469861945)}
ZERO={k:[0.]*8 for k in ('hue_shift','saturation_delta','luminance_delta')}
PALETTE=[(1,0,0),(1,1,0),(0,1,0),(0,1,1),(0,0,1),(1,0,1)]

def select(rgb):
    r,g,b=rgb;m=max(rgb);c=m-min(rgb)
    if not c:return None
    h=60*((g-b)/c) if m==r else 60*((b-r)/c+2) if m==g else 60*((r-g)/c+4)
    return h%360

def rational(rgb,p,space):
    rgb=list(map(F,rgb));c=max(rgb)-min(rgb)
    if not c or all(v==0 for a in p.values() for v in a):return rgb,dict(bypass=True)
    h=select(rgb);band=bisect.bisect_right(CENTERS,h)-1
    t=(h-CENTERS[band])/(CENTERS[band+1]-CENTERS[band])
    parameters=[(1-t)*F(p[k][band])+t*F(p[k][(band+1)%8]) for k in ZERO]
    H,D,L=parameters;wr,wb=map(F,WEIGHTS[space]);wg=1-wr-wb
    luminance=lambda v:wr*v[0]+wg*v[1]+wb*v[2]
    y=luminance(rgb);fade=c/(abs(y)+c);target=y*(1+L*fade);S=1+D
    if H==0 and S==1 and target==y:return rgb,dict(bypass=True)
    if H:
        u=((h+H)%360)/60;sector=int(u);f=u-sector
        # Convex combination of explicit primary/secondary vertices, independent
        # of the proposed six-sector component branch implementation.
        q=[c*((1-f)*a+f*b) for a,b in zip(PALETTE[sector],PALETTE[(sector+1)%6])]
        qy=luminance(q);chroma=[v-qy for v in q]
    else:chroma=[v-y for v in rgb]
    return [target+S*v for v in chroma],dict(bypass=False,hue=h,band=band,t=t,y=y,target=target,S=S,c=c,L=L)

def wheel(h):
    u=F(h)/60;sector=int(u)%6;f=u-int(u)
    return tuple(float((1-f)*a+f*b) for a,b in zip(PALETTE[sector],PALETTE[(sector+1)%6]))

COMBINED={'hue_shift':[30,-30,60,-60,15,-15,45,-45],
    'saturation_delta':[1,-1,.5,-.5,.25,-.25,.75,-.75],
    'luminance_delta':[.5,-.5,1,-1,.25,-.25,.75,-.75]}
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
    node=dict(id='93000000-0000-0000-0000-000000000090',type='rawengine.color_mixer',schema_version=1,
        processing_version=2,enabled=True,input_domain=domain,output_domain=domain,
        inputs=dict(image=doc['output']),parameters=copy.deepcopy(parameters),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc

class ColorMixerTests(unittest.TestCase):
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

    def test_rational_anchors_overlaps_wrap_ties_headroom_and_limits(self):
        points=[tuple(offset+gain*v for v in wheel(h)) for h in range(0,360,15) for offset in (-2,0,3) for gain in (.125,1,4)]
        points += [(1,1,0),(0,1,1),(1,0,1),(2**-149,0,2**-148)]
        # Non-dyadic near-seam samples on both sides of all anchors.
        for h in CENTERS[:-1]:
            for e in (-2**-17,2**-17):points.append(wheel((h+e)%360))
        points += [(1,1,1+2**-n) for n in (4,8,16,23)]
        settings=[ZERO,COMBINED]
        for key in ZERO:
            for value in ((-60,30,60) if key=='hue_shift' else (-1,.5,1)):
                p=copy.deepcopy(ZERO);p[key]=[value]*8;settings.append(p)
        for space in WEIGHTS:
            session=raw.RasterSession(array.array('f',[v for rgb in points for v in rgb]),len(points),1,space)
            try:
                for p in settings:self.assert_map(session,source_doc(session),p)
                for rgb in points:
                    expected,info=rational(rgb,COMBINED,space)
                    if not info['bypass']:
                        wr,wb=map(F,WEIGHTS[space]);wg=1-wr-wb
                        self.assertEqual(sum(w*v for w,v in zip((wr,wg,wb),expected)),info['target'])
                        self.assertEqual(max(expected)-min(expected),info['S']*info['c'])
                        self.assertLessEqual(abs(info['target']-info['y']),abs(info['L'])*info['c'])
            finally:session.close()

    def test_bits_neutrals_effective_identity_and_tiny_edit(self):
        points=[(-0.,0.,-0.),(3.4028234663852886e38,)*3,(-3.4028234663852886e38,)*3,(2**-149,)*3]
        for space in WEIGHTS:
            session=raw.RasterSession(array.array('f',[v for rgb in points for v in rgb]),4,1,space)
            try:
                base=source_doc(session);before=session.render_manifest(json.dumps(base))
                for p in (ZERO,COMBINED):self.assertEqual(session.render_manifest(json.dumps(add(base,p))),before)
            finally:session.close()
            for rgb,p in (((1,0,-0.),dict(ZERO,hue_shift=[0,0,0,60,0,0,0,0])),
                ((1,.25,-0.),dict(hue_shift=[30,-30]+[0]*6,saturation_delta=[.5,-.5]+[0]*6,luminance_delta=[1,-1]+[0]*6))):
                session=raw.RasterSession(array.array('f',rgb),1,1,space)
                try:
                    base=source_doc(session);self.assertEqual(session.render_manifest(json.dumps(add(base,p))),session.render_manifest(json.dumps(base)))
                finally:session.close()
            session=raw.RasterSession(array.array('f',[1,0,0]),1,1,space)
            try:
                base=source_doc(session);tiny=dict(ZERO,hue_shift=[2**-20]*8)
                self.assertNotEqual(session.render_manifest(json.dumps(add(base,tiny))),session.render_manifest(json.dumps(base)))
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
        session=raw.RasterSession(array.array('f',[1,0,0,0,1,0]),2,1,'prophoto-d50')
        try:
            p=dict(ZERO,hue_shift=[60,0,0,0,0,0,0,0]);base=source_doc(session)
            _,reduced=self.assert_map(session,base,p,dict(mip=1,quality='preview'))
            native=values(session.render_manifest(json.dumps(add(base,p))))
            averaged=array.array('f',[(native[c]+native[3+c])/2 for c in range(3)])
            self.assertNotEqual(reduced[2],averaged.tobytes())
        finally:session.close()

    def test_jobs_cache_history_analysis_and_ownership(self):
        data=array.array('f',[(i%29-10)/8 for i in range(9*7*3)])
        session=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(session);parameters=copy.deepcopy(COMBINED);text=json.dumps(add(base,parameters));req=dict(tile_size=3)
            original=session.render_manifest(text,req);parameters['hue_shift'][0]=999;before=session.cache_stats()
            self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original)
            self.assertEqual(session.cache_stats()['misses'],before['misses'])
            for key,change in (('hue_shift',15),('saturation_delta',-.5),('luminance_delta',-.5)):
                p=copy.deepcopy(COMBINED);p[key]=[change]*8;revised_text=json.dumps(add(base,p))
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

    def test_strict_parameters_versions_disabled_domains_and_overflow(self):
        session=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(session)
        try:
            invalid=[{},dict(ZERO,extra=0)]
            for key in ZERO:
                p=copy.deepcopy(ZERO);del p[key];invalid.append(p)
                for value in (None,0,[],[0]*7,[0]*9,[[0]]*8,[True]+[0]*7,['0']+[0]*7,
                    [61 if key=='hue_shift' else 1.01]+[0]*7,[-61 if key=='hue_shift' else -1.01]+[0]*7,
                    [float('nan')]+[0]*7,[float('inf')]+[0]*7,[-float('inf')]+[0]*7):
                    invalid.append(dict(ZERO,**{key:value}))
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
        text=json.dumps(add(source_doc(session),dict(ZERO,saturation_delta=[1]*8)))
        with self.assertRaises(ValueError):session.render_manifest(text)
        session.close()
        with self.assertRaises(RuntimeError):session.render_manifest(text)

if __name__=='__main__':unittest.main()
