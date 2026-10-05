"""Frozen brush replay through scalar/mixed Python graph/history/job ownership."""
import array
import copy
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
sys.path.insert(0,str(Path(__file__).parent/'reference'))
import brush_mask_oracle as oracle
from mask_graph_numeric_oracle import mix

HEADER=(Path(__file__).parent/'reference/brush_mask_v1.hpp').read_text(encoding='utf-8')


def words(name):
    match=re.search(r'inline constexpr std::uint32_t '+name+r'\[\] = \{([^}]+)\};',HEADER)
    return [int(value,16) for value in re.findall(r'0x([0-9a-f]+)u',match.group(1))]


def packed(values):
    return struct.pack('='+'I'*len(values),*values)


def uid(value):
    return f'00000000-0000-4000-8000-{value:012d}'


def number(word):
    return struct.unpack('=d',struct.pack('=Q',word))[0]


def parameters(strokes):
    return dict(strokes=[dict(mode='paint' if s['mode']==0 else 'erase',
        radius=number(s['radius']),hardness=number(s['hardness']),flow=number(s['flow']),opacity=number(s['opacity']),
        points=[[number(word) for word in p] for p in s['points']]) for s in strokes])


def operation(value,kind,inputs,params,domain='coverage',masks=None):
    return dict(id=uid(value),type=kind,schema_version=1,processing_version=2,enabled=True,
        input_domain=domain,output_domain=domain,parameters=params,inputs=inputs,masks=masks or {},
        blend_mode='normal',opacity=1)


def fixture(index=0,space=None):
    origin,width,height,_,strokes,*_=oracle.make_frames()[index]
    buffer=array.array('f');buffer.frombytes(packed(words(f'frame{index}_input')))
    specs={uid(1):dict(coverage=buffer,width=width,height=height,x=origin[0],y=origin[1])}
    if space:
        specs.update({uid(3):dict(rgb=array.array('f',[-1])*(width*height*3),width=width,height=height,working_space=space),
                      uid(4):dict(rgb=array.array('f',[3])*(width*height*3),width=width,height=height,working_space=space)})
    session=raw.RasterGraphSession(specs,workers=2)
    document=json.loads(session.export_manifest(uid(1)))
    document.update(operations=[operation(2,'rawengine.mask_brush',{'mask':uid(1)},parameters(strokes))],output=uid(2))
    return session,document,specs,(origin,width,height)


