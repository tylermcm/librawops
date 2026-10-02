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

def axes(parameters):
    n=parameters['size']
    return [[a if i==0 else b if i==n-1 else a+(b-a)*(i/(n-1)) for i in range(n)]
        for a,b in zip(parameters['input_min'],parameters['input_max'])]

def table(n=3,fn=None,lo=(-1,0,-2),hi=(1,2,2)):
    p=dict(size=n,input_min=list(lo),input_max=list(hi));grid=axes(p)
    fn=fn or (lambda r,g,b:(r+.25*g-.125*b+.125*r*g,-.5*r+g+.25*b+.0625*g*b,
        .25*r-.125*g+b+.125*r*b+.03125*r*g*b))
    p['values']=[v for b in grid[2] for g in grid[1] for r in grid[0] for v in fn(r,g,b)]
    return p

IDENTITY=table(2,lambda r,g,b:(r,g,b),(0,0,0),(1,1,1))
TABLE=table()

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
    node=dict(id='93000000-0000-0000-0000-000000000090',type='rawengine.lut3d',schema_version=1,
        processing_version=2,enabled=True,input_domain=domain,output_domain=domain,
        inputs=dict(image=doc['output']),parameters=copy.deepcopy(parameters),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=node['id'];return doc

def truth(rgb,p,coordinates):
    # Exact eight-corner barycentric sum, independent of ordered native lerps.
    n=p['size'];left=[max(0,min(n-2,bisect.bisect_right(xs,float(v))-1)) for xs,v in zip(coordinates,rgb)]
    t=[(Fraction(float(v))-Fraction(xs[i]))/(Fraction(xs[i+1])-Fraction(xs[i])) for v,xs,i in zip(rgb,coordinates,left)]
    output=[]
    for c in range(3):
        value=Fraction(0)
        for b in range(2):
            for g in range(2):
                for r in range(2):
                    offset=3*(((left[2]+b)*n+left[1]+g)*n+left[0]+r)+c
                    weight=(t[0] if r else 1-t[0])*(t[1] if g else 1-t[1])*(t[2] if b else 1-t[2])
                    value+=Fraction(p['values'][offset])*weight
        output.append(float(value))
    return output

class Lut3DTests(unittest.TestCase):
    def assert_map(self,session,base,parameters=TABLE,request=None,domain=None):
        text=json.dumps(add(base,parameters,domain));original=session.render_manifest(json.dumps(base),request)
        actual=session.render_manifest(text,request);self.assertEqual(actual[:2],original[:2])
        coordinates=axes(parameters);before=values(original);after=values(actual)
        for i in range(0,len(before),3):
            expected=truth(before[i:i+3],parameters,coordinates)
            for c in range(3):self.assertAlmostEqual(after[i+c],expected[c],delta=4e-7*max(1,abs(expected[c])))
        for tile in (1,3,64):
            self.assertEqual(session.render_manifest(text,{**(request or {}),'tile_size':tile}),actual)
        self.assertEqual(session.required_source_regions(text,request),session.required_source_regions(json.dumps(base),request))
        return text,actual

    def test_rational_cross_terms_vertices_extrapolation_and_17_grid(self):
        points=[(r,g,b) for r in (-3,-1,-.5,0,.5,1,3) for g in (-2,0,1,2,4) for b in (-4,-2,0,2,4)]
        data=array.array('f',[v for p in points for v in p])
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(data,len(points),1,space)
            try:
                for p in (TABLE,table(2,lambda r,g,b:(1.25*r-.25*g+.125*b,-.5*r+g+.25*b,.25*r-.125*g+b)),
                    table(17,lambda r,g,b:(r*g+.25*b,g*b+.5*r,b*r+.125*g)),
                    table(7,lambda r,g,b:(.25,.5,.75)),table(7)):
                    self.assert_map(session,source_doc(session),p)
                output=values(self.assert_map(session,source_doc(session))[1])
                self.assertTrue(any(v<0 for v in output));self.assertTrue(any(v>1 for v in output))
            finally:session.close()

    def test_full_partial_identity_bits_tiny_box_and_near_identity(self):
        data=array.array('f',[-0.,0.,-0.,3.4028234663852886e38,-3.4028234663852886e38,2**-149])
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(data,2,1,space)
            try:
                base=source_doc(session);original=session.render_manifest(json.dumps(base))
                for n in (2,3,7,17):
                    p=table(n,lambda r,g,b:(r,g,b))
                    self.assertEqual(session.render_manifest(json.dumps(add(base,p))),original)
                tiny=table(2,lambda r,g,b:(r,g,b),(0,0,0),(1e-320,1e-320,1e-320))
                self.assertEqual(session.render_manifest(json.dumps(add(base,tiny))),original)
                partial=table(2,lambda r,g,b:(r,.25,b));result=session.render_manifest(json.dumps(add(base,partial)))[2]
                for i in (0,1):
                    for c in (0,2):self.assertEqual(result[i*12+c*4:i*12+c*4+4],data.tobytes()[i*12+c*4:i*12+c*4+4])
            finally:session.close()
        session=raw.RasterSession(array.array('f',[1,1,1]),1,1,'prophoto-d50')
        try:
            p=copy.deepcopy(IDENTITY);p['values'][21]=1+2**-22
            self.assertGreater(values(session.render_manifest(json.dumps(add(source_doc(session),p))))[0],1)
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
        session=raw.RasterSession(array.array('f',[0,1,0,1,0,0]),2,1,'prophoto-d50')
        try:
            mapping=table(2,lambda r,g,b:(r*g,g,b),(0,0,0),(1,1,1))
            text,result=self.assert_map(session,source_doc(session),mapping,dict(mip=1,quality='preview'))
            self.assertEqual(list(values(result)),[.25,.5,0]) # Native-before-average red would be 0.
        finally:session.close()

    def test_jobs_cache_history_analysis_and_ownership(self):
        data=array.array('f',[(i%29-10)/8 for i in range(9*7*3)])
        session=raw.RasterSession(data,9,7,'rec2020-d65')
        try:
            base=source_doc(session);parameters=copy.deepcopy(TABLE);doc=add(base,parameters);text=json.dumps(doc);req=dict(tile_size=3)
            original=session.render_manifest(text,req);parameters['values'][0]=999;before=session.cache_stats()
            self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original)
            self.assertEqual(session.cache_stats()['misses'],before['misses'])
            revised_table=copy.deepcopy(TABLE);revised_table['input_min'][0]=-2
            revised_text=json.dumps(add(base,revised_table));revised=session.render_manifest(revised_text,req);after=session.cache_stats()
            self.assertGreater(after['hits'],before['hits']);self.assertGreater(after['misses'],before['misses'])
            self.assertNotEqual(original,revised)
            sample_edit=copy.deepcopy(TABLE);sample_edit['values'][0]=-1
            self.assertNotEqual(session.render_manifest(json.dumps(add(base,sample_edit)),req),original)
            before_size=session.cache_stats();session.render_manifest(json.dumps(add(base,table(2))),req)
            self.assertGreater(session.cache_stats()['misses'],before_size['misses'])
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
            invalid=[{},dict(IDENTITY,extra=0)]
            for size in (0,1,18,2.0,True,-2,2**32,'2'):invalid.append(dict(IDENTITY,size=size))
            for field,v in (('input_min',[0,0]),('input_max',[1,1,1,1]),('input_min',[True,0,0]),
                ('input_min',[1,0,0]),('input_max',[65537,1,1]),('input_max',[1e-300,1,1]),
                ('values',[]),('values',IDENTITY['values'][:-1]),('values',IDENTITY['values']+[0]),
                ('values',[[0,1,2]]*8),('values',[True]+IDENTITY['values'][1:]),
                ('values',[65537]+IDENTITY['values'][1:])):invalid.append(dict(IDENTITY,**{field:v}))
            collapsed=table(3,lambda r,g,b:(0,0,0));collapsed['input_min'][0]=1;collapsed['input_max'][0]=1.0000000000000002
            invalid.append(collapsed)
            for p in invalid:
                for enabled in (True,False):
                    doc=add(base,p);doc['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for field,value in (('schema_version',2),('processing_version',1),('processing_version',3)):
                for enabled in (True,False):
                    doc=add(base);doc['operations'][-1].update({field:value,'enabled':enabled})
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            for v in (float('nan'),float('inf'),-float('inf')):
                for p in (dict(IDENTITY,input_min=[v,0,0]),dict(IDENTITY,values=[v]+IDENTITY['values'][1:])):
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(base,p)))
            disabled=add(base);disabled['operations'][-1]['enabled']=False
            self.assertEqual(session.render_manifest(json.dumps(disabled)),session.render_manifest(json.dumps(base)))
            encoded=json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(encoded,IDENTITY,'encoded_srgb')))
        finally:session.close()
        session=raw.RasterSession(array.array('f',[3.4028234663852886e38,0,0]),1,1,'prophoto-d50')
        p=table(2,lambda r,g,b:(2*r,g,b),(0,0,0),(1,1,1));text=json.dumps(add(source_doc(session),p))
        with self.assertRaises(ValueError):session.render_manifest(text)
        session.close()
        with self.assertRaises(RuntimeError):session.render_manifest(text)

if __name__=='__main__':unittest.main()
