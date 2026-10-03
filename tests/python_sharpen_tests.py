"""Independent rational clipped-window sharpen truth and graph integration."""
import array
import copy
from fractions import Fraction as F
import json
import math
import sys
import unittest
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw
WEIGHTS={'prophoto-d50':(.28807112822929337,.00008565396060525903),
         'rec2020-d65':(.26270021201126703,.059301716469861945)}
DEFAULT=dict(amount=0,radius=1)
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
def operation(doc,kind,parameters,domain=None,id=90):
    domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    doc=copy.deepcopy(doc);node=dict(id=f'96000000-0000-0000-0000-{id:012d}',type='rawengine.'+kind,
        schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,
        inputs=dict(image=doc['output']),parameters=copy.deepcopy(parameters),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc
def add(doc,p=dict(amount=.5,radius=1),domain=None):return operation(doc,'sharpen',p,domain)
def oracle(result,p,space):
    # Independent direct exact-rational channel means, not centered differences.
    w,h,data=result[0],result[1],values(result);expected=[]
    for y in range(h):
        for x in range(w):
            window=[(yy*w+xx)*3 for yy in range(max(0,y-p['radius']),min(h,y+p['radius']+1))
                    for xx in range(max(0,x-p['radius']),min(w,x+p['radius']+1))]
            for c in range(3):
                center=F(data[(y*w+x)*3+c]);mean=sum((F(data[i+c]) for i in reversed(window)),F(0))/len(window)
                expected.append(float(center+F(p['amount'])*(center-mean)))
    return expected
class SharpenTests(unittest.TestCase):
    def assert_map(self,session,base,p=dict(amount=.5,radius=1),req=None,space=None):
        space=space or ('prophoto-d50' if base['working_space']=='linear_prophoto_d50' else 'rec2020-d65')
        domain='scene_linear_prophoto_d50' if space=='prophoto-d50' else 'scene_linear_rec2020_d65'
        text=json.dumps(add(base,p,domain));original=session.render_manifest(json.dumps(base),req)
        actual=session.render_manifest(text,req);expected=oracle(original,p,space)
        self.assertEqual(actual[:2],original[:2])
        for v,t in zip(values(actual),expected):self.assertAlmostEqual(v,t,delta=4e-7*max(1,abs(t)))
        for size in (1,3,64):self.assertEqual(session.render_manifest(text,{**(req or {}),'tile_size':size}),actual)
        return text,actual
    def test_rational_truth_borders_scales_neutral_signed_and_headroom(self):
        for w,h in ((1,1),(1,7),(9,1),(5,4)):
            data=array.array('f',[(i%23-5)/16 for i in range(w*h*3)])
            for space in WEIGHTS:
                s=raw.RasterSession(data,w,h,space)
                try:
                    for radius in (1,2,3):
                        for amount in (0.,.5,1.,2.,2**-40):self.assert_map(s,source_doc(s),dict(amount=amount,radius=radius))
                finally:s.close()
        for space in WEIGHTS:
            data=array.array('f',[v for x in range(19) for v in ((.25,)*3 if x<9 else (.75,)*3)])
            s=raw.RasterSession(data,19,1,space)
            try:
                _,out=self.assert_map(s,source_doc(s),dict(amount=1,radius=3));v=values(out)
                self.assertAlmostEqual(v[8*3],.0357142857142857,delta=1e-7)
                self.assertAlmostEqual(v[9*3],.9642857142857143,delta=1e-7)
                self.assertEqual(v[0],.25);self.assertEqual(v[-1],.75)
                self.assertTrue(all(v[i]==v[i+1]==v[i+2] for i in range(0,len(v),3)))
            finally:s.close()
    def test_identity_constant_bits_disabled_and_requested_reduction_order(self):
        for rgb in ((-0.,0.,-0.),(2**-149,)*3,(3.4028234663852886e38,)*3,(-3.4028234663852886e38,)*3,(-.125,.5,1.25)):
            data=array.array('f',rgb*9);s=raw.RasterSession(data,3,3,'prophoto-d50')
            try:
                base=source_doc(s);original=s.render_manifest(json.dumps(base))
                for amount in (0.,1.,2.):self.assertEqual(s.render_manifest(json.dumps(add(base,dict(amount=amount,radius=3)))),original)
                disabled=add(base);disabled['operations'][-1]['enabled']=False
                self.assertEqual(s.render_manifest(json.dumps(disabled)),original)
            finally:s.close()
        data=array.array('f',[v for x in (.125,.25,.75,.875)*2 for v in (x,x,x)])
        s=raw.RasterSession(data,4,2,'prophoto-d50')
        try:
            base=source_doc(s);p=dict(amount=1,radius=1)
            text,native=self.assert_map(s,base,p)
            _,reduced=self.assert_map(s,base,p,dict(mip=1,quality='preview'))
            n=values(native);before=array.array('f',[sum(n[(y*4+x+dx)*3+c] for y in (0,1) for dx in (0,1))/4 for x in (0,2) for c in range(3)])
            self.assertNotEqual(before.tobytes(),reduced[2])
        finally:s.close()
    def test_roi_true_halo_geometry_and_analysis_composition(self):
        data=array.array('f',[(i%29-5)/32 for i in range(11*9*3)])
        s=raw.RasterSession(data,11,9,'prophoto-d50')
        try:
            base=source_doc(s)
            for kind,p in (('crop',dict(x=2,y=1,width=7,height=7)),
                ('orientation',dict(quarter_turns=1,flip_horizontal=True,flip_vertical=False)),
                ('resize',dict(width=7,height=5,filter='bilinear')),
                ('convolution',dict(width=3,height=1,coefficients=[.25,.5,.25],border='replicate'))):
                transformed=operation(base,kind,p,id=80)
                for mip in (0,1,2):self.assert_map(s,transformed,req=dict(mip=mip,quality='preview' if mip else 'final'))
            text,full=self.assert_map(s,base,dict(amount=.5,radius=3))
            roi=s.render_manifest(text,dict(x=4,y=3,roi_width=1,roi_height=1,tile_size=1))
            self.assertEqual(roi[2],full[2][(3*11+4)*12:(3*11+5)*12])
            footprints=s.required_source_regions(text,dict(x=4,y=3,roi_width=1,roi_height=1))
            self.assertEqual(list(footprints.values()),[(1,0,7,7)])
            identity=json.dumps(add(base,dict(amount=0,radius=3)))
            self.assertEqual(list(s.required_source_regions(identity,dict(x=4,y=3,roi_width=1,roi_height=1)).values()),[(4,3,1,1)])
            tiles=[];s.analyze_local_manifest(text,tiles.append,dict(x=4,y=3,roi_width=1,roi_height=1),radius=1)
            mean=array.array('d');mean.frombytes(tiles[0]['mean_f64']);v=values(full)
            for c in range(3):self.assertAlmostEqual(mean[c],sum(v[(y*11+x)*3+c] for y in range(2,5) for x in range(3,6))/9,delta=1e-12)
        finally:s.close()
    def test_jobs_cache_history_radius_amount_and_ownership(self):
        data=array.array('f',[(i%29-5)/32 for i in range(9*7*3)])
        s=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(s);p=dict(amount=.5,radius=1);text=json.dumps(add(base,p));req=dict(tile_size=64)
            original=s.render_manifest(text,req);before=s.cache_stats();p['amount']=1
            self.assertEqual(s.submit_manifest(text,req).result(timeout=5),original)
            self.assertEqual(s.cache_stats()['misses'],before['misses'])
            for controls in (dict(amount=1.5,radius=1),dict(amount=.5,radius=3),dict(amount=0,radius=1),dict(amount=0,radius=3)):
                revised_text=json.dumps(add(base,controls));revised=s.render_manifest(revised_text,req);after=s.cache_stats()
                self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses']);before=after
            self.assertEqual(s.submit_manifest_latest('view',revised_text,req).result(timeout=5),revised)
            jobs=[s.submit_manifest(text,req) for _ in range(4)]
            self.assertTrue(all(j.result(timeout=5)==original for j in jobs))
            canceled=s.submit_manifest(text,dict(tile_size=1));canceled.cancel()
            try:canceled.result(timeout=5)
            except RuntimeError:pass  # Cancellation is tile-boundary cooperative; completion may win.
            history=s.history(text);history.commit(revised_text)
            self.assertEqual(history.render(req),revised);self.assertTrue(history.undo());self.assertEqual(history.render(req),original)
            restored=s.restore_history(history.save());self.assertEqual(restored.render(req),original)
            self.assertEqual(s.histogram_manifest(text)['descriptor']['primaries'],'rec2020')
            data[0]=999;self.assertEqual(s.render_manifest(text,req),original)
            restored.close();history.close()
        finally:s.close()
    def test_calibrated_raw_demosaicers_spaces_levels_and_footprints(self):
        data=array.array('H',((i*719)%50000 for i in range(99)))
        meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            s=raw.RawSession(data,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                for space in WEIGHTS:
                    doc=json.loads(s.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),working_space=space,output_mode='srgb-preview')))
                    base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
                    for mip in (0,1,2):
                        req=dict(mip=mip,quality='preview' if mip else 'final')
                        self.assert_map(s,base,dict(amount=.5,radius=3),req,space)
                    text=json.dumps(add(base,dict(amount=.5,radius=1)))
                    req=dict(x=1,y=1,roi_width=1,roi_height=1,mip=1,quality='preview')
                    # Reduced 3x3 halo maps to6x6 native at active origin1,1;
                    # demosaic support clips to the true active area.
                    expected=(1,1,7,7) if algorithm=='rawengine.bilinear' else (1,1,9,7)
                    self.assertEqual(list(s.required_source_regions(text,req).values()),[expected])
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(add(source_doc(s),DEFAULT,'camera_linear')))
            finally:s.close()
    def test_multisource_conversion_replacement_and_pinned_history(self):
        a,b='96000000-0000-0000-0000-000000000001','96000000-0000-0000-0000-000000000002'
        spec=dict(rgb=array.array('f',[(i%19)/16 for i in range(7*5*3)]),width=7,height=5,working_space='prophoto-d50')
        s=raw.RasterGraphSession({a:spec,b:{**spec,'rgb':array.array('f',[.5]*105)}})
        try:
            exported=json.loads(s.export_manifest(a));base=tap(exported,a)
            mix=operation(base,'linear_mix',dict(amount=.25),id=80)
            mix['operations'][-1]['inputs']=dict(base=a,layer=b)
            converted=operation(mix,'working_space_convert',{},id=81);converted['operations'][-1]['output_domain']='scene_linear_rec2020_d65'
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final')
                text,original=self.assert_map(s,converted,req=req,space='rec2020-d65')
                self.assertEqual(set(s.required_source_regions(text,req)),{a,b})
            history=s.history(text);pinned=history.render(req)
            s.replace_source(a,{**spec,'rgb':array.array('f',[.125]*105)})
            self.assertEqual(history.render(req),pinned)
            with self.assertRaises(ValueError):s.render_manifest(text,req)
            revised=json.loads(text);revised['sources']=json.loads(s.export_manifest(a))['sources']
            self.assertNotEqual(s.render_manifest(json.dumps(revised),req),pinned)
            history.close()
        finally:s.close()
    def test_strict_parameters_versions_domains_levels_and_close(self):
        s=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(s)
        try:
            invalid=[{},dict(DEFAULT,extra=0),dict(amount=0),dict(radius=3)]
            invalid += [dict(DEFAULT,amount=v) for v in (True,None,'0',[],float('nan'),float('inf'),-.01,2.01)]
            invalid += [dict(DEFAULT,radius=v) for v in (True,None,'3',[],1.,0,4,-1,2**40)]
            for p in invalid:
                for enabled in (True,False):
                    doc=add(base,p);doc['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):s.render_manifest(json.dumps(doc))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    doc=add(base);doc['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):s.render_manifest(json.dumps(doc))
            for change in ({'input_domain':'scene_linear_rec2020_d65'},{'output_domain':'scene_linear_rec2020_d65'},
                {'opacity':.5},{'blend_mode':'multiply'},{'inputs':{}},{'extensions':{'test':1}}):
                doc=add(base);doc['operations'][-1].update(change)
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(doc))
            encoded=json.loads(s.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):s.render_manifest(json.dumps(add(encoded,DEFAULT,'encoded_srgb')))
            text=json.dumps(add(base))
            for req in ({'mip':1,'quality':'final'},{'mip':3,'quality':'preview'},{'x':1,'roi_width':1}):
                with self.assertRaises(ValueError):s.render_manifest(text,req)
        finally:s.close()
        with self.assertRaises(RuntimeError):s.render_manifest(text)
if __name__=='__main__':unittest.main()
