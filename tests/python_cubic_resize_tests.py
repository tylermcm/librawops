"""Independent rational tensor-product truth and saved-graph integration."""
import array
import copy
from fractions import Fraction as F
import json
import math
import sys
import unittest
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw

def values(result):
    a=array.array('f');a.frombytes(result[2]);return a
def tap(doc,output):
    doc=copy.deepcopy(doc);ops={o['id']:o for o in doc['operations']};needed=set()
    def visit(id):
        if id not in ops or id in needed:return
        needed.add(id)
        for upstream in ops[id]['inputs'].values():visit(upstream)
    visit(output);doc.update(output=output,operations=[o for o in doc['operations'] if o['id'] in needed]);return doc
def source_doc(s):
    d=json.loads(s.export_manifest());return tap(d,d['sources'][0]['id'])
def operation(doc,kind,p,domain=None,id=90):
    doc=copy.deepcopy(doc);domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    node=dict(id=f'98000000-0000-0000-0000-{id:012d}',type='rawengine.'+kind,schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,inputs=dict(image=doc['output']),parameters=copy.deepcopy(p),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc
def add(doc,W=7,H=5,domain=None):return operation(doc,'cubic_resize',dict(width=W,height=H),domain)
def rational_axis(N,O,index):
    if N==O:return [(index,F(1))]
    z=min(max(F((2*index+1)*N,2*O)-F(1,2),0),N-1);s=max(F(1),F(N,O));weights=[]
    for i in range(math.floor(z-2*s),math.ceil(z+2*s)+1):
        t=abs((i-z)/s)
        # Expanded exact rational polynomial, independent of native factored FP.
        w=(F(3,2)*t**3-F(5,2)*t**2+1 if t<1 else -F(1,2)*t**3+F(5,2)*t**2-4*t+2 if t<2 else F(0))/s
        if w:weights.append((min(max(i,0),N-1),w))
    total=sum(w for _,w in weights);return [(i,w/total) for i,w in weights]
def oracle(source,OW,OH,mip=0):
    W,H=source[:2];v=values(source);result=array.array('f');xs=[rational_axis(W,OW,x) for x in range(OW)];ys=[rational_axis(H,OH,y) for y in range(OH)]
    for y in range(OH):
        for x in range(OW):
            for c in range(3):result.append(float(sum(wx*wy*F(v[(iy*W+ix)*3+c]) for ix,wx in xs[x] for iy,wy in ys[y])))
    return reduce(result,OW,OH,mip)
def reduce(v,W,H,mip):
    if not mip:return v
    s=1<<mip;out=array.array('f')
    for y in range(0,H,s):
        for x in range(0,W,s):
            coords=[(yy*W+xx)*3 for yy in range(y,min(y+s,H)) for xx in range(x,min(x+s,W))]
            for c in range(3):out.append(sum(v[i+c] for i in coords)/len(coords))
    return out

class CubicResizeTests(unittest.TestCase):
    def assert_map(self,s,base,W=7,H=5,mip=0,space=None):
        domain=None if space is None else ('scene_linear_prophoto_d50' if space=='prophoto-d50' else 'scene_linear_rec2020_d65')
        text=json.dumps(add(base,W,H,domain));req=dict(mip=mip,quality='preview' if mip else 'final');expected=oracle(s.render_manifest(json.dumps(base)),W,H,mip);actual=s.render_manifest(text,req)
        scale=1<<mip;self.assertEqual(actual[:2],((W+scale-1)//scale,(H+scale-1)//scale))
        for a,b in zip(values(actual),expected):self.assertAlmostEqual(a,b,delta=4e-7*max(1,abs(b)))
        for size in (1,3,64):self.assertEqual(s.render_manifest(text,{**req,'tile_size':size}),actual)
        return text,actual
    def test_rational_sharp_and_widened_reconstruction_thin_mixed_axes_and_mips(self):
        for W,H in ((1,1),(1,7),(9,1),(5,4),(12,8)):
            for space in ('prophoto-d50','rec2020-d65'):
                s=raw.RasterSession(array.array('f',[(i%23-5)/8 for i in range(W*H*3)]),W,H,space)
                try:
                    for OW,OH in ((W,H),(W*2+1,H*2+1),((W+3)//4,(H+3)//4),(W,H*2+1)):
                        for mip in (0,1,2):self.assert_map(s,source_doc(s),OW,OH,mip)
                finally:s.close()
    def test_identity_constants_extremes_native_reduction_and_legacy_controls(self):
        for rgb in ((-0.,0.,-0.),(2**-149,)*3,(3.4028234663852886e38,)*3,(-3.4028234663852886e38,)*3,(-.125,.5,1.25)):
            s=raw.RasterSession(array.array('f',rgb*9),3,3,'prophoto-d50')
            try:
                base=source_doc(s);original=s.render_manifest(json.dumps(base))
                self.assertEqual(s.render_manifest(json.dumps(add(base,3,3))),original)
                out=s.render_manifest(json.dumps(add(base,7,5)))
                self.assertEqual(out[2],array.array('f',rgb*35).tobytes())
                disabled=add(base,11,9);disabled['operations'][-1]['enabled']=False;self.assertEqual(s.render_manifest(json.dumps(disabled)),original)
            finally:s.close()
        s=raw.RasterSession(array.array('f',[(i%31)/16 for i in range(9*7*3)]),9,7,'prophoto-d50')
        try:
            base=source_doc(s);text,result=self.assert_map(s,base,11,9,1);native=s.render_manifest(text);self.assertEqual(result[2],reduce(values(native),11,9,1).tobytes())
            self.assertNotEqual(result[2],oracle(s.render_manifest(json.dumps(base),dict(mip=1,quality='preview')),6,5).tobytes())
            for f in ('nearest','bilinear','area'):
                legacy=operation(base,'resize',dict(width=11,height=9,filter=f));before=s.render_manifest(json.dumps(legacy));self.assertNotEqual(native[2],before[2]);self.assertEqual(before,s.render_manifest(json.dumps(legacy)))
        finally:s.close()
    def test_strict_parameters_relative_admission_disabled_domains_levels_close(self):
        s=raw.RasterSession(array.array('f',[.2,.4,.6]*25),5,5,'prophoto-d50');base=source_doc(s)
        try:
            good=dict(width=2,height=2);invalid=[{},dict(good,extra=0)]+[dict(good,**{k:v}) for k in ('width','height') for v in (True,None,'2',2.,0,-1,2**32,1)]
            for p in invalid:
                for enabled in (True,False):
                    d=operation(base,'cubic_resize',p);d['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):s.render_manifest(json.dumps(d))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    d=add(base);d['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):s.render_manifest(json.dumps(d))
            encoded=json.loads(s.export_manifest(dict(output_mode='srgb-preview')))
            for enabled in (True,False):
                d=add(encoded,5,5,'encoded_srgb');d['operations'][-1]['enabled']=enabled
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(d))
            for change in ({'input_domain':'scene_linear_rec2020_d65'},{'output_domain':'scene_linear_rec2020_d65'},{'opacity':.5},{'blend_mode':'multiply'},{'inputs':{}},{'extensions':{'test':1}}):
                d=add(base);d['operations'][-1].update(change)
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(d))
            text=json.dumps(add(base))
            for req in ({'mip':0,'quality':'preview'},{'mip':1,'quality':'final'},{'mip':3,'quality':'preview'},{'x':7,'roi_width':1}):
                with self.assertRaises(ValueError):s.render_manifest(text,req)
        finally:s.close()
        with self.assertRaises(RuntimeError):s.render_manifest(text)

    def test_geometry_chains_roi_analysis_and_source_support(self):
        s=raw.RasterSession(array.array('f',[(i%29-5)/32 for i in range(11*9*3)]),11,9,'prophoto-d50')
        try:
            base=source_doc(s)
            for kind,p in (('crop',dict(x=2,y=1,width=7,height=7)),('orientation',dict(quarter_turns=1,flip_horizontal=True,flip_vertical=False)),('resize',dict(width=7,height=5,filter='bilinear')),('rotate',dict(angle_degrees=7.5)),('convolution',dict(width=3,height=1,coefficients=[.25,.5,.25],border='replicate'))):
                chain=operation(base,kind,p,id=80)
                for mip in (0,1,2):self.assert_map(s,chain,9,7,mip)
            text,full=self.assert_map(s,base,9,7);roi=s.render_manifest(text,dict(x=4,y=3,roi_width=2,roi_height=2,tile_size=1));self.assertEqual(roi[2],b''.join(full[2][(y*9+4)*12:(y*9+6)*12] for y in (3,4)))
            crop=operation(base,'crop',dict(x=2,y=1,width=7,height=7),id=80);identity=json.dumps(add(crop,7,7));self.assertEqual(list(s.required_source_regions(identity,dict(x=2,y=1,roi_width=1,roi_height=1)).values()),[(4,2,1,1)])
            tiles=[];s.analyze_local_manifest(text,tiles.append,dict(x=4,y=3,roi_width=1,roi_height=1),radius=1);mean=array.array('d');mean.frombytes(tiles[0]['mean_f64']);v=values(full)
            for c in range(3):self.assertAlmostEqual(mean[c],sum(v[(y*9+x)*3+c] for y in range(2,5) for x in range(3,6))/9,delta=1e-12)
            for kind,p in (('crop',dict(x=1,y=1,width=7,height=5)),('resize',dict(width=9,height=7,filter='area')),('orientation',dict(quarter_turns=3,flip_horizontal=False,flip_vertical=True)),('rotate',dict(angle_degrees=13))):
                chain=operation(json.loads(text),kind,p,id=91);whole=s.render_manifest(json.dumps(chain));self.assertEqual(s.render_manifest(json.dumps(chain),dict(tile_size=1)),whole)
        finally:s.close()
    def test_jobs_cache_history_immutable_source(self):
        data=array.array('f',[(i%29-5)/32 for i in range(9*7*3)]);s=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(s);text=json.dumps(add(base,9,7));req=dict(tile_size=64);original=s.render_manifest(text,req);before=s.cache_stats();self.assertEqual(s.submit_manifest(text,req).result(timeout=5),original);self.assertEqual(s.cache_stats()['misses'],before['misses'])
            changed=json.dumps(add(base,7,5));revised=s.render_manifest(changed,req);after=s.cache_stats();self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses'])
            self.assertEqual(s.submit_manifest_latest('view',changed,req).result(timeout=5),revised);self.assertTrue(all(j.result(timeout=5)==original for j in [s.submit_manifest(text,req) for _ in range(4)]))
            canceled=s.submit_manifest(text,dict(tile_size=1));canceled.cancel()
            try:canceled.result(timeout=5)
            except RuntimeError:pass
            history=s.history(text);history.commit(changed);self.assertEqual(history.render(req),revised);self.assertTrue(history.undo());self.assertEqual(history.render(req),original);restored=s.restore_history(history.save());self.assertEqual(restored.render(req),original)
            self.assertEqual(s.histogram_manifest(text)['descriptor']['primaries'],'rec2020');data[0]=999;self.assertEqual(s.render_manifest(text,req),original);restored.close();history.close()
        finally:s.close()
    def test_raw_native_origins_demosaicers_working_spaces_and_halos(self):
        data=array.array('H',((i*719)%50000 for i in range(99)));meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            s=raw.RawSession(data,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                for space in ('prophoto-d50','rec2020-d65'):
                    d=json.loads(s.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),working_space=space,output_mode='srgb-preview')));base=tap(d,next(o['id'] for o in d['operations'] if o['type']=='rawengine.camera_to_working'))
                    for mip in (0,1,2):self.assert_map(s,base,9,7,mip,space)
                    text=json.dumps(add(base,9,7));self.assertEqual(list(s.required_source_regions(text,dict(x=0,y=0,roi_width=1,roi_height=1)).values()),[(1,1,2,2) if algorithm=='rawengine.bilinear' else (1,1,7,7)])
                    disabled=add(base,11,9);disabled['operations'][-1]['enabled']=False;self.assertEqual(s.render_manifest(json.dumps(disabled)),s.render_manifest(json.dumps(base)))
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(add(source_doc(s),9,7,'camera_linear')))
            finally:s.close()
    def test_multisource_conversion_replacement_and_history_pinning(self):
        a,b='98000000-0000-0000-0000-000000000001','98000000-0000-0000-0000-000000000002';spec=dict(rgb=array.array('f',[(i%19)/16 for i in range(7*5*3)]),width=7,height=5,working_space='prophoto-d50');s=raw.RasterGraphSession({a:spec,b:{**spec,'rgb':array.array('f',[.5]*105)}})
        try:
            base=tap(json.loads(s.export_manifest(a)),a);mix=operation(base,'linear_mix',dict(amount=.25),id=80);mix['operations'][-1]['inputs']=dict(base=a,layer=b);converted=operation(mix,'working_space_convert',{},id=81);converted['operations'][-1]['output_domain']='scene_linear_rec2020_d65'
            for mip in (0,1,2):text,result=self.assert_map(s,converted,9,7,mip,'rec2020-d65');self.assertEqual(set(s.required_source_regions(text,dict(mip=mip,quality='preview' if mip else 'final'))),{a,b})
            history=s.history(text);pinned=history.render();s.replace_source(a,{**spec,'rgb':array.array('f',[.125]*105)});self.assertEqual(history.render(),pinned)
            with self.assertRaises(ValueError):s.render_manifest(text)
            revised=json.loads(text);revised['sources']=json.loads(s.export_manifest(a))['sources'];self.assertNotEqual(s.render_manifest(json.dumps(revised)),pinned);history.close()
        finally:s.close()
if __name__=='__main__':unittest.main()
