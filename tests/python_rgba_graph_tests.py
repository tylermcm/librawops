"""Frozen native RGBA truth through owned saved sessions, jobs and history."""
import array
import copy
import ctypes
import gc
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import unittest

sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw


def uid(n): return '82000000-0000-4000-8000-{:012d}'.format(n)
A,B,M = [uid(n) for n in (1,2,3)]


def packed(words): return struct.pack('<{}I'.format(len(words)),*words)
def floats(words):
    out=array.array('f');out.frombytes(packed(words));return out


def frames():
    header=(Path(__file__).parent/'reference/rgba_native_v1.hpp').read_text(encoding='utf-8')
    for line in header.splitlines():
        if not re.match(r'^\{\d,\d,',line): continue
        text=re.sub(r'0x([0-9a-f]+)u',lambda m:str(int(m.group(1),16)),line.rstrip(','))
        frame=json.loads(text.replace('{','[').replace('}',']'))
        frame[6]=frame[6][0] # C++ std::array aggregate has an extra brace layer.
        yield frame


def operation(n,kind,inputs,domain,output=None):
    return dict(id=uid(n),type=kind,schema_version=1,processing_version=2,enabled=True,
                input_domain=domain,output_domain=output or domain,parameters={},inputs=inputs,
                masks={},blend_mode='normal',opacity=1)


def fixture(frame):
    kind,space,bounds,source,backdrop,mask,levels,digest=frame
    x,y,w,h=bounds
    working='prophoto-d50' if space==1 else 'rec2020-d65'
    rgbdomain='scene_linear_prophoto_d50' if space==1 else 'scene_linear_rec2020_d65'
    domain='premultiplied_'+rgbdomain
    rgba=lambda words:dict(rgba=floats(words),width=w,height=h,x=x,y=y,working_space=working)
    inputs={A:rgba(source)}
    ops=[]
    if kind==1:
        x=y=0
        inputs={A:dict(rgb=floats([v for i,v in enumerate(source) if i%4!=3]),width=w,height=h,working_space=working),
                M:dict(coverage=floats(source[3::4]),width=w,height=h)}
        ops=[operation(4,'rawengine.rgba_premultiply',{'image':A,'alpha':M},rgbdomain,domain)]
    elif kind==2:
        inputs[M]=dict(coverage=floats(mask),width=w,height=h,x=x,y=y)
        ops=[operation(4,'rawengine.rgba_apply_coverage',{'image':A,'coverage':M},domain)]
    elif kind==3:
        inputs[B]=rgba(backdrop)
        ops=[operation(4,'rawengine.rgba_source_over',{'source':A,'backdrop':B},domain)]
    elif kind==4:
        ops=[operation(4,'rawengine.rgba_alpha',{'image':A},domain,'coverage')]
    elif kind==5:
        ops=[operation(4,'rawengine.rgba_straight_rgb',{'image':A},domain,rgbdomain)]
    session=raw.RasterGraphSession(inputs,workers=2)
    document=json.loads(session.export_manifest(A))
    document.update(format_version=5,operations=ops,output=uid(4) if ops else A)
    return session,document,inputs,(x,y,w,h)


