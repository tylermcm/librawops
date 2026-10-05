"""Frozen range masks through mixed scalar/RGB graph planning and sessions."""
import array,copy,gc,hashlib,json,re,struct,sys,unittest
from pathlib import Path

sys.path.insert(0,sys.argv.pop(1));import rawengine_native as raw
sys.path.insert(0,str(Path(__file__).parent/'reference'));import range_mask_oracle as oracle
from mask_graph_numeric_oracle import mix
from alpha_numeric_oracle import encode

HEADER=(Path(__file__).parent/'reference/range_mask_v1.hpp').read_text(encoding='utf-8')
METADATA=oracle.frame_metadata()
uid=lambda n:f'a3000000-0000-0000-0000-{n:012d}'
def packed(words):return struct.pack('='+'I'*len(words),*words)
def buffer(words):
    result=array.array('f');result.frombytes(packed(words));return result
def number(word):return struct.unpack('=d',struct.pack('=Q',word))[0]
def words(name):
    match=re.search(r'inline constexpr std::uint32_t '+name+r'\[\] = \{([^}]+)\};',HEADER)
    return [int(v,16) for v in re.findall(r'0x([0-9a-f]+)u',match.group(1))]
def parameters(s):
    c=list(map(number,s['controls']))
    return dict(center=c[:3],scales=c[3:6],inner=c[6],outer=c[7],invert=s['invert']) if s['kind']==1 else dict(edges=c[:4],invert=s['invert'])
def operation(n,kind,inputs,params,domain='coverage',masks=None):
    return dict(id=uid(n),type=kind,schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,
                inputs=inputs,parameters=params,masks=masks or {},blend_mode='normal',opacity=1)
def fixture(index=0):
    f=METADATA[index];s=f['settings'];w,h=f['width'],f['height']
    # Built-in RGB raster sources are zero-origin; depth sources retain native origin.
    x,y=(f['x'],f['y']) if s['kind']==2 else (0,0)
    specs={uid(1):dict(coverage=buffer(words(f'frame{index}_input')),width=w,height=h,x=x,y=y)}
    guide=words(f'frame{index}_guide')
    specs[uid(2)]=dict(coverage=buffer(guide),width=w,height=h,x=x,y=y) if s['kind']==2 else \
        dict(rgb=buffer(guide),width=w,height=h,working_space='rec2020-d65' if s['space'] else 'prophoto-d50')
    session=raw.RasterGraphSession(specs,workers=2);doc=json.loads(session.export_manifest(uid(1)))
    op=operation(3,('rawengine.mask_luminance_range','rawengine.mask_color_range','rawengine.mask_depth_range')[s['kind']],
                 {'mask':uid(1),'depth' if s['kind']==2 else 'image':uid(2)},parameters(s))
    doc.update(operations=[op],output=uid(3))
    return session,doc,specs,(x,y,w,h)
def expected(index,mip):
    values=words(f'frame{index}_'+('native' if not mip else f'mip{mip}'))
    # CoverageImage canonicalizes source -0 before range evaluation. Direct-node
    # fixtures deliberately retain -0 to independently test endpoint copies.
    return packed([0 if v==0x80000000 else v for v in values])

