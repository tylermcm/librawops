"""Frozen coverage refinement replay through saved Python graph sessions."""
import array,copy,gc,json,re,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,sys.argv.pop(1));import rawengine_native as raw
sys.path.insert(0,str(Path(__file__).parent/'reference'));import mask_refinement_oracle as oracle
HEADER=(Path(__file__).parent/'reference/mask_refinement_v1.hpp').read_text(encoding='utf-8')
METADATA=oracle.metadata()
uid=lambda n:'a4000000-0000-0000-0000-%012d'%n
def packed(words):return struct.pack('='+'I'*len(words),*words)
def buffer(words):
    result=array.array('f');result.frombytes(packed(words));return result
def number(bits):return struct.unpack('=d',struct.pack('=Q',bits))[0]
def words(name):
    match=re.search(r'inline constexpr std::uint32_t '+name+r'\[\] = \{([^}]+)\};',HEADER)
    return [int(v,16) for v in re.findall(r'0x([0-9a-f]+)u',match.group(1))]
def expected(i,mip):return packed([0 if v==0x80000000 else v for v in words('frame%d_%s'%(i,'native' if not mip else 'mip%d'%mip))])
def operation(n,typ,inputs,params,domain='coverage',masks=None):
    return dict(id=uid(n),type=typ,schema_version=1,processing_version=2,enabled=True,input_domain=domain,
                output_domain=domain,inputs=inputs,parameters=params,masks=masks or {},blend_mode='normal',opacity=1)
def fixture(i):
    f=METADATA[i];w,h=f['width'],f['height'];kind=f['kind'];x,y=(0,0) if kind==2 else (f['x'],f['y'])
    specs={uid(1):dict(coverage=buffer(words('frame%d_input'%i)),width=w,height=h,x=x,y=y)}
    ports={'mask':uid(1)};params={'density':number(f['control'])} if not kind else {'radius':f['radius']}
    if kind==2:
        specs[uid(2)]=dict(rgb=buffer(words('frame%d_guide'%i)),width=w,height=h,working_space='rec2020-d65' if f['space'] else 'prophoto-d50')
        ports['image']=uid(2);params['epsilon']=number(f['control'])
    session=raw.RasterGraphSession(specs,workers=2);doc=json.loads(session.export_manifest(uid(1)))
    op=operation(3,('rawengine.mask_density','rawengine.mask_feather_binomial','rawengine.mask_refine_working_y')[kind],ports,params)
    doc.update(operations=[op],output=uid(3));return session,doc,specs,(x,y,w,h)

