"""Frozen geometric masks through Python scalar/mixed graph/history/jobs."""
import array,copy,gc,hashlib,json,re,struct,sys,unittest
from pathlib import Path

sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw
sys.path.insert(0,str(Path(__file__).parent/'reference'))
import parametric_mask_oracle as oracle
from mask_graph_numeric_oracle import mix

HEADER=(Path(__file__).parent/'reference/parametric_mask_v1.hpp').read_text(encoding='utf-8')
METADATA=oracle.frame_metadata()


def uid(n):return f'00000000-0000-4000-8000-{n:012d}'
def packed(words):return struct.pack('='+'I'*len(words),*words)
def number(word):return struct.unpack('=d',struct.pack('=Q',word))[0]
def words(name):
    found=re.search(r'inline constexpr std::uint32_t '+name+r'\[\] = \{([^}]+)\};',HEADER)
    return [int(v,16) for v in re.findall(r'0x([0-9a-f]+)u',found.group(1))]
def parameters(shape):
    g=list(map(number,shape['geometry']))
    if shape['kind']==0:return dict(start=g[:2],end=g[2:4],invert=shape['invert'])
    if shape['kind']==1:return dict(center=g[:2],axes=g[2:6],inner=g[6],invert=shape['invert'])
    return dict(rings=[[[number(x),number(y)] for x,y in ring] for ring in shape['rings']],invert=shape['invert'])
def operation(n,kind,inputs,params,domain='coverage',masks=None):
    return dict(id=uid(n),type=kind,schema_version=1,processing_version=2,enabled=True,
        input_domain=domain,output_domain=domain,inputs=inputs,parameters=params,masks=masks or {},blend_mode='normal',opacity=1)
def fixture(index=0,space=None):
    origin,width,height,_,shape=METADATA[index];buf=array.array('f');buf.frombytes(packed(words(f'frame{index}_input')))
    specs={uid(1):dict(coverage=buf,width=width,height=height,x=origin[0],y=origin[1])}
    if space:
        specs.update({uid(3):dict(rgb=array.array('f',[-1])*(width*height*3),width=width,height=height,working_space=space),
                      uid(4):dict(rgb=array.array('f',[3])*(width*height*3),width=width,height=height,working_space=space),
                      uid(8):dict(coverage=array.array('f',[1])*(width*height),width=width,height=height)})
    session=raw.RasterGraphSession(specs,workers=2)
    names=['rawengine.mask_linear_gradient','rawengine.mask_radial_gradient','rawengine.mask_polygon']
    doc=json.loads(session.export_manifest(uid(1)))
    doc.update(operations=[operation(2,names[shape['kind']],{'mask':uid(1)},parameters(shape))],output=uid(2))
    return session,doc,specs,(origin,width,height)


