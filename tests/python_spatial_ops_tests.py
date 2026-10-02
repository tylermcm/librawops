"""Independent true-border/window/convolution oracles and streamed ownership."""
import array
import copy
import json
import math
import sys
import unittest

sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw

def uid(n): return f"20000000-0000-0000-0000-{n:012d}"
def tap(document,output):
    doc=copy.deepcopy(document); ops={op['id']:op for op in doc['operations']}; needed=set()
    def visit(id):
        if id not in ops or id in needed: return
        needed.add(id)
        for parent in ops[id]['inputs'].values(): visit(parent)
    visit(output)
    doc.update(output=output,operations=[op for op in doc['operations'] if op['id'] in needed])
    return doc
def source_doc(session):
    doc=json.loads(session.export_manifest()); return tap(doc,doc['sources'][0]['id'])
def convolution(doc,width=3,height=1,coefficients=(1,0,-1),domain=None):
    doc=copy.deepcopy(doc)
    if domain is None:
        domain='scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65'
    op=dict(id=uid(90),type='rawengine.convolution',schema_version=1,processing_version=2,enabled=True,
        input_domain=domain,output_domain=domain,inputs=dict(image=doc['output']),
        parameters=dict(width=width,height=height,coefficients=list(coefficients),border='replicate'),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(op); doc['output']=op['id']; return doc
def rgb(result):
    data=array.array('f'); data.frombytes(result[2]); return data
def streamed(session,text,request=None,radius=3):
    result={}; retained=[]
    def accept(tile):
        retained.append(tile)
        mean=array.array('d'); mean.frombytes(tile['mean_f64'])
        variance=array.array('d'); variance.frombytes(tile['variance_f64'])
        count=array.array('I'); count.frombytes(tile['finite_count_u32'])
        x,y,w,h=tile['bounds']
        for row in range(h):
            for col in range(w):
                i=(row*w+col)*3
                result[x+col,y+row]=(tuple(mean[i:i+3]),tuple(variance[i:i+3]),tuple(count[i:i+3]))
    progress=session.analyze_local_manifest(text,accept,request,radius=radius)
    assert progress==dict(completed_tiles=len(retained),pixel_count=len(result))
    return result,retained
def window_oracle(values,w,h,x,y,radius,c):
    window=[values[(yy*w+xx)*3+c] for yy in range(max(0,y-radius),min(h,y+radius+1))
            for xx in range(max(0,x-radius),min(w,x+radius+1)) if math.isfinite(values[(yy*w+xx)*3+c])]
    if not window: return float('nan'),float('nan'),0
    origin=window[0]; delta=[v-origin for v in window]
    mean=math.fsum(delta)/len(window)
    variance=math.fsum((d-mean)**2 for d in delta)/len(window)
    return origin+mean,variance,len(window)
def convolution_oracle(values,w,h,kernel,kw,kh):
    result=array.array('f')
    # Flip the kernel once, then walk the neighborhood in the usual direction.
    weights=list(reversed(kernel))
    for y in range(h):
        for x in range(w):
            for c in range(3):
                samples=[]
                for dy in range(kh):
                    for dx in range(kw):
                        xx=max(0,min(w-1,x+dx-kw//2)); yy=max(0,min(h-1,y+dy-kh//2))
                        samples.append(values[(yy*w+xx)*3+c]*weights[dy*kw+dx])
                result.append(math.fsum(samples))
    return result

class SpatialTests(unittest.TestCase):
    def pixels(self,w=9,h=7):
        return array.array('f',((i*37%129-42)/64 for i in range(w*h*3)))
    def assert_window_truth(self,session,text,request=None,radius=3):
        output=session.render_manifest(text,request); values=rgb(output)
        data,tiles=streamed(session,text,request,radius)
        x0=min(x for x,y in data); y0=min(y for x,y in data)
        # Full output/level is rendered independently; windows can extend beyond
        # an inspection ROI, so only use this oracle for full output requests.
        for (x,y),(means,variances,counts) in data.items():
            for c in range(3):
                expected=window_oracle(values,output[0],output[1],x-x0,y-y0,radius,c)
                self.assertAlmostEqual(means[c],expected[0],delta=1e-12*max(1,abs(expected[0])))
                self.assertAlmostEqual(variances[c],expected[1],delta=1e-12*max(1,abs(expected[1])))
                self.assertEqual(counts[c],expected[2])
        return data,tiles
    def test_local_truth_partitions_roi_and_spaces(self):
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(self.pixels(),9,7,space)
            text=json.dumps(source_doc(session))
            for radius in (0,1,3,8):
                full,tiles=self.assert_window_truth(session,text,radius=radius)
                for size in (1,2,4):
                    self.assertEqual(streamed(session,text,dict(tile_size=size),radius)[0],full)
                interior=streamed(session,text,dict(x=3,y=3,roi_width=1,roi_height=1,tile_size=1),radius)[0]
                self.assertEqual(interior[(3,3)],full[(3,3)])
            before=session.source_info()
            session.clear_cache(); streamed(session,text,dict(tile_size=4),3); stats=session.cache_stats()
            streamed(session,text,dict(tile_size=4),3)
            self.assertEqual(stats['misses'],session.cache_stats()['misses'])
            self.assertGreater(session.cache_stats()['hits'],stats['hits'])
            self.assertEqual(before,session.source_info())
            session.close()
    def test_convolution_numeric_identity_and_true_borders(self):
        original=self.pixels()
        original[0]=-0.0
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(original,9,7,space)
            base=source_doc(session)
            for kw,kh,kernel in ((3,1,[1,0,-1]),(1,3,[1,2,3]),(3,3,[0,.125,0,.125,.5,.125,0,.125,0]),
                                  (5,1,[0.0625,.25,.375,.25,.0625])):
                doc=convolution(base,kw,kh,kernel); text=json.dumps(doc)
                expected=convolution_oracle(original,9,7,kernel,kw,kh)
                result=session.render_manifest(text)
                for actual,truth in zip(rgb(result),expected): self.assertAlmostEqual(actual,truth,delta=2e-7*max(1,abs(truth)))
                for tile in (1,2,4): self.assertEqual(session.render_manifest(text,dict(tile_size=tile)),result)
                self.assert_window_truth(session,text,radius=1)
                doc['operations'][-1]['enabled']=False
                self.assertEqual(session.render_manifest(json.dumps(doc))[2],original.tobytes())
            identity=convolution(base,3,3,[0,0,0,0,1,0,0,0,0])
            text=json.dumps(identity)
            self.assertEqual(session.render_manifest(text)[2],original.tobytes())
            self.assertEqual(session.required_source_regions(text,dict(x=3,y=3,roi_width=1,roi_height=1)),
                             {base['sources'][0]['id']:(3,3,1,1)})
            session.close()
    def test_geometry_levels_and_composed_support(self):
        session=raw.RasterSession(self.pixels(11,9),11,9,'prophoto-d50')
        doc=json.loads(session.export_manifest(dict(crop=(1,1,9,7),rotate=90,resize=(9,5),resize_filter='area',output_mode='srgb-preview')))
        selected=next(op['id'] for op in doc['operations'] if op['type']=='rawengine.resize')
        base=tap(doc,selected); filtered=convolution(base,3,1,[.25,.5,.25]); text=json.dumps(filtered)
        for mip in (0,1,2):
            req=dict(mip=mip,quality='preview' if mip else 'final')
            ref=session.render_manifest(json.dumps(base),req)
            expected=convolution_oracle(rgb(ref),ref[0],ref[1],[.25,.5,.25],3,1)
            self.assertEqual(session.render_manifest(text,req)[2],expected.tobytes())
            truth,_=self.assert_window_truth(session,text,req,radius=1)
            self.assertEqual(streamed(session,text,{**req,'tile_size':1},1)[0],truth)
        simple=convolution(source_doc(session),3,3,[1/9]*9)
        op=copy.deepcopy(simple['operations'][-1]); op['id']=uid(91); op['inputs']['image']=simple['output']
        simple['operations'].append(op); simple['output']=op['id']
        text=json.dumps(simple)
        source=simple['sources'][0]['id']
        self.assertEqual(session.required_source_regions(text,dict(x=4,y=4,roi_width=1,roi_height=1)),{source:(2,2,5,5)})
        self.assertEqual(session.required_source_regions(text,dict(x=2,y=2,roi_width=1,roi_height=1,mip=1,quality='preview')),
                         {source:(0,0,10,9)})
        session.close()
    def test_raw_taps_and_reduced_calibrated_windows(self):
        samples=array.array('H',((i*719)%50000 for i in range(99)))
        meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=(1000,)*4,white_levels=(30000,)*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            session=raw.RawSession(samples,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            source=json.dumps(source_doc(session)); self.assert_window_truth(session,source,radius=1)
            doc=json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),output_mode='srgb-preview')))
            selected=next(op['id'] for op in doc['operations'] if op['type']=='rawengine.camera_to_working')
            filtered=convolution(tap(doc,selected),3,1,[.25,.5,.25]); text=json.dumps(filtered)
            for mip in (0,1,2):
                req=dict(mip=mip,quality='preview' if mip else 'final')
                full,_=self.assert_window_truth(session,text,req,1)
                self.assertEqual(streamed(session,text,{**req,'tile_size':2},1)[0],full)
            camera=convolution(source_doc(session),domain='camera_linear')
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(camera))
            session.close()
    def test_multisource_cache_revision_history_and_jobs(self):
        a,b=uid(1),uid(2)
        spec=dict(rgb=self.pixels(),width=9,height=7,working_space='prophoto-d50')
        session=raw.RasterGraphSession({a:spec,b:{**spec,'rgb':array.array('f',[.5]*189)}})
        doc=json.loads(session.export_manifest(a))
        mix=dict(id=uid(89),type='rawengine.linear_mix',schema_version=1,processing_version=2,enabled=True,
                 input_domain='scene_linear_prophoto_d50',output_domain='scene_linear_prophoto_d50',inputs=dict(base=a,layer=b),
                 parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1)
        doc.update(operations=[mix],output=mix['id']); doc=convolution(doc); text=json.dumps(doc)
        original=session.render_manifest(text,dict(tile_size=3)); self.assert_window_truth(session,text,radius=1)
        self.assertEqual(session.submit_manifest(text,dict(tile_size=3)).result(timeout=5),original)
        history=session.history(text); saved=history.save()
        changed=copy.deepcopy(doc); changed['operations'][-1]['parameters']['coefficients']=[.25,.5,.25]
        changed_text=json.dumps(changed); before=session.cache_stats()
        revised=session.render_manifest(changed_text,dict(tile_size=3))
        self.assertNotEqual(revised,original); self.assertGreater(session.cache_stats()['hits'],before['hits'])
        history.commit(changed_text); self.assertEqual(history.render(dict(tile_size=3)),revised)
        restored=session.restore_history(saved); self.assertEqual(restored.render(dict(tile_size=3)),original)
        restored.close(); history.close(); session.close()
    def test_stream_callbacks_exceptions_snapshot_and_validation(self):
        session=raw.RasterSession(self.pixels(),9,7,'prophoto-d50'); text=json.dumps(source_doc(session))
        seen=[]
        class Stop(Exception): pass
        def fail(payload):
            seen.append(payload); raise Stop('stop after first tile')
        with self.assertRaisesRegex(Stop,'stop after first tile'):
            session.analyze_local_manifest(text,fail,dict(tile_size=1))
        self.assertEqual(len(seen),1)
        for radius in (-1,9):
            with self.assertRaises((ValueError,OverflowError)): session.analyze_local_manifest(text,seen.append,radius=radius)
        for radius in (True,1.0,'1'):
            with self.assertRaises(TypeError): session.analyze_local_manifest(text,seen.append,radius=radius)
        with self.assertRaises(TypeError): session.analyze_local_manifest(text,5)
        for request in ({'tile_size':0},{'radius':1},{'roi_width':0},{'mip':1,'quality':'final'}):
            with self.assertRaises(ValueError): session.analyze_local_manifest(text,seen.append,request)
        retained=[]
        def close_during_callback(payload):
            retained.append(payload); session.clear_cache(); session.close()
        progress=session.analyze_local_manifest(text,close_during_callback,dict(tile_size=3),radius=1)
        self.assertEqual(progress,dict(completed_tiles=9,pixel_count=63))
        self.assertEqual(len(retained),9)
        with self.assertRaises(RuntimeError): session.analyze_local_manifest(text,seen.append)
        self.assertEqual(len(retained[0]['mean_f64']),3*3*3*8)
    def test_convolution_parameter_validation_and_overflow(self):
        session=raw.RasterSession(self.pixels(),9,7,'prophoto-d50'); base=source_doc(session)
        doc=convolution(base)
        for change in (dict(width=0),dict(width=2),dict(width=True),dict(height=19),dict(coefficients=[1]),
                       dict(coefficients=[float('nan'),0,0]),dict(coefficients=[65537,0,0]),dict(border='mirror'),dict(extra=1)):
            bad=copy.deepcopy(doc); bad['operations'][-1]['parameters'].update(change)
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(bad))
        for field,value in (('schema_version',2),('processing_version',1)):
            bad=copy.deepcopy(doc); bad['operations'][-1][field]=value
            with self.assertRaises(ValueError): session.render_manifest(json.dumps(bad))
        display=json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
        bad=convolution(display,domain='display_encoded_srgb')
        with self.assertRaises(ValueError): session.render_manifest(json.dumps(bad))
        session.close()
        huge=raw.RasterSession(array.array('f',[3.4028234663852886e38]*3),1,1,'prophoto-d50')
        with self.assertRaises(ValueError): huge.render_manifest(json.dumps(convolution(source_doc(huge),1,1,[2])))
        huge.close()

if __name__=='__main__': unittest.main()