class MaskRefinementTests(unittest.TestCase):
    def test_frozen_frames_levels_tiles_roi_footprints_jobs_owned_buffers(self):
        for i,f in enumerate(METADATA):
            session,doc,specs,(ox,oy,w,h)=fixture(i);text=json.dumps(doc)
            for spec in specs.values():
                key='coverage' if 'coverage' in spec else 'rgb';spec[key][:]=array.array('f',[99])*len(spec[key])
            for mip,quality in ((0,'final'),(0,'preview'),(1,'preview'),(2,'preview')):
                scale=1<<mip;rw,rh=(w+scale-1)//scale,(h+scale-1)//scale;truth=(rw,rh,expected(i,mip))
                for tile in (1,2,8):self.assertEqual(session.render_coverage_manifest(text,dict(mip=mip,quality=quality,tile_size=tile)),truth)
                for y in range(rh):
                    for x in range(rw):
                        opts=dict(mip=mip,quality=quality,x=ox+x if not mip else x,y=oy+y if not mip else y,roi_width=1,roi_height=1)
                        offset=(y*rw+x)*4
                        self.assertEqual(session.render_coverage_manifest(text,opts),(1,1,truth[2][offset:offset+4]))
                        rx,ry,ww,hh=oracle.input_region(f,mip,x,y);region=(rx-f['x']+ox,ry-f['y']+oy,ww,hh)
                        sources={uid(1):region}
                        if f['kind']==2:sources[uid(2)]=region
                        self.assertEqual(session.required_source_regions(text,opts),sources)
                job=session.submit_coverage_manifest_latest('refinement',text,dict(mip=mip,quality=quality,tile_size=2))
                self.assertEqual(job.output_kind(),'coverage');self.assertEqual(job.result(timeout=5),truth)
            session.close()

    def test_strict_disabled_records_and_actual_valid_predecessor_guide_guard(self):
        for index in (69,76,82):
            session,doc,_,bounds=fixture(index);kind=METADATA[index]['kind'];op=doc['operations'][0]
            variants=[]
            for change in ({'schema_version':2},{'processing_version':1},{'opacity':.5},{'blend_mode':'add'},
                           {'extra':1},{'masks':{'coverage':uid(1)}},{'inputs':{}},
                           {'inputs':dict(op['inputs'],extra=uid(1))},{'input_domain':'scene_linear_prophoto_d50'},
                           {'output_domain':'scene_linear_prophoto_d50'}):
                d=copy.deepcopy(doc);d['operations'][0].update(change);variants.append(d)
            variants.append(dict(doc,operations=[dict(op,parameters={})]))
            for key in op['parameters']:
                bad_values=(True,None,'1',[],{},-1,1.5,33) if key=='radius' else (True,None,'1',[],{},-1,65537)
                if key=='density':bad_values+=(1.01,)
                if key=='epsilon':bad_values+=(0,2**-25,)
                for value in bad_values:
                    d=copy.deepcopy(doc);d['operations'][0]['parameters'][key]=value;variants.append(d)
            d=copy.deepcopy(doc);d['operations'][0]['parameters']['extra']=1;variants.append(d)
            if kind==2:
                d=copy.deepcopy(doc);d['operations'][0]['inputs']['image']=uid(1);variants.append(d)
                d=copy.deepcopy(doc);d['operations'][0]['inputs']['mask']=uid(2);variants.append(d)
                d=copy.deepcopy(doc);d['operations'][0]['inputs']={'mask':uid(1)};variants.append(d)
                # First prove the precursor is a valid executable RGB graph.
                domain='scene_linear_rec2020_d65' if METADATA[index]['space'] else 'scene_linear_prophoto_d50'
                precursor=operation(4,'rawengine.working_to_srgb',{'image':uid(2)},{},domain)
                precursor['output_domain']='scene_linear_srgb'
                guide_doc=dict(doc,operations=[precursor],output=uid(4))
                session.render_manifest(json.dumps(guide_doc))
                d=copy.deepcopy(doc);d['operations'].insert(0,precursor);d['operations'][1]['inputs']['image']=uid(4)
                for enabled in (False,True):
                    d['operations'][1]['enabled']=enabled
                    with self.assertRaisesRegex(ValueError,'mask refinement guide requires scene-linear working RGB'):
                        session.render_coverage_manifest(json.dumps(d))
            for d in variants:
                for enabled in (False,True):
                    d['operations'][0]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(d))
            disabled=copy.deepcopy(doc);disabled['operations'][0]['enabled']=False
            opts=dict(x=bounds[0]+1,y=bounds[1]+1,roi_width=1,roi_height=1)
            self.assertEqual(session.required_source_regions(json.dumps(disabled),opts),{uid(1):(opts['x'],opts['y'],1,1)})
            self.assertEqual(session.render_coverage_manifest(json.dumps(disabled)),session.render_coverage_manifest(session.export_manifest(uid(1))))
            session.close()

    def test_cache_history_source_replacement_pinned_jobs_and_lifetime(self):
        for index in (69,76,82):
            session,doc,specs,bounds=fixture(index);text=json.dumps(doc);truth=(bounds[2],bounds[3],expected(index,0))
            self.assertEqual(session.render_coverage_manifest(text),truth);warm=session.cache_stats()
            self.assertEqual(session.render_coverage_manifest(text),truth);self.assertEqual(session.cache_stats()['misses'],warm['misses'])
            history=session.history(text);original_source=session.render_coverage_manifest(session.export_manifest(uid(1)))
            disabled=copy.deepcopy(doc);disabled['operations'][0]['enabled']=False
            revision=history.commit(json.dumps(disabled));self.assertEqual(history.render_coverage(),original_source)
            self.assertEqual(history.undo(),1);self.assertEqual(history.render_coverage(),truth)
            self.assertEqual(history.redo(),revision);self.assertEqual(history.render_coverage(),original_source)
            restored=session.restore_history(history.save());self.assertEqual(restored.save(),history.save())
            bad=copy.deepcopy(doc);bad['operations'][0]['parameters']={};saved=history.save()
            with self.assertRaises(ValueError):history.commit(json.dumps(bad))
            self.assertEqual(history.save(),saved)
            pinned=session.submit_coverage_manifest(text,dict(tile_size=2))
            if METADATA[index]['kind']==2:
                guide_spec=copy.deepcopy(specs[uid(2)]);guide_spec['rgb']=array.array('f',[0])*(3*bounds[2]*bounds[3])
                session.replace_source(uid(2),guide_spec)
                with self.assertRaises(ValueError):session.render_coverage_manifest(text)
                guided_fresh=json.loads(session.export_manifest(uid(1)));guided_fresh.update(operations=doc['operations'],output=uid(3))
                f=METADATA[index];source=[0 if v==0x80000000 else v for v in words('frame%d_input'%index)]
                guided_expected=oracle.refine(source,[0]*(3*bounds[2]*bounds[3]),bounds[2],bounds[3],f['radius'],oracle.d64(f['control']),f['space'])
                misses=session.cache_stats()['misses']
                self.assertEqual(session.render_coverage_manifest(json.dumps(guided_fresh)),(bounds[2],bounds[3],packed(guided_expected)))
                self.assertGreater(session.cache_stats()['misses'],misses)
            spec=copy.deepcopy(specs[uid(1)]);spec['coverage']=array.array('f',[0])*(bounds[2]*bounds[3])
            session.replace_source(uid(1),spec)
            with self.assertRaises(ValueError):session.render_coverage_manifest(text)
            fresh=json.loads(session.export_manifest(uid(1)));fresh.update(operations=doc['operations'],output=uid(3))
            misses=session.cache_stats()['misses'];result=session.render_coverage_manifest(json.dumps(fresh))
            self.assertGreater(session.cache_stats()['misses'],misses)
            value=1 if METADATA[index]['kind']==0 and number(METADATA[index]['control'])==0 else 0
            self.assertEqual(result,(bounds[2],bounds[3],array.array('f',[value]*(bounds[2]*bounds[3])).tobytes()))
            self.assertEqual(pinned.result(timeout=5),truth)
            session.clear_cache();self.assertEqual(session.cache_stats()['entries'],0)
            session.close();self.assertEqual(history.render_coverage(),original_source)
            job=history.submit_coverage();del session,history,restored;gc.collect();self.assertEqual(job.result(timeout=5),original_source)

    def test_mixed_four_source_composed_halos_and_canonical_operations(self):
        for space,domain in (('prophoto-d50','scene_linear_prophoto_d50'),('rec2020-d65','scene_linear_rec2020_d65')):
            w,h=7,5
            specs={uid(1):dict(coverage=array.array('f',[1])*(w*h),width=w,height=h),
                   uid(2):dict(coverage=array.array('f',[.5])*(w*h),width=w,height=h),
                   uid(4):dict(rgb=array.array('f',[-.5])*(3*w*h),width=w,height=h,working_space=space),
                   uid(5):dict(rgb=array.array('f',[1.5])*(3*w*h),width=w,height=h,working_space=space)}
            session=raw.RasterGraphSession(specs,workers=2);doc=json.loads(session.export_manifest(uid(1)))
            depth=operation(10,'rawengine.mask_depth_range',{'mask':uid(1),'depth':uid(2)},dict(edges=[0,.25,.75,1],invert=False))
            mixed=operation(11,'rawengine.masked_mix',{'base':uid(4),'layer':uid(5)},dict(amount=.5),domain,{'coverage':uid(10)})
            blur=operation(12,'rawengine.box_blur',{'image':uid(11)},dict(radius=1),domain)
            refine=operation(13,'rawengine.mask_refine_working_y',{'mask':uid(1),'image':uid(12)},dict(radius=1,epsilon=2**-12))
            output=operation(14,'rawengine.masked_mix',{'base':uid(4),'layer':uid(5)},dict(amount=1),domain,{'coverage':uid(13)})
            doc.update(operations=[depth,mixed,blur,refine,output],output=uid(14));text=json.dumps(doc)
            expected_rgb=array.array('f',[1.5]*(3*w*h)).tobytes()
            self.assertEqual(session.render_manifest(text,dict(tile_size=1)),(w,h,expected_rgb))
            # Refine r1 needs mask +/-2; blurred mixed guide adds one further halo,
            # and its depth mask reuses source1, merging its larger footprint.
            opts=dict(x=3,y=2,roi_width=1,roi_height=1)
            self.assertEqual(session.required_source_regions(text,opts),{uid(n):(0,0,7,5) for n in (1,2,4,5)})
            session.close()
        for kind,index in enumerate((0,5,11)):
            canonical=re.search(r'operation%d_json\[\] = R"json\((.*?)\)json";'%kind,HEADER).group(1)
            session,doc,_,_=fixture(index);doc['operations']=[json.loads(canonical)];doc['output']=uid(3)
            history=session.history(json.dumps(doc));saved=json.loads(history.save())
            self.assertIn(canonical,saved['revisions'][0]['manifest'])
            self.assertEqual(session.restore_history(history.save()).save(),history.save());session.close()

if __name__=='__main__':unittest.main()