class RangeMaskTests(unittest.TestCase):
    def test_frozen_frames_roi_levels_partitions_jobs_owned_sources(self):
        for i in range(30):
            session,doc,specs,(ox,oy,w,h)=fixture(i);text=json.dumps(doc)
            for spec in specs.values():
                key='coverage' if 'coverage' in spec else 'rgb';spec[key][:]=array.array('f',[99])*len(spec[key])
            for mip,quality in [(0,'final'),(0,'preview'),(1,'preview'),(2,'preview')]:
                scale=1<<mip;rw,rh=(w+scale-1)//scale,(h+scale-1)//scale;truth=(rw,rh,expected(i,mip))
                for tile in (1,2,8):self.assertEqual(session.render_coverage_manifest(text,dict(mip=mip,quality=quality,tile_size=tile)),truth)
                for y in range(rh):
                    for x in range(rw):
                        opts=dict(mip=mip,quality=quality,x=ox+x if not mip else x,y=oy+y if not mip else y,roi_width=1,roi_height=1)
                        offset=(y*rw+x)*4
                        self.assertEqual(session.render_coverage_manifest(text,opts),(1,1,truth[2][offset:offset+4]))
                        region=(ox+x*scale,oy+y*scale,min(scale,w-x*scale),min(scale,h-y*scale))
                        self.assertEqual(session.required_source_regions(text,opts),{uid(1):region,uid(2):region})
                job=session.submit_coverage_manifest_latest('range',text,dict(mip=mip,quality=quality,tile_size=2))
                self.assertEqual(job.output_kind(),'coverage');self.assertEqual(job.result(timeout=5),truth);self.assertEqual(job.result(timeout=5),truth)
            session.close()

    def test_strict_controls_ports_domains_and_disabled_guide_exclusion(self):
        for index in (0,2,4):
            session,doc,_,_=fixture(index);op=doc['operations'][0];variants=[]
            for change in ({'schema_version':2},{'processing_version':1},{'inputs':{'mask':uid(1)}},{'inputs':{'mask':uid(2),'image':uid(1)}},
                           {'masks':{'coverage':uid(1)}},{'opacity':.5},{'blend_mode':'add'},{'extra':1},
                           {'input_domain':'scene_linear_prophoto_d50'},{'output_domain':'scene_linear_prophoto_d50'},
                           {'inputs':{'mask':uid(3),'depth' if index==4 else 'image':uid(2)}}):
                d=copy.deepcopy(doc);d['operations'][0].update(change);variants.append(d)
            controls=[{'invert':1},{'invert':'false'},{'extra':1}]
            controls += [{'edges':[0,1,0,1]},{'edges':[0,0,1]},{'edges':[False,0,1,1]},{'edges':'0'},
                         {'edges':[-1,0,1,1] if index==4 else [-65537,0,1,1]}] if index!=2 else \
                        [{'center':[0,0]},{'center':[0,False,0]},{'scales':[1,0,1]},{'scales':[1,1/512,1]},
                         {'inner':2},{'outer':0},{'outer':None},{'center':[65537,0,0]}]
            for change in controls:
                d=copy.deepcopy(doc);d['operations'][0]['parameters'].update(change);variants.append(d)
            d=copy.deepcopy(doc);del d['operations'][0]['parameters']['invert'];variants.append(d)
            for d in variants:
                for enabled in (True,False):
                    d['operations'][0]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(d))
            for format in (1,2,3):
                d=copy.deepcopy(doc);d['format_version']=format
                with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(d))
            disabled=copy.deepcopy(doc);disabled['operations'][0]['enabled']=False
            source=json.loads(session.export_manifest(uid(1)))
            self.assertEqual(session.render_coverage_manifest(json.dumps(disabled)),session.render_coverage_manifest(json.dumps(source)))
            self.assertEqual(set(session.required_source_regions(json.dumps(disabled))),{uid(1)})
            with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
            if index!=4:
                encoded=operation(8,'rawengine.working_to_srgb',{'image':uid(2)}, {},
                                  'scene_linear_prophoto_d50')
                encoded['output_domain']='display_linear_srgb'
                d=copy.deepcopy(doc);d['operations'].insert(0,encoded);d['operations'][1]['inputs']['image']=uid(8)
                for enabled in (True,False):
                    d['operations'][1]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(d))
            session.close()

    def test_cache_history_replacement_pinned_sources_and_atomic_failure(self):
        for index in (0,2,4):
            session,doc,specs,_=fixture(index);text=json.dumps(doc);baseline=session.render_coverage_manifest(text)
            before=session.cache_stats();self.assertEqual(session.render_coverage_manifest(text),baseline)
            self.assertEqual(session.cache_stats()['misses'],before['misses']);self.assertGreater(session.cache_stats()['hits'],before['hits'])
            changed=copy.deepcopy(doc);changed['operations'][0]['parameters']['invert']=True
            alternate=session.render_coverage_manifest(json.dumps(changed));self.assertNotEqual(alternate,baseline)
            history=session.history(text,max_revisions=8);revision=history.commit(json.dumps(changed))
            self.assertEqual(history.compare_coverage(1,revision),(baseline,alternate));self.assertEqual(history.undo(),1)
            self.assertEqual(history.render_coverage(),baseline);self.assertEqual(history.redo(),revision)
            saved=history.save();restored=session.restore_history(saved);self.assertEqual(restored.save(),saved)
            self.assertEqual(restored.submit_coverage().result(timeout=5),alternate)
            bad=copy.deepcopy(changed);bad['operations'][0]['parameters']['invert']='true'
            with self.assertRaises(ValueError):history.commit(json.dumps(bad))
            self.assertEqual(history.save(),saved)
            pinned=session.submit_coverage_manifest(text);replacement=copy.deepcopy(specs[uid(2)])
            key='coverage' if 'coverage' in replacement else 'rgb';replacement[key][:]=array.array('f',[0])*len(replacement[key])
            session.replace_source(uid(2),replacement)
            with self.assertRaises(ValueError):session.render_coverage_manifest(text)
            with self.assertRaises(ValueError):session.restore_history(saved)
            self.assertEqual(pinned.result(timeout=5),baseline)
            fresh=json.loads(session.export_manifest(uid(1)));fresh.update(operations=doc['operations'],output=uid(3))
            misses=session.cache_stats()['misses']
            new_pixels=session.render_coverage_manifest(json.dumps(fresh))
            self.assertGreater(session.cache_stats()['misses'],misses)
            # Default ranges select guide zero at one for all three types.
            self.assertEqual(new_pixels,session.render_coverage_manifest(session.export_manifest(uid(1))))
            session.clear_cache();self.assertEqual(session.cache_stats()['entries'],0)
            session.close();self.assertEqual(history.render_coverage(),alternate)
            job=history.submit_coverage();del session,history,restored;gc.collect();self.assertEqual(job.result(timeout=5),alternate)

    def test_nested_cross_domain_planner_masked_rgb_crop_halo_and_canonical_bytes(self):
        for space,domain in [('prophoto-d50','scene_linear_prophoto_d50'),('rec2020-d65','scene_linear_rec2020_d65')]:
            w,h=7,5;depth=[encode(oracle.Q(i%9,8)) for i in range(w*h)]
            specs={uid(1):dict(coverage=array.array('f',[1])*(w*h),width=w,height=h),
                   uid(2):dict(coverage=buffer(depth),width=w,height=h),
                   uid(4):dict(rgb=array.array('f',[-.5])*(3*w*h),width=w,height=h,working_space=space),
                   uid(5):dict(rgb=array.array('f',[1.5])*(3*w*h),width=w,height=h,working_space=space)}
            session=raw.RasterGraphSession(specs,workers=2);doc=json.loads(session.export_manifest(uid(1)))
            d=operation(10,'rawengine.mask_depth_range',{'mask':uid(1),'depth':uid(2)},dict(edges=[0,.25,.75,1],invert=False))
            guide=operation(11,'rawengine.masked_mix',{'base':uid(4),'layer':uid(5)},dict(amount=.5),domain,{'coverage':uid(10)})
            l=operation(12,'rawengine.mask_luminance_range',{'mask':uid(1),'image':uid(11)},dict(edges=[0,.25,.75,1],invert=False))
            output=operation(13,'rawengine.masked_mix',{'base':uid(4),'layer':uid(5)},dict(amount=1),domain,{'coverage':uid(12)})
            doc.update(operations=[d,guide,l,output],output=uid(13));text=json.dumps(doc)
            truth=[]
            ds=oracle.scalar(2,(0,oracle.Q(1,4),oracle.Q(3,4),1));ls=oracle.scalar(0,(0,oracle.Q(1,4),oracle.Q(3,4),1),space=int(space.startswith('rec')))
            for z in depth:
                dw=oracle.evaluate(ds,(0,0,0),z)[0]
                gw=mix([0xbf000000]*3,[0x3fc00000]*3,dw,0x3fe0000000000000)
                lw=oracle.evaluate(ls,gw,0)[0];truth.extend(mix([0xbf000000]*3,[0x3fc00000]*3,lw,0x3ff0000000000000))
            self.assertEqual(session.render_manifest(text),(w,h,packed(truth)))
            roi=dict(x=2,y=1,roi_width=1,roi_height=1)
            self.assertEqual(session.required_source_regions(text,roi),{uid(n):(2,1,1,1) for n in (1,2,4,5)})
            crop=operation(14,'rawengine.crop',{'image':uid(13)},dict(x=1,y=1,width=5,height=3),domain)
            blur=operation(15,'rawengine.box_blur',{'image':uid(14)},dict(radius=1),domain)
            doc['operations'] += [crop,blur];doc['output']=uid(15)
            self.assertEqual(session.required_source_regions(json.dumps(doc),dict(x=1,y=1,roi_width=1,roi_height=1)),
                             {uid(n):(1,1,3,3) for n in (1,2,4,5)})
            session.render_manifest(json.dumps(doc),dict(tile_size=1))
            session.close()
        # Bounded alternating scalar/RGB chain checks both dependencies after DAG
        # merges. The 160-stage supports_level expansion is deferred PERF-045.
        session,doc,_,_=fixture(0);previous=uid(2);ops=[]
        for i in range(8):
            r=operation(100+2*i,'rawengine.mask_luminance_range',{'mask':uid(1),'image':previous},dict(edges=[-65536,-65536,65536,65536],invert=False))
            image=operation(101+2*i,'rawengine.masked_mix',{'base':uid(2),'layer':previous},dict(amount=1),
                            'scene_linear_prophoto_d50',{'coverage':r['id']})
            ops += [r,image];previous=image['id']
        doc.update(operations=ops,output=previous)
        self.assertEqual(session.required_source_regions(json.dumps(doc),dict(x=2,y=1,roi_width=1,roi_height=1)),
                         {uid(1):(2,1,1,1),uid(2):(2,1,1,1)})
        session.close()
        for kind,index in enumerate((0,2,4)):
            canonical=re.search(r'operation%d_json\[\] = R"json\((.*?)\)json";'%kind,HEADER).group(1)
            digest=re.search(r'operation%d_sha256\[\] = "([0-9a-f]+)";'%kind,HEADER).group(1)
            self.assertEqual(hashlib.sha256(canonical.encode()).hexdigest(),digest)
            session,doc,_,_=fixture(index);doc['operations']=[json.loads(canonical)];doc['output']=uid(3)
            history=session.history(json.dumps(doc));saved=json.loads(history.save())
            self.assertIn(canonical,saved['revisions'][0]['manifest'])
            self.assertEqual(history.render_coverage(),session.render_coverage_manifest(json.dumps(doc)))
            self.assertEqual(session.restore_history(history.save()).save(),history.save());session.close()

if __name__=='__main__':unittest.main()
