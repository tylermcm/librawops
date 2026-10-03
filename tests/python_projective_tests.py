"""Rational homography/four-weight truth and saved-graph integration."""
import array
import copy
from fractions import Fraction as F
import json
import math
import sys
import unittest
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw
I=[1,0,0,0,1,0,0,0,1]
M=[1,.125,.1,-.125,1,-.2,.25,-.125,1]
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
def add(doc,W=7,H=5,m=None,domain=None):return operation(doc,'projective',dict(width=W,height=H,source_from_output=I if m is None else m),domain)
def oracle(source,OW,OH,m,mip=0):
    W,H=source[:2];data=values(source);m=[F(v) for v in m];result=array.array('f')
    for y in range(OH):
        for x in range(OW):
            u=F(0) if OW==1 else F(2*x,OW-1)-1;v=F(0) if OH==1 else F(2*y,OH-1)-1
            d=m[6]*u+m[7]*v+1;sx=min(max(((m[0]*u+m[1]*v+m[2])/d+1)*F(W-1,2),0),W-1);sy=min(max(((m[3]*u+m[4]*v+m[5])/d+1)*F(H-1,2),0),H-1)
            ix,iy=math.floor(sx),math.floor(sy);jx,jy=min(ix+1,W-1),min(iy+1,H-1);fx,fy=sx-ix,sy-iy
            for c in range(3):
                a,b,t,e=[F(data[(yy*W+xx)*3+c]) for xx,yy in ((ix,iy),(jx,iy),(ix,jy),(jx,jy))]
                result.append(float((1-fx)*(1-fy)*a+fx*(1-fy)*b+(1-fx)*fy*t+fx*fy*e))
    if not mip:return result
    s=1<<mip;out=array.array('f')
    for y in range(0,OH,s):
        for x in range(0,OW,s):
            coords=[(yy*OW+xx)*3 for yy in range(y,min(y+s,OH)) for xx in range(x,min(x+s,OW))]
            for c in range(3):out.append(sum(result[i+c] for i in coords)/len(coords))
    return out
