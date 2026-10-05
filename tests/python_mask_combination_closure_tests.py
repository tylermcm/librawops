"""Optional original-row closure proof for all saved mask combination modes."""
import array,copy,json,re,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,sys.argv.pop(1));import rawengine_native as raw
sys.path.insert(0,str(Path(__file__).parent/'reference'))
from mask_graph_numeric_oracle import algebra
from coverage_source_oracle import reduced

def uid(n):return f'a4000000-0000-0000-0000-{n:012d}'
def packed(values):return struct.pack('='+'I'*len(values),*values)
def buffer(values):
    result=array.array('f');result.frombytes(packed(values));return result
HEADER=(Path(__file__).parent/'reference/mask_graph_numeric_v1.hpp').read_text(encoding='utf-8')
ROWS=[(int(mode),int(a,16),int(b,16),int(out,16)) for mode,a,b,out in
      re.findall(r'^\{([0-3]),0x([0-9a-f]+)u,0x([0-9a-f]+)u,0x([0-9a-f]+)u\},$',HEADER,re.M)]
assert len(ROWS)==324
AS=[a for mode,a,b,out in ROWS if mode==0];BS=[b for mode,a,b,out in ROWS if mode==0]
def operation(mode):
    return dict(id=uid(3),type='rawengine.mask_invert' if mode==0 else 'rawengine.mask_combine',
                schema_version=1,processing_version=2,enabled=True,input_domain='coverage',output_domain='coverage',
                inputs={'mask':uid(1)} if mode==0 else {'base':uid(1),'layer':uid(2)},masks={},
                parameters={} if mode==0 else {'mode':('add','subtract','intersect')[mode-1]},blend_mode='normal',opacity=1)

class MaskCombinationClosureTests(unittest.TestCase):
    def test_all_frozen_modes_saved_levels_rois_cache_history_jobs_and_disabled(self):
        for space in ('linear_prophoto_d50','linear_rec2020_d65'):
            session=raw.RasterGraphSession({uid(1):dict(coverage=buffer(AS),width=9,height=9),
                                           uid(2):dict(coverage=buffer(BS),width=9,height=9)},workers=2)
            source=json.loads(session.export_manifest(uid(1)));source['working_space']=space
            for mode in range(4):
                doc=copy.deepcopy(source);doc.update(operations=[operation(mode)],output=uid(3));text=json.dumps(doc)
                for mip,quality in ((0,'final'),(0,'preview'),(1,'preview'),(2,'preview')):
                    scale=1<<mip;w=(9+scale-1)//scale
                    if mip==0:truth=[out for m,a,b,out in ROWS if m==mode]
                    else:
                        a,b=reduced(AS,9,9,scale),reduced(BS,9,9,scale)
                        truth=[algebra(mode,x,y) for x,y in zip(a,b)]
                    expected=(w,w,packed(truth));options=dict(mip=mip,quality=quality)
                    for tile in (1,2,8):self.assertEqual(session.render_coverage_manifest(text,{**options,'tile_size':tile}),expected)
                    self.assertEqual(session.render_coverage_manifest(text,options),expected)
                    before=session.cache_stats();self.assertEqual(session.render_coverage_manifest(text,options),expected)
                    self.assertEqual(session.cache_stats()['misses'],before['misses'])
                    job=session.submit_coverage_manifest(text,options);self.assertEqual(job.result(timeout=5),expected)
                    for y in range(w):
                        for x in range(w):
                            roi={**options,'x':x,'y':y,'roi_width':1,'roi_height':1}
                            self.assertEqual(session.render_coverage_manifest(text,roi),(1,1,packed([truth[y*w+x]])))
                            native=(x*scale,y*scale,min(scale,9-x*scale),min(scale,9-y*scale))
                            self.assertEqual(session.required_source_regions(text,roi),{uid(n):native for n in ((1,) if mode==0 else (1,2))})
                history=session.history(text,max_revisions=4);disabled=copy.deepcopy(doc);disabled['operations'][0]['enabled']=False
                revision=history.commit(json.dumps(disabled));saved=history.save()
                self.assertEqual(history.render_coverage(),(9,9,packed(AS)))
                self.assertEqual(session.required_source_regions(json.dumps(disabled)),{uid(1):(0,0,9,9)})
                self.assertEqual(history.undo(),1)
                self.assertEqual(history.render_coverage(),(9,9,packed([out for m,a,b,out in ROWS if m==mode])))
                self.assertEqual(history.redo(),revision);self.assertEqual(session.restore_history(saved).save(),saved)
                if mode:
                    for invalid in (None,False,1,[],{},'max'):
                        bad=copy.deepcopy(doc);bad['operations'][0]['parameters']['mode']=invalid
                        for enabled in (True,False):
                            bad['operations'][0]['enabled']=enabled
                            with self.assertRaises(ValueError):session.render_coverage_manifest(json.dumps(bad))
                history.close()
            session.close()

if __name__=='__main__':unittest.main()