class ParametricMaskTests(unittest.TestCase):
    def test_all_frozen_frames_levels_roi_partitions_jobs_ownership(self):
        for index in range(36):
            session,doc,specs,(origin,width,height)=fixture(index);text=json.dumps(doc)
            specs[uid(1)]['coverage'][:]=array.array('f',[99])*(width*height)
            for mip,quality in [(0,'final'),(0,'preview'),(1,'preview'),(2,'preview')]:
                scale=1<<mip;w,h=(width+scale-1)//scale,(height+scale-1)//scale
                expected=(w,h,packed(words(f'frame{index}_'+('native' if mip==0 else f'mip{mip}'))))
                options=dict(mip=mip,quality=quality,tile_size=2)
                for size in (1,2,8):self.assertEqual(session.render_coverage_manifest(text,{**options,'tile_size':size}),expected)
                for y in range(h):
                    for x in range(w):
                        roi={**options,'x':origin[0]+x if mip==0 else x,'y':origin[1]+y if mip==0 else y,'roi_width':1,'roi_height':1}
                        offset=(y*w+x)*4
                        self.assertEqual(session.render_coverage_manifest(text,roi),(1,1,expected[2][offset:offset+4]))
                        self.assertEqual(session.required_source_regions(text,roi),
                            {uid(1):(origin[0]+x*scale,origin[1]+y*scale,min(scale,width-x*scale),min(scale,height-y*scale))})
                job=session.submit_coverage_manifest_latest('geometry',text,options)
                self.assertEqual(job.output_kind(),'coverage');self.assertEqual(job.result(timeout=5),expected)
                self.assertEqual(job.result(timeout=5),expected)
                total=((w+1)//2)*((h+1)//2);self.assertEqual(job.progress(),dict(completed_tiles=total,total_tiles=total))
            session.close()

    def test_strict_nested_parameters_disabled_versions_and_types(self):
        for kind in range(3):
            session,doc,_,_=fixture(kind);variants=[]
            for change in ({'schema_version':2},{'processing_version':1},{'inputs':{'image':uid(1)}},{'masks':{'coverage':uid(1)}},
                           {'opacity':.5},{'blend_mode':'add'},{'extra':True},{'input_domain':'scene_linear_prophoto_d50'},
                           {'output_domain':'scene_linear_prophoto_d50'},{'inputs':{'mask':uid(2)}}):
                altered=copy.deepcopy(doc);altered['operations'][0].update(change);variants.append(altered)
            changes=[{'invert':1},{'invert':'false'},{'extra':1}]
            if kind==0:changes += [{'start':False},{'end':[.5,.5]},{'start':[0]},{'end':[True,0]},{'end':[2**34,0]}]
            elif kind==1:changes += [{'center':None},{'axes':[1]},{'axes':[1,0,2,0]},{'axes':[1/512,0,0,1]},
                                    {'axes':[1,False,0,1]},{'inner':1.1},{'inner':'0'}]
            else:changes += [{'rings':False},{'rings':[]},{'rings':[True]},{'rings':[[[0,0],[1,1]]]},
                              {'rings':[[[0,0],[1,1],[True,0]]]},{'rings':[[[0,0],[1,1],[2**34,0]]]}]
            for change in changes:
                altered=copy.deepcopy(doc);altered['operations'][0]['parameters'].update(change);variants.append(altered)
            missing=copy.deepcopy(doc);del missing['operations'][0]['parameters']['invert'];variants.append(missing)
            for altered in variants:
                for enabled in (True,False):
                    altered['operations'][0]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(altered))
            for format in (1,2,3):
                altered=copy.deepcopy(doc);altered['format_version']=format
                with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(altered))
            disabled=copy.deepcopy(doc);disabled['operations'][0]['enabled']=False
            self.assertEqual(session.render_coverage_manifest(json.dumps(disabled))[2],packed(words(f'frame{kind}_input')))
            with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            with self.assertRaises(ValueError):session.submit_manifest(json.dumps(doc))
            session.close()

    def test_cache_history_inversion_restore_pinned_sources_and_jobs(self):
        for kind in range(3):
            session,doc,_,_=fixture(kind);text=json.dumps(doc);expected=(7,5,packed(words(f'frame{kind}_native')))
            self.assertEqual(session.render_coverage_manifest(text),expected);before=session.cache_stats()
            self.assertEqual(session.render_coverage_manifest(text),expected);after=session.cache_stats()
            self.assertEqual(before['misses'],after['misses']);self.assertGreater(after['hits'],before['hits'])
            changed=copy.deepcopy(doc);changed['operations'][0]['parameters']['invert']=True
            inverted=session.render_coverage_manifest(json.dumps(changed));self.assertNotEqual(inverted,expected)
            history=session.history(text,max_revisions=8);second=history.commit(json.dumps(changed))
            self.assertEqual(history.compare_coverage(1,second),(expected,inverted))
            self.assertEqual(history.undo(),1);self.assertEqual(history.render_coverage(),expected)
            self.assertEqual(history.redo(),second);self.assertEqual(history.submit_coverage().result(timeout=5),inverted)
            saved=history.save();restored=session.restore_history(saved)
            self.assertEqual(restored.save(),saved);self.assertEqual(restored.render_coverage(),inverted)
            invalid=copy.deepcopy(changed);invalid['operations'][0]['parameters']['invert']='true'
            with self.assertRaises(ValueError):history.commit(json.dumps(invalid))
            self.assertEqual(history.save(),saved)
            old=session.submit_coverage_manifest(text)
            session.replace_source(uid(1),dict(coverage=array.array('f',[1])*35,width=7,height=5))
            with self.assertRaises(ValueError):session.render_coverage_manifest(text)
            with self.assertRaises(ValueError):session.restore_history(saved)
            self.assertEqual(old.result(timeout=5),expected)
            session.clear_cache();self.assertEqual(session.cache_stats()['entries'],0)
            session.close();self.assertEqual(history.render_coverage(),inverted)
            pending=history.submit_coverage();del session,history,restored;gc.collect()
            self.assertEqual(pending.result(timeout=5),inverted)

    def test_both_space_masked_rgb_upstream_mask_union_transform_and_canonical_ops(self):
        for kind in range(3):
            for space in ('prophoto-d50','rec2020-d65'):
                session,doc,_,_=fixture(kind,space)
                domain='scene_linear_prophoto_d50' if space=='prophoto-d50' else 'scene_linear_rec2020_d65'
                doc['operations'][0]['inputs']={'mask':uid(9)}
                doc['operations'].append(operation(9,'rawengine.mask_combine',{'base':uid(1),'layer':uid(8)},{'mode':'intersect'}))
                doc['operations'].append(operation(5,'rawengine.masked_mix',{'base':uid(3),'layer':uid(4)},{'amount':1},domain,{'coverage':uid(2)}))
                doc['output']=uid(5);text=json.dumps(doc)
                for mip in (0,1,2):
                    options=dict(mip=mip,quality='preview' if mip else 'final',tile_size=2)
                    mask=words(f'frame{kind}_'+('native' if mip==0 else f'mip{mip}'));expected=[]
                    for word in mask:expected.extend(mix([0xbf800000]*3,[0x40400000]*3,word,0x3ff0000000000000))
                    self.assertEqual(session.render_manifest(text,options)[2],packed(expected))
                    self.assertEqual(session.submit_manifest(text,options).result(timeout=5)[2],packed(expected))
                    self.assertEqual(set(session.required_source_regions(text,options)),{uid(1),uid(3),uid(4),uid(8)})
                doc['operations'] += [operation(6,'rawengine.box_blur',{'image':uid(5)},{'radius':1},domain),
                    operation(7,'rawengine.crop',{'image':uid(6)},{'x':1,'y':1,'width':5,'height':3},domain)]
                doc['output']=uid(7)
                for mip in (0,1):
                    options=dict(mip=mip,quality='preview' if mip else 'final',x=0,y=0,roi_width=1,roi_height=1)
                    footprint=(0,0,4,4) if mip else (0,0,3,3)
                    self.assertEqual(session.required_source_regions(json.dumps(doc),options),
                                     {key:footprint for key in (uid(1),uid(3),uid(4),uid(8))})
                session.close()
            session,doc,_,_=fixture(kind)
            canonical=re.search(r'operation%d_json\[\] = R"json\((.*?)\)json";'%kind,HEADER).group(1)
            digest=re.search(r'operation%d_sha256\[\] = "([0-9a-f]+)";'%kind,HEADER).group(1)
            self.assertEqual(hashlib.sha256(canonical.encode()).hexdigest(),digest)
            doc['operations']=[json.loads(canonical)];history=session.history(json.dumps(doc))
            self.assertIn(canonical,json.loads(history.save())['revisions'][0]['manifest'])
            session.close();history.close()


if __name__=='__main__':unittest.main()