class BrushMaskTests(unittest.TestCase):
    def test_frozen_frames_levels_partitions_footprints_jobs_and_caller_ownership(self):
        for index in range(12):
            session,document,specs,(origin,width,height)=fixture(index)
            text=json.dumps(document)
            specs[uid(1)]['coverage'][:]=array.array('f',[99])*(width*height)
            for mip,quality in ((0,'final'),(0,'preview'),(1,'preview'),(2,'preview')):
                scale=1<<mip;w,h=(width+scale-1)//scale,(height+scale-1)//scale
                name=f'frame{index}_'+('native' if mip==0 else f'mip{mip}')
                expected=(w,h,packed(words(name)));options=dict(mip=mip,quality=quality,tile_size=2)
                for size in (1,2,8):
                    self.assertEqual(session.render_coverage_manifest(text,{**options,'tile_size':size}),expected)
                for y in range(h):
                    for x in range(w):
                        roi={**options,'x':origin[0]+x if mip==0 else x,'y':origin[1]+y if mip==0 else y,
                             'roi_width':1,'roi_height':1}
                        offset=(y*w+x)*4
                        self.assertEqual(session.render_coverage_manifest(text,roi),(1,1,expected[2][offset:offset+4]))
                        self.assertEqual(session.required_source_regions(text,roi),
                            {uid(1):(origin[0]+x*scale,origin[1]+y*scale,min(scale,width-x*scale),min(scale,height-y*scale))})
                job=session.submit_coverage_manifest_latest('paint',text,options)
                self.assertEqual(job.output_kind(),'coverage')
                self.assertEqual(job.result(timeout=5),expected)
                self.assertEqual(job.result(timeout=5),expected)
                self.assertEqual(job.progress(),dict(completed_tiles=((w+1)//2)*((h+1)//2),total_tiles=((w+1)//2)*((h+1)//2)))
            session.close()

    def test_strict_saved_tree_and_disabled_guards(self):
        session,document,_,_=fixture()
        variants=[]
        for change in ({'schema_version':2},{'processing_version':1},{'inputs':{'image':uid(1)}},
                       {'masks':{'coverage':uid(1)}},{'opacity':.5},{'blend_mode':'add'},{'extra':True},
                       {'input_domain':'scene_linear_prophoto_d50'},{'output_domain':'scene_linear_prophoto_d50'},
                       {'parameters':{'strokes':False}},{'parameters':{'strokes':[],'extra':1}},
                       {'inputs':{'mask':uid(2)}}):
            altered=copy.deepcopy(document);altered['operations'][0].update(change);variants.append(altered)
        for change in ({'mode':'add'},{'radius':True},{'hardness':'0.5'},{'flow':None},{'opacity':2},
                       {'radius':0},{'extra':1},{'points':[]},{'points':[[0,0]]},{'points':[[0,0,True]]},
                       {'points':[[2**34,0,1]]},{'points':[[0,0,-.1]]},{'points':False}):
            altered=copy.deepcopy(document);altered['operations'][0]['parameters']['strokes'][0].update(change);variants.append(altered)
        for altered in variants:
            for enabled in (True,False):
                altered['operations'][0]['enabled']=enabled
                with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(altered))
        for format in (1,2,3):
            altered=copy.deepcopy(document);altered['format_version']=format
            with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(altered))
        with self.assertRaises(ValueError):session.render_manifest(json.dumps(document))
        with self.assertRaises(ValueError):session.submit_manifest(json.dumps(document))
        disabled=copy.deepcopy(document);disabled['operations'][0]['enabled']=False
        self.assertEqual(session.render_coverage_manifest(json.dumps(disabled))[2],packed(words('frame0_input')))
        session.close()

    def test_cache_stroke_reorder_history_restore_source_pinning(self):
        session,document,_,_=fixture();text=json.dumps(document)
        expected=(7,5,packed(words('frame0_native')))
        self.assertEqual(session.render_coverage_manifest(text),expected)
        before=session.cache_stats();self.assertEqual(session.render_coverage_manifest(text),expected)
        after=session.cache_stats();self.assertEqual(before['misses'],after['misses']);self.assertGreater(after['hits'],before['hits'])
        changed=copy.deepcopy(document);changed['operations'][0]['parameters']['strokes'].reverse()
        reordered=session.render_coverage_manifest(json.dumps(changed))
        self.assertNotEqual(reordered,expected)
        after=session.cache_stats();self.assertGreater(after['misses'],before['misses']);self.assertGreater(after['hits'],before['hits'])
        history=session.history(text,max_revisions=8)
        second=history.commit(json.dumps(changed))
        self.assertEqual(history.compare_coverage(1,second),(expected,reordered))
        self.assertEqual(history.undo(),1);self.assertEqual(history.render_coverage(),expected)
        self.assertEqual(history.redo(),second);self.assertEqual(history.submit_coverage().result(timeout=5),reordered)
        saved=history.save();restored=session.restore_history(saved)
        self.assertEqual(restored.save(),saved);self.assertEqual(restored.render_coverage(),reordered)
        invalid=copy.deepcopy(changed);invalid['operations'][0]['parameters']['strokes'][0]['mode']='invalid'
        with self.assertRaises(ValueError):history.commit(json.dumps(invalid))
        self.assertEqual(history.save(),saved)
        old=session.submit_coverage_manifest(text)
        session.replace_source(uid(1),dict(coverage=array.array('f',[1])*35,width=7,height=5))
        with self.assertRaises(ValueError):session.render_coverage_manifest(text)
        with self.assertRaises(ValueError):session.restore_history(saved)
        self.assertEqual(old.result(timeout=5),expected)
        session.close();self.assertEqual(history.render_coverage(),reordered)
        pinned=history.submit_coverage();del session,history,restored;gc.collect()
        self.assertEqual(pinned.result(timeout=5),reordered)

    def test_masked_rgb_both_spaces_transform_halo_and_canonical_identity(self):
        for space in ('prophoto-d50','rec2020-d65'):
            session,document,_,_=fixture(space=space)
            domain='scene_linear_prophoto_d50' if space=='prophoto-d50' else 'scene_linear_rec2020_d65'
            document['operations'].append(operation(5,'rawengine.masked_mix',{'base':uid(3),'layer':uid(4)},
                                                   {'amount':1},domain,{'coverage':uid(2)}))
            document['output']=uid(5);text=json.dumps(document)
            for mip in (0,1,2):
                options=dict(mip=mip,quality='preview' if mip else 'final',tile_size=2)
                mask=words('frame0_'+('native' if mip==0 else f'mip{mip}'))
                expected=[]
                for value in mask:expected.extend(mix([0xbf800000]*3,[0x40400000]*3,value,0x3ff0000000000000))
                self.assertEqual(session.render_manifest(text,options)[2],packed(expected))
                self.assertEqual(session.submit_manifest(text,options).result(timeout=5)[2],packed(expected))
                self.assertEqual(set(session.required_source_regions(text,options)),{uid(1),uid(3),uid(4)})
            document['operations'] += [operation(6,'rawengine.box_blur',{'image':uid(5)},{'radius':1},domain),
                operation(7,'rawengine.crop',{'image':uid(6)},{'x':1,'y':1,'width':5,'height':3},domain)]
            document['output']=uid(7)
            for mip in (0,1):
                options=dict(mip=mip,quality='preview' if mip else 'final',x=0,y=0,roi_width=1,roi_height=1)
                expected=(0,0,4,4) if mip else (0,0,3,3)
                self.assertEqual(session.required_source_regions(json.dumps(document),options),
                                 {key:expected for key in (uid(1),uid(3),uid(4))})
            session.close()
        # Independent frozen operation bytes remain stable through manifest/history serialization.
        session,document,_,_=fixture()
        canonical=re.search(r'operation_json\[\] = R"json\((.*?)\)json";',HEADER).group(1)
        digest=re.search(r'operation_sha256\[\] = "([0-9a-f]+)";',HEADER).group(1)
        self.assertEqual(hashlib.sha256(canonical.encode()).hexdigest(),digest)
        document['operations']=[json.loads(canonical)]
        history=session.history(json.dumps(document));saved=json.loads(history.save())
        self.assertIn(canonical,saved['revisions'][0]['manifest'])
        session.close();history.close()


if __name__=='__main__':
    unittest.main()