class ProjectiveTests(unittest.TestCase):
    def assert_map(self,s,base,W=7,H=5,m=M,mip=0,space=None):
        domain=None if space is None else ('scene_linear_prophoto_d50' if space=='prophoto-d50' else 'scene_linear_rec2020_d65')
        text=json.dumps(add(base,W,H,m,domain));req=dict(mip=mip,quality='preview' if mip else 'final');source=s.render_manifest(json.dumps(base));expected=oracle(source,W,H,m,mip);actual=s.render_manifest(text,req)
        scale=1<<mip;self.assertEqual(actual[:2],((W+scale-1)//scale,(H+scale-1)//scale));self.assertEqual(len(values(actual)),len(expected))
        for a,b in zip(values(actual),expected):self.assertAlmostEqual(a,b,delta=4e-7*max(1,abs(b)))
        for size in (1,3,64):self.assertEqual(s.render_manifest(text,{**req,'tile_size':size}),actual)
        return text,actual
    def test_rational_mapping_reflection_shear_perspective_thin_and_mips(self):
        matrices=[I,[-1,0,0,0,1,0,0,0,1],[.5,.25,.125,-.125,1.5,-.25,0,0,1],M,[1,0,0,0,1,0,.75,0,1]]
        for W,H in ((1,1),(1,7),(9,1),(5,4)):
            for space in ('prophoto-d50','rec2020-d65'):
                s=raw.RasterSession(array.array('f',[(i%23-5)/8 for i in range(W*H*3)]),W,H,space)
                try:
                    for OW,OH in ((W,H),(7,5)):
                        for m in matrices:
                            for mip in (0,1,2):self.assert_map(s,source_doc(s),OW,OH,m,mip)
                finally:s.close()
    def test_identity_extremes_disabled_and_native_reduction_anchor(self):
        for rgb in ((-0.,0.,-0.),(2**-149,)*3,(3.4028234663852886e38,)*3,(-3.4028234663852886e38,)*3,(-.125,.5,1.25)):
            s=raw.RasterSession(array.array('f',rgb*9),3,3,'prophoto-d50')
            try:
                base=source_doc(s);original=s.render_manifest(json.dumps(base))
                for m in (I,M,[-1,0,0,0,1,0,0,0,1]):self.assertEqual(s.render_manifest(json.dumps(add(base,3,3,m))),original)
                disabled=add(base,11,9,M);disabled['operations'][-1]['enabled']=False;self.assertEqual(s.render_manifest(json.dumps(disabled)),original)
            finally:s.close()
        s=raw.RasterSession(array.array('f',[(i%31)/16 for i in range(9*7*3)]),9,7,'prophoto-d50')
        try:
            base=source_doc(s);text,result=self.assert_map(s,base,9,7,M,1)
            native=s.render_manifest(text);v=values(native);direct=array.array('f')
            for y in range(0,7,2):
                for x in range(0,9,2):
                    cells=[(yy*9+xx)*3 for yy in range(y,min(y+2,7)) for xx in range(x,min(x+2,9))]
                    for c in range(3):direct.append(sum(v[i+c] for i in cells)/len(cells))
            self.assertEqual(result[2],direct.tobytes())
            self.assertNotEqual(result[2],oracle(s.render_manifest(json.dumps(base),dict(mip=1,quality='preview')),5,4,M).tobytes())
        finally:s.close()
    def test_geometry_chains_roi_analysis_and_source_support(self):
        s=raw.RasterSession(array.array('f',[(i%29-5)/32 for i in range(11*9*3)]),11,9,'prophoto-d50')
        try:
            base=source_doc(s)
            for kind,p in (('crop',dict(x=2,y=1,width=7,height=7)),('orientation',dict(quarter_turns=1,flip_horizontal=True,flip_vertical=False)),('resize',dict(width=7,height=5,filter='bilinear')),('rotate',dict(angle_degrees=7.5)),('convolution',dict(width=3,height=1,coefficients=[.25,.5,.25],border='replicate'))):
                chain=operation(base,kind,p,id=80)
                for mip in (0,1,2):self.assert_map(s,chain,9,7,M,mip)
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
            base=source_doc(s);text=json.dumps(add(base,9,7,M));req=dict(tile_size=64);original=s.render_manifest(text,req);before=s.cache_stats();self.assertEqual(s.submit_manifest(text,req).result(timeout=5),original);self.assertEqual(s.cache_stats()['misses'],before['misses'])
            changed=json.dumps(add(base,7,5,I));revised=s.render_manifest(changed,req);after=s.cache_stats();self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses'])
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
                    for mip in (0,1,2):self.assert_map(s,base,9,7,M,mip,space)
                    text=json.dumps(add(base,9,7,I));self.assertEqual(list(s.required_source_regions(text,dict(x=0,y=0,roi_width=1,roi_height=1)).values()),[(1,1,2,2) if algorithm=='rawengine.bilinear' else (1,1,7,7)])
                    disabled=add(base,11,9);disabled['operations'][-1]['enabled']=False;self.assertEqual(s.render_manifest(json.dumps(disabled)),s.render_manifest(json.dumps(base)))
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(add(source_doc(s),9,7,I,'camera_linear')))
            finally:s.close()
    def test_multisource_conversion_replacement_and_history_pinning(self):
        a,b='98000000-0000-0000-0000-000000000001','98000000-0000-0000-0000-000000000002';spec=dict(rgb=array.array('f',[(i%19)/16 for i in range(7*5*3)]),width=7,height=5,working_space='prophoto-d50');s=raw.RasterGraphSession({a:spec,b:{**spec,'rgb':array.array('f',[.5]*105)}})
        try:
            base=tap(json.loads(s.export_manifest(a)),a);mix=operation(base,'linear_mix',dict(amount=.25),id=80);mix['operations'][-1]['inputs']=dict(base=a,layer=b);converted=operation(mix,'working_space_convert',{},id=81);converted['operations'][-1]['output_domain']='scene_linear_rec2020_d65'
            for mip in (0,1,2):text,result=self.assert_map(s,converted,9,7,M,mip,'rec2020-d65');self.assertEqual(set(s.required_source_regions(text,dict(mip=mip,quality='preview' if mip else 'final'))),{a,b})
            history=s.history(text);pinned=history.render();s.replace_source(a,{**spec,'rgb':array.array('f',[.125]*105)});self.assertEqual(history.render(),pinned)
            with self.assertRaises(ValueError):s.render_manifest(text)
            revised=json.loads(text);revised['sources']=json.loads(s.export_manifest(a))['sources'];self.assertNotEqual(s.render_manifest(json.dumps(revised)),pinned);history.close()
        finally:s.close()
    def test_strict_parameters_matrix_admission_disabled_versions_domains_levels_close(self):
        s=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(s);good=dict(width=1,height=1,source_from_output=I)
        try:
            invalid=[{},dict(good,extra=0)]+[dict(good,**{k:v}) for k in ('width','height') for v in (True,None,'1',1.,0,-1,2**32)]+[dict(good,source_from_output=v) for v in (None,True,'matrix',[],I+[0])]
            invalid += [dict(good,source_from_output=[v]+I[1:]) for v in (True,None,'1',float('nan'),float('inf'),17,0,2**-21)]
            invalid += [dict(good,source_from_output=m) for m in ([1,0,0,0,1,0,.75001,0,1],[1,0,0,0,1,0,0,0,2])]
            for p in invalid:
                for enabled in (True,False):
                    d=operation(base,'projective',p);d['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):s.render_manifest(json.dumps(d))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    d=add(base);d['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):s.render_manifest(json.dumps(d))
            for change in ({'input_domain':'scene_linear_rec2020_d65'},{'output_domain':'scene_linear_rec2020_d65'},{'opacity':.5},{'blend_mode':'multiply'},{'inputs':{}},{'extensions':{'test':1}}):
                d=add(base);d['operations'][-1].update(change)
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(d))
            encoded=json.loads(s.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):s.render_manifest(json.dumps(add(encoded,1,1,I,'encoded_srgb')))
            text=json.dumps(add(base,1,1))
            for req in ({'mip':0,'quality':'preview'},{'mip':1,'quality':'final'},{'mip':3,'quality':'preview'},{'x':1,'roi_width':1}):
                with self.assertRaises(ValueError):s.render_manifest(text,req)
        finally:s.close()
        with self.assertRaises(RuntimeError):s.render_manifest(text)
if __name__=='__main__':unittest.main()