class RgbaGraphTests(unittest.TestCase):
    def test_all_frozen_frames_spaces_levels_roi_jobs_and_ownership(self):
        count=0
        for frame in frames():
            kind,space,_,source,_,_,levels,digest=frame
            session,document,inputs,(x,y,w,h)=fixture(frame)
            text=json.dumps(document)
            if kind!=1:
                data=b'librawops.source.rgba.premult.f32.v1\0'+struct.pack('<5I',x,y,w,h,space)+packed(source)
                self.assertEqual(session.source_info()[A]['content_sha256'],hashlib.sha256(data).hexdigest())
                self.assertEqual(json.loads(session.export_manifest(A))['format_version'],5)
            for spec in inputs.values():
                buffer=spec.get('rgba',spec.get('rgb',spec.get('coverage')))
                buffer[:]=array.array('f',[99])*len(buffer)
            render=session.render_rgba_manifest if kind<=3 else session.render_coverage_manifest if kind==4 else session.render_manifest
            submit=session.submit_rgba_manifest if kind<=3 else session.submit_coverage_manifest if kind==4 else session.submit_manifest
            latest=session.submit_rgba_manifest_latest if kind<=3 else session.submit_coverage_manifest_latest if kind==4 else session.submit_manifest_latest
            channels=4 if kind<=3 else 1 if kind==4 else 3
            for mip,quality in ((0,'final'),(0,'preview'),(1,'preview'),(2,'preview')):
                scale=1<<mip;dims=((w+scale-1)//scale,(h+scale-1)//scale)
                expected=(*dims,packed(levels[mip]))
                options=dict(mip=mip,quality=quality,tile_size=2)
                for size in (1,2,8): self.assertEqual(render(text,{**options,'tile_size':size}),expected)
                job=submit(text,options);self.assertEqual(job.output_kind(),'rgba' if kind<=3 else 'coverage' if kind==4 else 'rgb')
                self.assertEqual(job.result(timeout=5),expected)
                self.assertEqual(latest('view',text,options).result(timeout=5),expected)
                self.assertTrue(job.done())
                self.assertEqual(job.progress(),dict(completed_tiles=((dims[0]+1)//2)*((dims[1]+1)//2),
                    total_tiles=((dims[0]+1)//2)*((dims[1]+1)//2)))
                for oy in range(dims[1]):
                    for ox in range(dims[0]):
                        roi={**options,'x':ox if mip else x+ox,'y':oy if mip else y+oy,'roi_width':1,'roi_height':1}
                        offset=(oy*dims[0]+ox)*channels*4
                        self.assertEqual(render(text,roi),(1,1,expected[2][offset:offset+channels*4]))
                        footprint=(x+ox*scale,y+oy*scale,min(scale,w-ox*scale),min(scale,h-oy*scale))
                        self.assertEqual(session.required_source_regions(text,roi),{key:footprint for key in inputs})
            render(text);before=session.cache_stats();render(text)
            self.assertEqual(session.cache_stats()['misses'],before['misses'])
            if kind<=3:
                self.assertEqual(render(text,dict(x=x+w,y=y+h,roi_width=0,roi_height=0)),(0,0,b''))
                with self.assertRaises(ValueError):session.render_manifest(text)
                with self.assertRaises(ValueError):session.render_coverage_manifest(text)
                with self.assertRaises(ValueError):session.submit_manifest(text)
                with self.assertRaises(ValueError):session.submit_coverage_manifest(text)
            else:
                with self.assertRaises(ValueError):session.render_rgba_manifest(text)
                with self.assertRaises(ValueError):session.submit_rgba_manifest(text)
            session.close();count+=1
        self.assertEqual(count,84)

    def test_strict_schema_sources_disabled_bypass_and_cache_identity(self):
        frame=list(frames())[3*7+3]
        session,document,_,bounds=fixture(frame)
        before=session.render_rgba_manifest(json.dumps(document))
        for enabled in (True,False):
            for field,value in [('parameters',{'unknown':0}),('opacity',.5),('blend_mode','multiply'),
                                ('input_domain','coverage'),('output_domain','coverage'),
                                ('schema_version',2),('processing_version',1),('inputs',{'source':A}),
                                ('masks',{'coverage':A}),('unknown',True)]:
                bad=copy.deepcopy(document);bad['operations'][0]['enabled']=enabled;bad['operations'][0][field]=value
                with self.assertRaises(ValueError,msg=field):session.render_rgba_manifest(json.dumps(bad))
        for format_version in (1,2,3,4):
            bad=copy.deepcopy(document);bad['format_version']=format_version
            with self.assertRaises(ValueError):session.render_rgba_manifest(json.dumps(bad))
        bad=copy.deepcopy(document);bad['operations'][0]['enabled']=False
        result=session.render_rgba_manifest(json.dumps(bad))
        self.assertEqual(result[2],packed(frame[4]))
        self.assertEqual(set(session.required_source_regions(json.dumps(bad))),{B})
        for kind in (1,4,5):
            other,doc,_,_=fixture(list(frames())[kind*7+3]);doc['operations'][0]['enabled']=False
            with self.assertRaises(ValueError):other.render_rgba_manifest(json.dumps(doc))
            other.close()
        x,y,w,h=bounds
        good=dict(rgba=floats(frame[3]),width=w,height=h,x=x,y=y)
        for change in ({'rgb':array.array('f',[0])*w*h*3},{'coverage':array.array('f',[0])*w*h},
                       {'input_profile':None},{'width':True},{'height':0},{'x':-1},
                       {'row_stride_samples':True},{'row_stride_samples':4*w-1},
                       {'row_stride_samples':2**64-1},{'working_space':'srgb'},
                       {'rgba':array.array('d',[0])*w*h*4}):
            with self.assertRaises((ValueError,TypeError,OverflowError)):raw.RasterGraphSession({A:{**good,**change}})
        for pixel in ([1,0,0,0],[0,0,0,-1],[0,0,0,2],[float('nan'),0,0,1]):
            with self.assertRaises(ValueError):raw.RasterGraphSession({A:dict(rgba=array.array('f',pixel),width=1,height=1)})
        with self.assertRaises((ValueError,BufferError)):raw.RasterGraphSession({A:{**good,'rgba':memoryview(good['rgba'])[::2]}})
        native=(ctypes.c_float*(w*h*4))(*good['rgba'])
        typed=raw.RasterGraphSession({A:{**good,'rgba':native}})
        self.assertEqual(typed.render_rgba_manifest(typed.export_manifest(A))[2],packed(frame[3]));typed.close()
        padded=array.array('f',[float('nan')])*((4*w+1)*h)
        for row in range(h):padded[row*(4*w+1):row*(4*w+1)+4*w]=good['rgba'][row*4*w:(row+1)*4*w]
        padding=raw.RasterGraphSession({A:{**good,'rgba':padded,'row_stride_samples':4*w+1}})
        self.assertEqual(padding.source_info()[A]['content_sha256'],session.source_info()[A]['content_sha256']);padding.close()
        for options in ({'tile_size':0},{'mip':1,'quality':'final'},{'x':x+w,'roi_width':1}):
            with self.assertRaises(ValueError):session.render_rgba_manifest(json.dumps(document),options)
        with self.assertRaises(ValueError):session.submit_rgba_manifest_latest('',json.dumps(document))
        self.assertEqual(session.render_rgba_manifest(json.dumps(document)),before)
        session.close()

    def test_history_source_replacement_pinning_mixed_output_and_lifetime(self):
        session,doc,_,bounds=fixture(list(frames())[3*7+3]);text=json.dumps(doc)
        initial=session.render_rgba_manifest(text);history=session.history(text,max_revisions=8)
        self.assertEqual(history.render_rgba(),initial)
        rgba2=copy.deepcopy(doc);rgba2['operations'][0]['enabled']=False
        second=history.commit(json.dumps(rgba2));self.assertEqual(second,2)
        result=history.render_rgba()
        self.assertEqual(history.compare_rgba(1,2),(initial,result))
        self.assertEqual(history.submit_rgba().result(timeout=5),result)
        self.assertEqual(history.submit_rgba_latest('history').result(timeout=5),result)
        pinned=history.submit_rgba(revision=1)
        scalar=copy.deepcopy(doc);scalar['operations'].append(operation(5,'rawengine.rgba_alpha',{'image':uid(4)},doc['operations'][0]['output_domain'],'coverage'))
        scalar['output']=uid(5);self.assertEqual(history.commit(json.dumps(scalar)),3)
        with self.assertRaises(ValueError):history.render_rgba()
        with self.assertRaises(ValueError):history.compare_rgba(1,3)
        self.assertEqual(history.render_coverage()[2],initial[2][12:16]+b''.join(initial[2][i+12:i+16] for i in range(16,len(initial[2]),16)))
        saved=history.save();restored=session.restore_history(saved);self.assertEqual(restored.save(),saved)
        self.assertEqual(history.undo(),2);self.assertEqual(history.redo(),3)
        bad=copy.deepcopy(doc);bad['operations'][0]['inputs'].pop('source')
        stats=history.stats()
        with self.assertRaises(ValueError):history.commit(json.dumps(bad))
        self.assertEqual(history.stats(),stats)
        x,y,w,h=bounds
        replacement=dict(rgba=array.array('f',[1,2,3,1])*w*h,width=w,height=h,x=x,y=y)
        session.replace_source(A,replacement)
        with self.assertRaises(ValueError):session.render_rgba_manifest(text)
        with self.assertRaises(ValueError):session.restore_history(saved)
        self.assertEqual(pinned.result(timeout=5),initial)
        newer=copy.deepcopy(doc);newer['sources']=json.loads(session.export_manifest(A))['sources']
        self.assertNotEqual(session.render_rgba_manifest(json.dumps(newer)),initial)
        with self.assertRaises(ValueError):history.commit(json.dumps(newer))
        session.close();self.assertEqual(history.render_rgba(revision=1),initial)
        job=history.submit_rgba(revision=1);del session,history,restored;gc.collect()
        self.assertEqual(job.result(timeout=5),initial)

    def test_cancellation_overflow_and_session_recovery(self):
        size=256
        session=raw.RasterGraphSession({A:dict(rgba=array.array('f',[1,2,3,1])*size*size,width=size,height=size)},cache_bytes=1)
        text=session.export_manifest(A)
        job=session.submit_rgba_manifest(text,{'tile_size':1});job.cancel()
        with self.assertRaises(raw.RenderCancelled):job.result(timeout=5)
        self.assertEqual(session.render_rgba_manifest(text,dict(roi_width=1,roi_height=1)),(1,1,array.array('f',[1,2,3,1]).tobytes()))
        self.assertEqual(session.cache_stats()['entries'],0)
        session.close()
        with self.assertRaisesRegex(RuntimeError,'closed'):session.render_rgba_manifest(text)
        tiny=raw.RasterGraphSession({A:dict(rgba=packed([0x7f7fffff,0,0,1]),width=1,height=1)})
        doc=json.loads(tiny.export_manifest(A));doc['operations']=[operation(4,'rawengine.rgba_straight_rgb',{'image':A},'premultiplied_scene_linear_prophoto_d50','scene_linear_prophoto_d50')];doc['output']=uid(4)
        with self.assertRaises((ValueError,OverflowError)):tiny.render_manifest(json.dumps(doc))
        with self.assertRaises((ValueError,RuntimeError,OverflowError)):tiny.submit_manifest(json.dumps(doc)).result(timeout=5)
        self.assertEqual(tiny.render_rgba_manifest(tiny.export_manifest(A))[2],packed([0x7f7fffff,0,0,1]));tiny.close()


if __name__=='__main__':unittest.main()
