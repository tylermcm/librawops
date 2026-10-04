"""Independent Hermite/power truth and saved curves/levels integration gates."""
import array,bisect,copy,gc,json,math,struct,sys,unittest
from decimal import Decimal as D,localcontext
from fractions import Fraction as F
from functools import lru_cache
from pathlib import Path
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw
SPACES=('prophoto-d50','rec2020-d65')
CURVE='rawengine.curves_extended'
GAMMA='rawengine.levels_gamma'
IDENTITY=[[0.,0.],[1.,1.]]
CURVES=dict(master=[[0.,0.],[.5,.25],[1.,1.]],red=[[0.,0.],[.5,.75],[1.,1.]],green=[[0.,0.],[.5,.375],[1.,1.]],blue=[[0.,0.],[.5,.625],[1.,1.]],interpolation='shape_preserving_cubic')
LEVELS=dict(input_black=[-.125,-.25,0.],input_white=[1.125,1.25,1.],output_black=[-.25,0.,-.125],output_white=[1.25,1.5,1.],gamma=[.7,2.,1.3])
def values(result):
    data=array.array('f');data.frombytes(result[2]);return data
def tap(doc,output):
    doc=copy.deepcopy(doc);ops={o['id']:o for o in doc['operations']};needed=set()
    def visit(id):
        if id not in ops or id in needed:return
        needed.add(id)
        for upstream in ops[id]['inputs'].values():visit(upstream)
    visit(output);doc.update(output=output,operations=[o for o in doc['operations'] if o['id'] in needed]);return doc
def source_doc(session):
    doc=json.loads(session.export_manifest());return tap(doc,doc['sources'][0]['id'])
