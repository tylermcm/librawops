"""Independent four-weight rotation truth and saved graph integration."""
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
    data=array.array('f');data.frombytes(result[2]);return data
def tap(doc,output):
    doc=copy.deepcopy(doc);ops={o['id']:o for o in doc['operations']};needed=set()
    def visit(id):
        if id not in ops or id in needed:return
        needed.add(id)
        for upstream in ops[id]['inputs'].values():visit(upstream)
    visit(output);doc.update(output=output,operations=[o for o in doc['operations'] if o['id'] in needed]);return doc
def source_doc(s):
    doc=json.loads(s.export_manifest());return tap(doc,doc['sources'][0]['id'])
def operation(doc,kind,p,domain=None,id=90):
    doc=copy.deepcopy(doc);domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    node=dict(id=f'97000000-0000-0000-0000-{id:012d}',type='rawengine.'+kind,schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,inputs=dict(image=doc['output']),parameters=copy.deepcopy(p),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc
def add(doc,angle=7.5,domain=None):return operation(doc,'rotate',dict(angle_degrees=angle),domain)
def oracle(source,angle,mip=0):
    W,H=source[:2];data=values(source);output=[];cx=(W-1)/2;cy=(H-1)/2
    if angle in (0,90,-90,180,-180):c,s={0:(1.,0.),90:(0.,1.),-90:(0.,-1.),180:(-1.,0.),-180:(-1.,0.)}[angle]
    else:t=(angle/180)*float.fromhex('0x1.921fb54442d18p+1');c,s=math.cos(t),math.sin(t)
    for y in range(H):
        for x in range(W):
            sx=min(max((cx+c*(x-cx))+s*(y-cy),0),W-1);sy=min(max((cy-s*(x-cx))+c*(y-cy),0),H-1)
            ix,iy=math.floor(sx),math.floor(sy);jx,jy=min(ix+1,W-1),min(iy+1,H-1);fx,fy=F(sx-ix),F(sy-iy)
            for channel in range(3):
                a,b,d,e=[F(data[(yy*W+xx)*3+channel]) for xx,yy in ((ix,iy),(jx,iy),(ix,jy),(jx,jy))]
                output.append(float((1-fx)*(1-fy)*a+fx*(1-fy)*b+(1-fx)*fy*d+fx*fy*e))
    native=array.array('f',output)
    if not mip:return native
    scale=1<<mip;result=array.array('f')
    for y in range(0,H,scale):
        for x in range(0,W,scale):
            coords=[(yy*W+xx)*3 for yy in range(y,min(y+scale,H)) for xx in range(x,min(x+scale,W))]
            for channel in range(3):result.append(sum(native[i+channel] for i in coords)/len(coords))
    return result

class RotateTests(unittest.TestCase):
    def test_saved_angle_keeps_double_precision_near_quarter_turn(self):
        data=array.array('f',[0.]*27);data[9:12]=array.array('f',[1.,1.,1.])
        s=raw.RasterSession(data,3,3,'prophoto-d50')
        try:
            base=source_doc(s);source=s.render_manifest(json.dumps(base));angle=90.-1e-8
            actual=s.render_manifest(json.dumps(add(base,angle)));quarter=s.render_manifest(json.dumps(add(base,90.)))
            self.assertNotEqual(actual[2],quarter[2]);self.assertGreater(values(actual)[0],0.)
            expected=oracle(source,angle)
            for value,target in zip(values(actual),expected):self.assertAlmostEqual(value,target,delta=1e-16)
            for mip in (1,2):
                text=json.dumps(add(base,angle));self.assertEqual(s.render_manifest(text,dict(mip=mip,quality='preview',tile_size=1)),s.render_manifest(text,dict(mip=mip,quality='preview',tile_size=64)))
        finally:s.close()
    def assert_map(self,s,base,angle=7.5,mip=0,space=None):
        domain=None if space is None else ('scene_linear_prophoto_d50' if space=='prophoto-d50' else 'scene_linear_rec2020_d65')
        text=json.dumps(add(base,angle,domain));source=s.render_manifest(json.dumps(base));req=dict(mip=mip,quality='preview' if mip else 'final')
        actual=s.render_manifest(text,req);expected=oracle(source,angle,mip)
        scale=1<<mip;self.assertEqual(actual[:2],((source[0]+scale-1)//scale,(source[1]+scale-1)//scale))
        for value,target in zip(values(actual),expected):self.assertAlmostEqual(value,target,delta=4e-7*max(1,abs(target)))
        for size in (1,3,64):self.assertEqual(s.render_manifest(text,{**req,'tile_size':size}),actual)
        return text,actual
    def test_independent_truth_thin_borders_signed_headroom_quarters_and_mips(self):
        for W,H in ((1,1),(1,7),(9,1),(5,4),(7,7)):
            for space in ('prophoto-d50','rec2020-d65'):
                s=raw.RasterSession(array.array('f',[(i%23-5)/8 for i in range(W*H*3)]),W,H,space)
                try:
                    for a in (-180,-90,-7.5,0,7.5,90,180):
                        for mip in (0,1,2):self.assert_map(s,source_doc(s),a,mip)
                finally:s.close()
    def test_identity_constant_bits_disabled_and_reduction_order(self):
        for rgb in ((-0.,0.,-0.),(2**-149,)*3,(3.4028234663852886e38,)*3,(-3.4028234663852886e38,)*3,(-.125,.5,1.25)):
            s=raw.RasterSession(array.array('f',rgb*9),3,3,'prophoto-d50')
            try:
                base=source_doc(s);original=s.render_manifest(json.dumps(base))
                for a in (-90,0,7.5,90):self.assertEqual(s.render_manifest(json.dumps(add(base,a))),original)
                doc=add(base);doc['operations'][-1]['enabled']=False;self.assertEqual(s.render_manifest(json.dumps(doc)),original)
            finally:s.close()
        s=raw.RasterSession(array.array('f',[(i%31)/16 for i in range(9*7*3)]),9,7,'prophoto-d50')
        try:
            base=source_doc(s);text,result=self.assert_map(s,base,13,1)
            reduced=s.render_manifest(json.dumps(base),dict(mip=1,quality='preview'))
            wrong=oracle(reduced,13);self.assertNotEqual(result[2],wrong.tobytes())
        finally:s.close()
    def test_geometry_chains_roi_and_analysis_footprints(self):
        s=raw.RasterSession(array.array('f',[(i%29-5)/32 for i in range(11*9*3)]),11,9,'prophoto-d50')
        try:
            base=source_doc(s)
            for kind,p in (('crop',dict(x=2,y=1,width=7,height=7)),('orientation',dict(quarter_turns=1,flip_horizontal=True,flip_vertical=False)),('resize',dict(width=7,height=5,filter='bilinear')),('convolution',dict(width=3,height=1,coefficients=[.25,.5,.25],border='replicate'))):
                doc=operation(base,kind,p,id=80)
                for mip in (0,1,2):self.assert_map(s,doc,mip=mip)
            text,full=self.assert_map(s,base);req=dict(x=4,y=3,roi_width=2,roi_height=2,tile_size=1)
            actual=s.render_manifest(text,req);expected=b''.join(full[2][((y*11+4)*12):((y*11+6)*12)] for y in (3,4));self.assertEqual(actual[2],expected)
            # A square quarter-turn maps one point exactly and excludes inactive taps.
            square=operation(base,'crop',dict(x=2,y=1,width=7,height=7),id=80);rot=json.dumps(add(square,90))
            self.assertEqual(list(s.required_source_regions(rot,dict(x=2,y=1,roi_width=1,roi_height=1)).values()),[(3,5,1,1)])
            tiles=[];s.analyze_local_manifest(text,tiles.append,dict(x=4,y=3,roi_width=1,roi_height=1),radius=1)
            mean=array.array('d');mean.frombytes(tiles[0]['mean_f64']);v=values(full)
            for channel in range(3):self.assertAlmostEqual(mean[channel],sum(v[(y*11+x)*3+channel] for y in range(2,5) for x in range(3,6))/9,delta=1e-12)
            for kind,p in (('crop',dict(x=1,y=1,width=7,height=5)),('resize',dict(width=9,height=7,filter='area')),('orientation',dict(quarter_turns=3,flip_horizontal=False,flip_vertical=True))):
                chained=operation(json.loads(text),kind,p,id=91);output=s.render_manifest(json.dumps(chained));self.assertEqual(s.render_manifest(json.dumps(chained),dict(tile_size=1)),output)
        finally:s.close()
    def test_jobs_cache_history_and_copied_source(self):
        data=array.array('f',[(i%29-5)/32 for i in range(9*7*3)]);s=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(s);text=json.dumps(add(base));req=dict(tile_size=64);original=s.render_manifest(text,req);before=s.cache_stats()
            self.assertEqual(s.submit_manifest(text,req).result(timeout=5),original);self.assertEqual(s.cache_stats()['misses'],before['misses'])
            changed=json.dumps(add(base,13));revised=s.render_manifest(changed,req);after=s.cache_stats();self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses'])
            self.assertEqual(s.submit_manifest_latest('view',changed,req).result(timeout=5),revised)
            self.assertTrue(all(j.result(timeout=5)==original for j in [s.submit_manifest(text,req) for _ in range(4)]))
            canceled=s.submit_manifest(text,dict(tile_size=1));canceled.cancel()
            try:canceled.result(timeout=5)
            except RuntimeError:pass
            history=s.history(text);history.commit(changed);self.assertEqual(history.render(req),revised);self.assertTrue(history.undo());self.assertEqual(history.render(req),original)
            restored=s.restore_history(history.save());self.assertEqual(restored.render(req),original);self.assertEqual(s.histogram_manifest(text)['descriptor']['primaries'],'rec2020')
            data[0]=999;self.assertEqual(s.render_manifest(text,req),original);restored.close();history.close()
        finally:s.close()
    def test_raw_demosaicers_spaces_native_rebased_mip_support(self):
        data=array.array('H',((i*719)%50000 for i in range(99)));meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            s=raw.RawSession(data,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                for space in ('prophoto-d50','rec2020-d65'):
                    doc=json.loads(s.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),working_space=space,output_mode='srgb-preview')));base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
                    for mip in (0,1,2):self.assert_map(s,base,mip=mip,space=space)
                    text=json.dumps(add(base,0));self.assertEqual(list(s.required_source_regions(text,dict(x=0,y=0,roi_width=1,roi_height=1)).values()),[(1,1,2,2) if algorithm=='rawengine.bilinear' else (1,1,7,7)])
                    disabled=add(base,0);disabled['operations'][-1]['enabled']=False
                    self.assertEqual(s.render_manifest(json.dumps(disabled)),s.render_manifest(json.dumps(base)))
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(add(source_doc(s),0,'camera_linear')))
            finally:s.close()
    def test_multisource_replacement_conversion_and_history_pinning(self):
        a,b='97000000-0000-0000-0000-000000000001','97000000-0000-0000-0000-000000000002';spec=dict(rgb=array.array('f',[(i%19)/16 for i in range(7*5*3)]),width=7,height=5,working_space='prophoto-d50')
        s=raw.RasterGraphSession({a:spec,b:{**spec,'rgb':array.array('f',[.5]*105)}})
        try:
            base=tap(json.loads(s.export_manifest(a)),a);mix=operation(base,'linear_mix',dict(amount=.25),id=80);mix['operations'][-1]['inputs']=dict(base=a,layer=b)
            converted=operation(mix,'working_space_convert',{},id=81);converted['operations'][-1]['output_domain']='scene_linear_rec2020_d65'
            for mip in (0,1,2):
                text,result=self.assert_map(s,converted,mip=mip,space='rec2020-d65');self.assertEqual(set(s.required_source_regions(text,dict(mip=mip,quality='preview' if mip else 'final'))),{a,b})
            history=s.history(text);pinned=history.render();s.replace_source(a,{**spec,'rgb':array.array('f',[.125]*105)});self.assertEqual(history.render(),pinned)
            with self.assertRaises(ValueError):s.render_manifest(text)
            revised=json.loads(text);revised['sources']=json.loads(s.export_manifest(a))['sources'];self.assertNotEqual(s.render_manifest(json.dumps(revised)),pinned);history.close()
        finally:s.close()
    def test_strict_parameters_versions_domains_levels_and_close(self):
        s=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(s)
        try:
            invalid=[{},dict(angle_degrees=0,extra=0)]+[dict(angle_degrees=v) for v in (True,None,'0',[],float('nan'),float('inf'),-180.01,180.01)]
            for p in invalid:
                for enabled in (True,False):
                    doc=operation(base,'rotate',p);doc['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):s.render_manifest(json.dumps(doc))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    doc=add(base);doc['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):s.render_manifest(json.dumps(doc))
            for change in ({'input_domain':'scene_linear_rec2020_d65'},{'output_domain':'scene_linear_rec2020_d65'},{'opacity':.5},{'blend_mode':'multiply'},{'inputs':{}},{'extensions':{'test':1}}):
                doc=add(base);doc['operations'][-1].update(change)
                with self.assertRaises(ValueError):s.render_manifest(json.dumps(doc))
            encoded=json.loads(s.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):s.render_manifest(json.dumps(add(encoded,0,'encoded_srgb')))
            text=json.dumps(add(base))
            for req in ({'mip':0,'quality':'preview'},{'mip':1,'quality':'final'},{'mip':3,'quality':'preview'},{'x':1,'roi_width':1}):
                with self.assertRaises(ValueError):s.render_manifest(text,req)
        finally:s.close()
        with self.assertRaises(RuntimeError):s.render_manifest(text)
if __name__=='__main__':unittest.main()
