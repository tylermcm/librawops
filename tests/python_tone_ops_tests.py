"""Independent curves/levels oracles and saved point-edit integration."""
import array
import copy
import json
import math
import sys
import unittest

sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw

def uid(n): return f'40000000-0000-0000-0000-{n:012d}'
IDENTITY=[[0,0],[1,1]]
CURVES=dict(red=[[0,0],[.25,.5],[1,1]],green=[[0,0],[.5,.25],[1,1]],blue=[[0,1],[1,0]])
LEVELS=dict(input_black=[.125,.25,-.25],input_white=[.875,.75,1.25],output_black=[0,-.5,.25],output_white=[1,1.5,1.25])
def source_doc(session):
    doc=json.loads(session.export_manifest()); doc.update(operations=[],output=doc['sources'][0]['id']); return doc
def tap(doc,output):
    doc=copy.deepcopy(doc); ops={o['id']:o for o in doc['operations']}; needed=set()
    def visit(id):
        if id not in ops or id in needed: return
        needed.add(id)
        for id in ops[id]['inputs'].values(): visit(id)
    visit(output); doc.update(output=output,operations=[o for o in doc['operations'] if o['id'] in needed]); return doc
def add(doc,kind,parameters,domain=None,id=90):
    doc=copy.deepcopy(doc)
    if domain is None: domain='scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65'
    operation=dict(id=uid(id),type='rawengine.'+kind,schema_version=1,processing_version=2,enabled=True,
        input_domain=domain,output_domain=domain,inputs=dict(image=doc['output']),parameters=copy.deepcopy(parameters),
        masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(operation); doc['output']=operation['id']; return doc
def values(result):
    values=array.array('f'); values.frombytes(result[2]); return values
def curve_value(x,points):
    # Linear scan and independently expressed interpolation/extrapolation.
    for px,py in points:
        if x==px: return py
    if x<points[0][0]: a,b=points[:2]
    elif x>points[-1][0]: a,b=points[-2:]
    else: a,b=next((a,b) for a,b in zip(points,points[1:]) if a[0]<x<b[0])
    return a[1]+(b[1]-a[1])*(x-a[0])/(b[0]-a[0])
def expected(data,kind,parameters):
    result=array.array('f')
    for i,x in enumerate(data):
        c=i%3
        if kind=='curves': result.append(curve_value(x,parameters[('red','green','blue')[c]]))
        else:
            low=parameters['input_black'][c]; high=parameters['input_white'][c]
            black=parameters['output_black'][c]; white=parameters['output_white'][c]
            result.append(black+(x-low)*(white-black)/(high-low))
    return result

class ToneTests(unittest.TestCase):
    def pixels(self,w=9,h=7): return array.array('f',((i*37%257-60)/128 for i in range(w*h*3)))
    def assert_map(self,session,base,kind,parameters,request=None):
        text=json.dumps(add(base,kind,parameters)); original=session.render_manifest(json.dumps(base),request)
        actual=session.render_manifest(text,request); truth=expected(values(original),kind,parameters)
        self.assertEqual(actual[:2],original[:2])
        for a,b in zip(values(actual),truth): self.assertAlmostEqual(a,b,delta=2e-7*max(1,abs(b)))
        for tile in (1,3,256): self.assertEqual(session.render_manifest(text,{**(request or {}),'tile_size':tile}),actual)
        return text,actual
    def test_numeric_endpoints_signed_headroom_and_identity(self):
        samples=(-1,-.5,-0.0,0,.125,.25,.5,.75,1,1.5,2)
        data=array.array('f',(v for v in samples for _ in range(3)))
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(data,11,1,space); base=source_doc(session)
            text,output=self.assert_map(session,base,'curves',CURVES)
            self.assertLess(values(output)[0],0); self.assertGreater(values(output)[-2],1)
            self.assertEqual(values(output)[12:15],array.array('f',[.25,.0625,.875]))
            self.assertEqual(session.histogram_manifest(text)['descriptor']['domain'],'scene_linear_rgb')
            self.assert_map(session,base,'levels',LEVELS)
            identity=dict(red=[[-8,-8],[-1,-1],[.5,.5],[9,9]],green=IDENTITY,blue=IDENTITY)
            self.assertEqual(session.render_manifest(json.dumps(add(base,'curves',identity)))[2],data.tobytes())
            plain=dict(input_black=[0]*3,input_white=[1]*3,output_black=[0]*3,output_white=[1]*3)
            self.assertEqual(session.render_manifest(json.dumps(add(base,'levels',plain)))[2],data.tobytes())
            disabled=add(base,'curves',CURVES); disabled['operations'][-1]['enabled']=False
            self.assertEqual(session.render_manifest(json.dumps(disabled))[2],data.tobytes())
            session.close()
    def test_geometry_reduced_mapping_and_no_extra_halo(self):
        session=raw.RasterSession(self.pixels(11,9),11,9,'prophoto-d50')
        doc=json.loads(session.export_manifest(dict(crop=(1,1,9,7),rotate=90,resize=(9,5),resize_filter='area',output_mode='srgb-preview')))
        base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.resize'))
        for kind,parameters in (('curves',CURVES),('levels',LEVELS)):
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final')
                text,output=self.assert_map(session,base,kind,parameters,req)
                roi={**req,'x':1,'y':0,'roi_width':1,'roi_height':1}
                self.assertEqual(session.render_manifest(text,roi)[2],output[2][12:24])
                self.assertEqual(session.required_source_regions(text,roi),session.required_source_regions(json.dumps(base),roi))
        # Nonlinear curves after reduction deliberately differ from native edit followed by average.
        plain=source_doc(session); text=json.dumps(add(plain,'curves',CURVES))
        native=values(session.render_manifest(text))
        expected_average=array.array('f')
        for c in range(3): expected_average.append(sum(native[(y*11+x)*3+c] for y in range(2) for x in range(2))/4)
        actual=values(session.render_manifest(text,dict(mip=1,quality='preview',roi_width=1,roi_height=1)))
        self.assertNotEqual(actual,expected_average)
        session.close()
    def test_raw_calibrated_taps_and_rejection(self):
        samples=array.array('H',((i*719)%50000 for i in range(99)))
        meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            session=raw.RawSession(samples,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            doc=json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),output_mode='srgb-preview')))
            base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
            for kind,params in (('curves',CURVES),('levels',LEVELS)):
                for mip in (0,1,2): self.assert_map(session,base,kind,params,dict(mip=mip,quality='preview' if mip else 'final'))
                with self.assertRaises(ValueError): session.render_manifest(json.dumps(add(source_doc(session),kind,params,domain='camera_linear')))
            session.close()
    def test_cache_history_jobs_analysis_and_source_ownership(self):
        data=self.pixels(); session=raw.RasterSession(data,9,7,'prophoto-d50'); base=source_doc(session)
        doc=add(add(base,'levels',LEVELS,id=89),'curves',CURVES); text=json.dumps(doc)
        result=session.render_manifest(text,dict(tile_size=3)); stats=session.cache_stats()
        self.assertEqual(session.submit_manifest(text,dict(tile_size=3)).result(timeout=5),result)
        self.assertEqual(stats['misses'],session.cache_stats()['misses'])
        changed=copy.deepcopy(doc); changed['operations'][-1]['parameters']['red']=[[0,0],[.25,.375],[1,1]]
        changed_text=json.dumps(changed); before=session.cache_stats()
        revised=session.render_manifest(changed_text,dict(tile_size=3)); self.assertNotEqual(result,revised)
        self.assertGreater(session.cache_stats()['hits'],before['hits'])
        history=session.history(text); saved=history.save(); history.commit(changed_text)
        self.assertEqual(history.render(dict(tile_size=3)),revised)
        self.assertTrue(history.undo()); self.assertEqual(history.render(dict(tile_size=3)),result)
        restored=session.restore_history(saved); self.assertEqual(restored.render(dict(tile_size=3)),result)
        tiles=[]; session.analyze_local_manifest(text,tiles.append,dict(tile_size=3),radius=1)
        self.assertTrue(tiles); self.assertEqual(tiles[0]['descriptor']['primaries'],'prophoto')
        data[0]=99; self.assertEqual(session.render_manifest(text,dict(tile_size=3)),result)
        self.assertEqual(json.loads(text),doc)
        restored.close(); history.close(); session.close()
    def test_multisource_branches_and_convolution_composition(self):
        a,b=uid(1),uid(2); spec=dict(rgb=self.pixels(),width=9,height=7,working_space='prophoto-d50')
        session=raw.RasterGraphSession({a:spec,b:{**spec,'rgb':array.array('f',[.5]*189)}})
        base=json.loads(session.export_manifest(a)); base=add(base,'curves',CURVES,id=88)
        mix=dict(id=uid(89),type='rawengine.linear_mix',schema_version=1,processing_version=2,enabled=True,
                 input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_prophoto_d50',inputs=dict(base=base['output'],layer=b),
                 parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1)
        base['operations'].append(mix); base['output']=mix['id']
        conv=copy.deepcopy(mix); conv.update(id=uid(90),type='rawengine.convolution',inputs=dict(image=base['output']),
                 parameters=dict(width=3,height=1,coefficients=[.25,.5,.25],border='replicate'))
        base['operations'].append(conv); base['output']=conv['id']; doc=add(base,'levels',LEVELS,id=91)
        text=json.dumps(doc)
        for mip in (0,1,2):
            req=dict(mip=mip,quality='preview' if mip else 'final')
            result=session.render_manifest(text,req)
            self.assertEqual(session.render_manifest(text,{**req,'tile_size':1}),result)
        roi=dict(x=3,y=3,roi_width=1,roi_height=1)
        self.assertEqual(session.required_source_regions(text,roi),{a:(2,3,3,1),b:(2,3,3,1)})
        session.close()
    def test_parameter_validation_extremes_and_errors(self):
        session=raw.RasterSession(self.pixels(),9,7,'prophoto-d50'); base=source_doc(session)
        bad_curves=([[0,0]],[[0,0],[0,1]],[[1,1],[0,0]],[[0,0],[1,float('nan')]],[[0,0],[1,65537]],
                    [[0,0],[1e-300,1]],[[False,0],[1,1]],[[0,0,0],[1,1]],[[i/256,i/256] for i in range(257)])
        for knots in bad_curves:
            doc=add(base,'curves',{**CURVES,'red':knots})
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(doc))
        for change in (dict(input_white=[0,1,1]),dict(input_white=[1]),dict(output_black=[2,0,0]),dict(output_white=[1,1,float('inf')]),dict(gamma=1)):
            doc=add(base,'levels',{**LEVELS,**change})
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(doc))
        for kind,params in (('curves',CURVES),('levels',LEVELS)):
            for field,value in (('schema_version',2),('processing_version',1)):
                doc=add(base,kind,params); doc['operations'][-1][field]=value
                with self.assertRaises(ValueError): session.render_manifest(json.dumps(doc))
            doc=add(base,kind,{**params,'extra':0})
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(doc))
            display=json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(add(display,kind,params,domain='display_encoded_srgb')))
        knots=[[i/255,i/255] for i in range(256)]; identity=dict(red=knots,green=knots,blue=knots)
        self.assertEqual(session.render_manifest(json.dumps(add(base,'curves',identity)))[2],self.pixels().tobytes())
        text=json.dumps(add(base,'curves',CURVES)); session.close()
        with self.assertRaises(RuntimeError): session.render_manifest(text)
        huge=raw.RasterSession(array.array('f',[3.4028234663852886e38]*3),1,1,'prophoto-d50')
        twice=dict(red=[[0,0],[1,2]],green=IDENTITY,blue=IDENTITY)
        with self.assertRaises(ValueError): huge.render_manifest(json.dumps(add(source_doc(huge),'curves',twice)))
        huge.close()

if __name__=='__main__': unittest.main()