def add(doc,kind,p,domain=None):
    doc=copy.deepcopy(doc);domain=domain or ('scene_linear_prophoto_d50' if doc['working_space']=='linear_prophoto_d50' else 'scene_linear_rec2020_d65')
    id='98000000-0000-0000-0000-'+str(90+len(doc['operations'])).zfill(12)
    node=dict(id=id,type=kind,schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,inputs=dict(image=doc['output']),parameters=copy.deepcopy(p),masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(node);doc['output']=id;return doc
@lru_cache(maxsize=256)
def prepared(points):
    k=[(F(x),F(y)) for x,y in points];d=[(b[1]-a[1])/(b[0]-a[0]) for a,b in zip(k,k[1:])]
    m=[d[0]]+[F(0) if a*b<=0 else (1 if a>0 else -1)*min(abs(a),abs(b)) for a,b in zip(d,d[1:])]+[d[-1]]
    return k,d,m
def curve(points,x,cubic):
    k,d,m=prepared(tuple(tuple(p) for p in points))
    if all(a==b for a,b in k):return x
    left=max(0,bisect.bisect_right([a for a,b in k],x)-1);i=min(left,len(k)-2);a,b=k[i:i+2]
    if x==k[left][0]:return k[left][1]
    if not cubic or x<a[0] or x>b[0]:return k[left][1]+(x-k[left][0])*d[i]
    t=(x-a[0])/(b[0]-a[0]);h=b[0]-a[0]
    # Expanded exact Hermite polynomial, independently of native de Casteljau.
    return (2*t**3-3*t*t+1)*a[1]+(t**3-2*t*t+t)*h*m[i]+(-2*t**3+3*t*t)*b[1]+(t**3-t*t)*h*m[i+1]
def curves_reference(rgb,p):
    cubic=p['interpolation']=='shape_preserving_cubic'
    return [float(curve(p[channel],curve(p['master'],F(float(x)),cubic),cubic)) for channel,x in zip(('red','green','blue'),rgb)]
def gamma_reference(rgb,p):
    result=[]
    with localcontext() as ctx:
        ctx.prec=100
        for i,x in enumerate(rgb):
            a,b,c,d,g=[F(float(p[name][i])) for name in ('input_black','input_white','output_black','output_white','gamma')];u=(F(float(x))-a)/(b-a)
            if g==1:y=c+u*(d-c);result.append(float(y));continue
            if c==d or not u:result.append(float(c));continue
            dec=lambda f:D(f.numerator)/D(f.denominator)
            if g in (F(1,4),F(1,2)):v=dec(abs(u)**int(1/g))
            elif g==2:v=dec(abs(u)).sqrt()
            elif g==4:v=dec(abs(u)).sqrt().sqrt()
            else:v=(dec(abs(u)).ln()/dec(g)).exp()
            if u<0:v=-v
            result.append(float(dec(c)+dec(d-c)*v))
    return result
class CurvesLevelsTests(unittest.TestCase):
    def assert_map(self,session,base,kind,p,req=None,tiles=True):
        text=json.dumps(add(base,kind,p));before=session.render_manifest(json.dumps(base),req);out=session.render_manifest(text,req)
        self.assertEqual(out[:2],before[:2]);original=values(before);actual=values(out)
        for i in range(0,len(actual),3):
            expected=(curves_reference if kind==CURVE else gamma_reference)(original[i:i+3],p)
            for c in range(3):self.assertAlmostEqual(actual[i+c],expected[c],delta=1.5e-7*abs(expected[c])+2e-11)
        if tiles:
            for size in (1,3,64):self.assertEqual(session.render_manifest(text,{**(req or {}),'tile_size':size}),out)
        self.assertEqual(session.required_source_regions(text,req),session.required_source_regions(json.dumps(base),req))
        return text,out
    def test_independent_curves_creative_flat_nonuniform_max_knots_composition(self):
        data=array.array('f',[x for i in range(-32,97) for x in (i/32,i/64,1-i/48)])
        knots=[IDENTITY,[[0,0],[.125,1],[.5,-1],[1,1]],[[0,0],[1,1],[2,1],[3,2]],[[0,0],[1/65536,1],[1,1.2],[4,2]],[[i/255,math.sin(i/255*math.pi*3)] for i in range(256)]]
        for space in SPACES:
            session=raw.RasterSession(data,129,1,space)
            try:
                base=source_doc(session)
                for interpolation in ('linear','shape_preserving_cubic'):
                    for k in knots:
                        p=dict(master=k,red=CURVES['red'],green=CURVES['green'],blue=CURVES['blue'],interpolation=interpolation);self.assert_map(session,base,CURVE,p,tiles=False)
            finally:session.close()
        session=raw.RasterSession(array.array('f',[.25]*3+[.5029296875]*3),2,1,'prophoto-d50')
        try:
            p={**CURVES,'green':CURVES['red'],'blue':CURVES['red']};out=session.render_manifest(json.dumps(add(source_doc(session),CURVE,p)))
            self.assertEqual(values(out),array.array('f',[.2109375]*3+[.4401211142539978]*3))
            reverse={**p,'master':p['red'],'red':p['master'],'green':p['master'],'blue':p['master']}
            self.assertNotEqual(session.render_manifest(json.dumps(add(source_doc(session),CURVE,reverse)))[2],out[2])
        finally:session.close()
    def test_independent_gamma_full_bounds_endpoints_signed_headroom(self):
        data=array.array('f',[x for x in (-65536.,-4.,-1.,-.5,-.25,-2**-149,-0.,0.,2**-149,.125,.25,.5,.75,1.,2.,4.,65536.) for c in range(3)])
        for space in SPACES:
            session=raw.RasterSession(data,17,1,space)
            try:
                base=source_doc(session)
                for gamma in (.25,.5,1.,2.,4.,.7,1.3,3.7):
                    p=dict(input_black=[0]*3,input_white=[1]*3,output_black=[0]*3,output_white=[1]*3,gamma=[gamma]*3);self.assert_map(session,base,GAMMA,p)
                self.assert_map(session,base,GAMMA,LEVELS)
                p=dict(input_black=[0]*3,input_white=[1]*3,output_black=[-0.]*3,output_white=[0.]*3,gamma=[2]*3)
                out=values(session.render_manifest(json.dumps(add(base,GAMMA,p))));original=values(session.render_manifest(json.dumps(base)))
                self.assertEqual([math.copysign(1,x) for x in out],[1 if x==1 else -1 for x in original])
            finally:session.close()
    def test_identity_extremes_partial_and_exact_affine_legacy(self):
        data=array.array('f',[-0.,2**-149,-2**-149,3.4028234663852886e38,-3.4028234663852886e38,0.])
        for space in SPACES:
            session=raw.RasterSession(data,2,1,space)
            try:
                base=source_doc(session);original=session.render_manifest(json.dumps(base))
                for interp in ('linear','shape_preserving_cubic'):
                    k=[[i/255,i/255] for i in range(256)];p=dict(master=k,red=IDENTITY,green=IDENTITY,blue=IDENTITY,interpolation=interp)
                    self.assertEqual(session.render_manifest(json.dumps(add(base,CURVE,p))),original)
                p=dict(input_black=[0]*3,input_white=[1]*3,output_black=[0]*3,output_white=[1]*3,gamma=[1]*3)
                self.assertEqual(session.render_manifest(json.dumps(add(base,GAMMA,p))),original)
                p={**CURVES,'master':[[0,0],[1,65536]],'red':[[0,0],[1,1/65536]],'green':[[0,0],[1,1/65536]],'blue':[[0,0],[1,1/65536]],'interpolation':'linear'}
                self.assertEqual(values(session.render_manifest(json.dumps(add(base,CURVE,p))))[3:5],data[3:5])
                for kind,partial in ((CURVE,dict(master=IDENTITY,red=IDENTITY,green=[[0,0],[1,.5]],blue=IDENTITY,interpolation='linear')),
                                     (GAMMA,dict(input_black=[0]*3,input_white=[1]*3,output_black=[0]*3,output_white=[1,.5,1],gamma=[1]*3))):
                    output=session.render_manifest(json.dumps(add(base,kind,partial)))[2]
                    for i in (0,8,12,20):self.assertEqual(output[i:i+4],original[2][i:i+4])
                for kind,p in [(CURVE,{**p,'red':IDENTITY}), (GAMMA,dict(input_black=[0]*3,input_white=[1]*3,output_black=[0]*3,output_white=[1]*3,gamma=[.25]*3))]:
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(base,kind,p)))
            finally:session.close()
        session=raw.RasterSession(array.array('f',[-2.,-0.,0.,.125,.25,.5,.75,1.,2.]),3,1,'prophoto-d50')
        try:
            base=source_doc(session)
            for a,b,c,d in ((.25,.75,-1,2),(0,1,-0.,0.),(0,1,0.,-0.),(0,1,2,2)):
                p=dict(input_black=[a]*3,input_white=[b]*3,output_black=[c]*3,output_white=[d]*3,gamma=[1]*3);legacy={k:v for k,v in p.items() if k!='gamma'}
                self.assertEqual(session.render_manifest(json.dumps(add(base,GAMMA,p))),session.render_manifest(json.dumps(add(base,'rawengine.levels',legacy))))
        finally:session.close()
    def test_geometry_reduced_roi_analysis_and_order(self):
        session=raw.RasterSession(array.array('f',((i*37%257-60)/128 for i in range(11*9*3))),11,9,'prophoto-d50')
        try:
            doc=json.loads(session.export_manifest(dict(crop=(1,1,9,7),rotate=90,resize=(9,5),resize_filter='area',output_mode='srgb-preview')));base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.resize'))
            for kind,p in ((CURVE,CURVES),(GAMMA,LEVELS)):
                for mip in (0,1,2):
                    req=dict(mip=mip,quality='preview' if mip else 'final');text,out=self.assert_map(session,base,kind,p,req)
                    self.assertEqual(session.render_manifest(text,{**req,'x':1,'y':0,'roi_width':1,'roi_height':1})[2],out[2][12:24]);tiles=[];session.analyze_local_manifest(text,tiles.append,req,radius=1);self.assertTrue(tiles)
                    self.assertEqual(session.histogram_manifest(text,req)['descriptor']['primaries'],'prophoto')
                    if mip:
                        before=session.render_manifest(json.dumps(base),req);reduced=raw.RasterSession(values(before),*before[:2],'prophoto-d50')
                        try:self.assertEqual(reduced.render_manifest(json.dumps(add(source_doc(reduced),kind,p)))[2],out[2])
                        finally:reduced.close()
            for kind,p in ((CURVE,CURVES),(GAMMA,LEVELS)):
                text=json.dumps(add(source_doc(session),kind,p));native=values(session.render_manifest(text));mip=values(session.render_manifest(text,dict(mip=1,quality='preview')))
                averaged=[sum(native[(y*11+x)*3+c] for y in (0,1) for x in (0,1))/4 for c in range(3)]
                self.assertTrue(any(abs(mip[c]-averaged[c])>1e-4 for c in range(3)))
                with self.assertRaises(ValueError):session.render_manifest(text,dict(mip=1,quality='final'))
        finally:session.close()
    def test_all_parameter_cache_history_jobs_and_owner_lifetimes(self):
        for kind,p in ((CURVE,CURVES),(GAMMA,LEVELS)):
            data=array.array('f',[(i%29-10)/8 for i in range(9*7*3)]);session=raw.RasterSession(data,9,7,'rec2020-d65');base=source_doc(session);params=copy.deepcopy(p);text=json.dumps(add(base,kind,params));req=dict(tile_size=3)
            original=session.render_manifest(text,req);data[0]=999;params.clear();before=session.cache_stats();self.assertEqual(session.submit_manifest(text,req).result(timeout=5),original);self.assertEqual(session.cache_stats()['misses'],before['misses']);history=session.history(text)
            for key in p:
                q=copy.deepcopy(p)
                if kind==CURVE:
                    if key=='interpolation':q[key]='linear'
                    else:q[key][1][1]+=.07
                else:q[key][0]+=.015
                revised_text=json.dumps(add(base,kind,q));revised=session.render_manifest(revised_text,req);self.assertGreater(session.cache_stats()['misses'],before['misses']);self.assertNotEqual(revised,original);before=session.cache_stats();revision=history.commit(revised_text);self.assertEqual(history.render(req,revision=revision),revised)
            self.assertEqual(session.submit_manifest_latest('view',revised_text,req).result(timeout=5),revised);history.undo();restored=session.restore_history(history.save());expected=restored.render(req);job=restored.submit(req);del session,history;gc.collect();self.assertEqual(job.result(timeout=5),expected);restored.close()
    def test_calibrated_raw_multisource_and_replacement_history(self):
        data=array.array('H',((i*719)%50000 for i in range(99)));meta=dict(active_x=1,active_y=1,active_width=9,active_height=7,black_levels=[1000]*4,white_levels=[30000]*4)
        for algorithm in ('rawengine.bilinear','rawengine.menon_base'):
            session=raw.RawSession(data,11,9,meta,demosaic=dict(algorithm=algorithm,processing_version=1))
            try:
                for space in SPACES:
                    doc=json.loads(session.export_manifest(dict(camera_to_xyz_d50=(.6,.2,.1,.2,.6,.1,.1,.1,.6),working_space=space,output_mode='srgb-preview')));base=tap(doc,next(o['id'] for o in doc['operations'] if o['type']=='rawengine.camera_to_working'))
                    for kind,p in ((CURVE,CURVES),(GAMMA,LEVELS)):
                        for mip in (0,1,2):self.assert_map(session,base,kind,p,dict(mip=mip,quality='preview' if mip else 'final'))
                        with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(source_doc(session),kind,p,'camera_linear')))
            finally:session.close()
        a,b='98000000-0000-0000-0000-000000000001','98000000-0000-0000-0000-000000000002'
        for kind,p in ((CURVE,CURVES),(GAMMA,LEVELS)):
            session=raw.RasterGraphSession({a:dict(rgb=array.array('f',[-1,.5,2]*9),width=3,height=3,working_space='prophoto-d50'),b:dict(rgb=array.array('f',[.25,.75,1.5]*9),width=3,height=3,working_space='prophoto-d50')})
            try:
                base=json.loads(session.export_manifest(a));domain='scene_linear_prophoto_d50';mix=dict(id='98000000-0000-0000-0000-000000000080',type='rawengine.linear_mix',schema_version=1,processing_version=2,enabled=True,input_domain=domain,output_domain=domain,inputs=dict(base=a,layer=b),parameters=dict(amount=.25),masks={},blend_mode='normal',opacity=1);base.update(operations=[mix],output=mix['id'])
                for mip in (0,1,2):
                    req=dict(mip=mip,quality='preview' if mip else 'final');text,out=self.assert_map(session,base,kind,p,req);self.assertEqual(set(session.required_source_regions(text,req)),{a,b})
                history=session.history(text);session.replace_source(a,dict(rgb=array.array('f',[.125]*27),width=3,height=3,working_space='prophoto-d50'));self.assertEqual(history.render(req),out)
                with self.assertRaises(ValueError):session.render_manifest(text,req)
                history.close()
            finally:session.close()
    def test_strict_settings_domain_version_even_disabled(self):
        session=raw.RasterSession(array.array('f',[.2,.4,.6]),1,1,'prophoto-d50');base=source_doc(session)
        try:
            for kind,p in ((CURVE,CURVES),(GAMMA,LEVELS)):
                invalid=[{},dict(p,extra=0)]+[{k:v for k,v in p.items() if k!=key} for key in p]
                for key in p:
                    for v in (None,True,'0',{},[]):invalid.append({**p,key:v})
                if kind==CURVE:
                    for token in ('cubic','',0,False):invalid.append({**p,'interpolation':token})
                    for k in ([],[[0,0]],[[0,0],[0,1]],[[1,0],[0,1]],[[0,0],[2**-17,1]],[[0,0],[1,65537]],[[0,0],[1,1,1]],[[0,0]]*257):invalid.append({**p,'master':k})
                    for v in (None,True,'1',float('nan'),float('inf'),-65537):invalid.append({**p,'red':[[0,0],[1,v]]})
                else:
                    for key in p:
                        for v in ([0,0],[0,0,0,0],[True,0,0],[float('nan'),0,0],[float('inf'),0,0]):invalid.append({**p,key:v})
                    for v in (0,.249,4.001):invalid.append({**p,'gamma':[v,1,1]})
                    for v in (math.nextafter(2**-16,0),1e-8,0,-1):invalid.append(dict(input_black=[0]*3,input_white=[v]*3,output_black=[2]*3,output_white=[2]*3,gamma=[2]*3))
                    invalid.append({**p,'output_black':[2]*3});invalid.append({**p,'input_white':[65537]*3});invalid.append(dict(input_black=[0]*3,input_white=[2**-16]*3,output_black=[0]*3,output_white=[2]*3,gamma=[1]*3))
                for q in invalid:
                    for enabled in (True,False):
                        doc=add(base,kind,q);doc['operations'][-1]['enabled']=enabled
                        with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
                for field,v in (('schema_version',2),('processing_version',1),('processing_version',3)):
                    for enabled in (True,False):
                        doc=add(base,kind,p);doc['operations'][-1].update({field:v,'enabled':enabled})
                        with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
                for change in ({'input_domain':'scene_linear_rec2020_d65'},{'output_domain':'scene_linear_rec2020_d65'},{'opacity':.5},{'blend_mode':'multiply'},{'inputs':{}},{'extensions':{'test':1}}):
                    doc=add(base,kind,p);doc['operations'][-1].update(change)
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
                doc=add(base,kind,p);doc['operations'][-1]['enabled']=False;self.assertEqual(session.render_manifest(json.dumps(doc)),session.render_manifest(json.dumps(base)))
                encoded=json.loads(session.export_manifest(dict(output_mode='srgb-preview')))
                with self.assertRaises(ValueError):session.render_manifest(json.dumps(add(encoded,kind,p,'encoded_srgb')))
        finally:session.close()
    @unittest.skipUnless(raw.icc_available(),'optional ICC backend disabled')
    def test_real_icc_output_mixed_history_and_analysis(self):
        profile=raw.create_icc_profile((Path(__file__).parent/'reference/profiles/sRGB2014.icc').read_bytes())
        for space in SPACES:
            for kind,p in ((CURVE,CURVES),(GAMMA,LEVELS)):
                session=raw.RasterSession(array.array('f',[(i%23-5)/16 for i in range(5*3*3)]),5,3,space,output_profile=profile)
                def output(mode):
                    doc=json.loads(session.export_manifest(dict(output_mode=mode)));point=add(source_doc(session),kind,p)['operations'][-1];source=doc['sources'][0]['id']
                    for op in doc['operations']:
                        for key,target in op['inputs'].items():
                            if target==source:op['inputs'][key]=point['id']
                    doc['operations'].insert(0,point);return json.dumps(doc)
                try:
                    linear=session.render_manifest(json.dumps(add(source_doc(session),kind,p)))
                    icc,srgb=output('icc-display'),output('srgb-preview');actual=session.render_manifest(icc)
                    expected=raw.render_raster(values(linear),5,3,dict(working_space=space,output_mode='icc-display',output_profile=profile))
                    self.assertEqual(actual,expected)
                    for tile in (1,3,64):self.assertEqual(session.render_manifest(icc,dict(tile_size=tile)),actual)
                    self.assertEqual(session.submit_manifest(icc).result(timeout=5),actual)
                    self.assertEqual(session.histogram_manifest(icc)['descriptor']['profile_sha256'],raw.icc_profile_info(profile)['profile_sha256'])
                    pieces=[];session.analyze_local_manifest(icc,pieces.append);self.assertTrue(pieces)
                    history=session.history(srgb);first=history.stats()['current_id'];second=history.commit(icc);self.assertEqual(history.compare(first,second),(session.render_manifest(srgb),actual))
                    restored=session.restore_history(history.save());self.assertEqual(restored.render(),actual);self.assertEqual(restored.render(revision=first),session.render_manifest(srgb));restored.close();history.close()
                    with self.assertRaises(ValueError):session.render_manifest(icc,dict(mip=1,quality='preview'))
                finally:session.close()
if __name__=='__main__':unittest.main()
