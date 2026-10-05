"""Independent Fraction/IEEE-stage candidate oracle; no engine or native FP math.

Scalar cases retain every guide bit, stored selection, input product and luminance
stage. Frames retain native products and direct clipped preview reductions.
"""
from fractions import Fraction as Q
from pathlib import Path
import argparse, hashlib, json, random
from alpha_numeric_oracle import decode, encode, double, power
from coverage_source_oracle import reduced


def b64(v): return encode(Q(v),53,11)
def d64(v): return decode(v,53,11)
def f32(v): return encode(Q(v))

WEIGHTS = tuple(tuple(b64(v) for v in pair) for pair in (
    ('0.28807112822929337','0.00008565396060525903'),
    ('0.26270021201126703','0.059301716469861945')))


def scalar(kind, edges=(0,0,1,1), space=0, invert=False):
    return dict(kind=kind,space=space,controls=tuple(b64(v) for v in edges)+(0,)*4,invert=invert)


def color(center=(0,0,0), scales=(1,1,1), inner=0, outer=1, space=0, invert=False):
    return dict(kind=1,space=space,controls=tuple(b64(v) for v in (*center,*scales,inner,outer)),invert=invert)


def validate(s):
    if type(s['invert']) is not bool or s['kind'] not in (0,1,2) or s['space'] not in (0,1):
        raise ValueError('kind/space/invert')
    try: c=list(map(d64,s['controls']))
    except AssertionError: raise ValueError('nonfinite controls')
    if len(c)!=8: raise ValueError('control length')
    if s['kind']!=1:
        a,b,c0,d=c[:4];low=Q(0) if s['kind']==2 else Q(-65536)
        high=Q(1) if s['kind']==2 else Q(65536)
        if not low<=a<=b<=c0<=d<=high: raise ValueError('scalar edges')
    elif (any(abs(v)>65536 for v in c[:3]) or
          any(not power(-8)<=v<=65536 for v in c[3:6]) or
          not power(-8)<=c[7]<=65536 or not 0<=c[6]<=c[7]):
        raise ValueError('ellipsoid controls')


def scalar_value(t,edges):
    a,b,c,d=edges
    if b<=t<=c: return Q(1)
    if t<=a or t>=d: return Q(0)
    value=double(double(t-a)/double(b-a)) if t<b else double(double(d-t)/double(d-c))
    return max(Q(0),min(Q(1),value))


def evaluate(s,rgb,depth):
    validate(s);c=list(map(d64,s['controls']));y=Q(0)
    try:
        if s['kind']==2:
            t=decode(depth)
            if not 0<=t<=1: raise ValueError('depth outside normalized domain')
            value=scalar_value(t,c[:4])
        else:
            r,g,b=map(decode,rgb)
            if s['kind']==0:
                wr,wb=map(d64,WEIGHTS[s['space']])
                dr,db=double(r-g),double(b-g)
                pr,pb=double(wr*dr),double(wb*db)
                y=double(double(g+pr)+pb)
                value=scalar_value(y,c[:4])
            else:
                sq=[]
                for channel,center,scale in zip((r,g,b),c[:3],c[3:6]):
                    q=double(double(channel-center)/scale);sq.append(double(q*q))
                distance2=double(double(sq[0]+sq[1])+sq[2])
                inner2,outer2=double(c[6]*c[6]),double(c[7]*c[7])
                value=Q(1) if distance2<=inner2 else Q(0) if distance2>=outer2 else \
                    max(Q(0),min(Q(1),double(double(outer2-distance2)/double(outer2-inner2))))
    except AssertionError: raise ValueError('nonfinite guide')
    selection=encode(value)
    if s['invert']: selection=encode(double(1-decode(selection)))
    return selection,b64(y)


def multiply(base,selection):
    a=decode(base)
    if not 0<=a<=1: raise ValueError('invalid mask')
    if not decode(selection): return 0
    if decode(selection)==1: return base
    return encode(double(a*decode(selection)))


def prototypes():
    result=[]
    for space in (0,1):
        result += [scalar(0,space=space),scalar(0,(-2,-1,2,4),space),
                   scalar(0,(Q(1,4),Q(1,2),Q(1,2),Q(3,4)),space),
                   scalar(0,(Q(1,2),)*4,space),scalar(0,(-65536,-65536,65536,65536),space),
                   color(space=space),color((Q(1,2),Q(1,4),-1),(Q(1,2),2,4),Q(1,4),2,space),
                   color((1,1,1),(1,2,4),1,1,space),
                   color((65536,-65536,0),(power(-8),65536,1),0,power(-8),space)]
    result += [scalar(2),scalar(2,(Q(1,8),Q(1,4),Q(3,4),Q(7,8))),
               scalar(2,(Q(1,2),)*4),scalar(2,(0,0,0,0)),scalar(2,(1,1,1,1))]
    return result


def cases():
    rng=random.Random(0x20261004);rows=[]
    bases=[0,0x80000000,1,2,0x007fffff,0x00800000,0x33800000,0x3e800000,0x3f000000,0x3f7fffff,0x3f800000]
    neutrals=[-65536,-2,-1,0,Q(1,8),Q(1,4),Q(1,2),Q(3,4),1,2,4,65536]
    rgbs=[(f32(v),)*3 for v in neutrals]+[(0x7f7fffff,0xff7fffff,1),
         (0x80000000,0,0x80000001),(0x3f800000,0,0),(0,0x3f800000,0),(0,0,0x3f800000)]
    depths=[0,1,2,0x007fffff,0x00800000]+[f32(v) for v in (Q(1,8),Q(1,4),Q(1,2),Q(3,4),Q(7,8),1)]
    for p in prototypes():
        for invert in (False,True):
            s=dict(p,invert=invert)
            for i in range(22):
                rgb=rgbs[i%len(rgbs)];depth=depths[i%len(depths)]
                for base in bases: rows.append((s,rgb,depth,base))
            for _ in range(24):
                rgb=tuple(rng.randrange(0x7f800000)|(rng.randrange(2)<<31) for _ in range(3))
                depth=rng.randrange(0x3f800001);base=rng.choice(bases)
                rows.append((s,rgb,depth,base))
    return rows


def frame_metadata():
    frames=[];ps=prototypes();rgb_cycle=[(f32(v),)*3 for v in (-2,0,Q(1,4),Q(1,2),Q(3,4),1,2)]+[
        tuple(f32(v) for v in rgb) for rgb in ((1,0,0),(0,1,0),(0,0,1),(-1,2,Q(1,4)))]
    depth_cycle=[f32(Q(i,8)) for i in range(9)];base_cycle=[0,0x80000000,1,2,0x00800000,0x3e800000,0x3f000000,0x3f800000]
    for i,(x,y,w,h) in enumerate(((0,0,7,5),(11,17,1,9),(23,31,9,1),
                                  (0xfffffff0,0xfffffff1,3,3),(2,3,8,6),(5,7,1,1))):
        for kind,space in ((0,0),(0,1),(1,0),(1,1),(2,0)):
            options=[p for p in ps if p['kind']==kind and p['space']==space]
            s=dict(options[i%len(options)],invert=bool(i%2))
            base=[base_cycle[(n+i)%len(base_cycle)] for n in range(w*h)]
            guide=[depth_cycle[(n+i)%len(depth_cycle)] for n in range(w*h)] if kind==2 else \
                [v for n in range(w*h) for v in rgb_cycle[(n+i)%len(rgb_cycle)]]
            frames.append(dict(x=x,y=y,width=w,height=h,settings=s,input=base,guide=guide))
    return frames


def make_frames():
    frames=frame_metadata();cells=0
    for f in frames:
        s=f['settings'];native=[]
        for n,base in enumerate(f['input']):
            rgb=f['guide'][3*n:3*n+3] if s['kind']!=2 else (0,0,0)
            depth=f['guide'][n] if s['kind']==2 else 0
            native.append(multiply(base,evaluate(s,rgb,depth)[0]))
        f['native']=native
        for mip in (1,2):
            scale=1<<mip;f['mip%d'%mip]=reduced(native,f['width'],f['height'],scale)
            rw=(f['width']+scale-1)//scale;rh=(f['height']+scale-1)//scale
            for y in range(rh):
                for x in range(rw):
                    # Independent explicit cell traversal; check actual clipped count/order.
                    acc=Q(0);count=0
                    for yy in range(y*scale,min((y+1)*scale,f['height'])):
                        for xx in range(x*scale,min((x+1)*scale,f['width'])):
                            acc=double(acc+decode(native[yy*f['width']+xx]));count+=1
                    assert encode(double(acc/count))==f['mip%d'%mip][y*rw+x];cells+=1
        cells+=len(native)
    return frames,cells


def operations():
    result=[]
    for kind,s in enumerate((scalar(0),color(),scalar(2))):
        params=dict(invert=False)
        if kind==1: params.update(center=[0,0,0],scales=[1,1,1],inner=0,outer=1)
        else: params['edges']=[0,0,1,1]
        ports=dict(mask='a3000000-0000-0000-0000-000000000001')
        ports['depth' if kind==2 else 'image']='a3000000-0000-0000-0000-000000000002'
        op=dict(id='a3000000-0000-0000-0000-000000000003',type=('rawengine.mask_luminance_range',
                'rawengine.mask_color_range','rawengine.mask_depth_range')[kind],schema_version=1,
                processing_version=2,enabled=True,input_domain='coverage',output_domain='coverage',
                parameters=params,inputs=ports,masks={},blend_mode='normal',opacity=1.0)
        result.append(json.dumps(op,sort_keys=True,separators=(',',':')))
    return result


def checks():
    # Hand-derived exact plateau/soft/singleton anchors.
    edges=list(map(Q,(0,Q(1,4),Q(3,4),1)))
    assert [scalar_value(Q(n,8),edges) for n in range(9)]==list(map(Q,(0,Q(1,2),1,1,1,1,1,Q(1,2),0)))
    assert scalar_value(Q(1,2),[Q(1,2)]*4)==1
    assert scalar_value(Q(1,2)+power(-53),[Q(1,2)]*4)==0
    for space in (0,1):
        for value in (-65536,-1,0,Q(1,2),1,65536):
            _,y=evaluate(scalar(0,space=space),(f32(value),)*3,0)
            assert y==b64(value)
        assert evaluate(color(space=space),(0,0,0),0)[0]==f32(1)
        assert evaluate(color(space=space),(f32(Q(1,2)),0,0),0)[0]==f32(Q(3,4))
        assert evaluate(color(space=space),(f32(1),0,0),0)[0]==0
        hard=color(inner=1,outer=1,space=space)
        assert evaluate(hard,(f32(1),0,0),0)[0]==f32(1)
        # Axis sign symmetry is exact for zero-centered representable anchors.
        assert evaluate(color(space=space),(f32(-Q(1,2)),0,0),0)[0]==f32(Q(3,4))
    assert multiply(0x80000000,f32(1))==0x80000000 and multiply(0x80000000,0)==0
    assert multiply(1,f32(Q(1,2)))==0 and multiply(3,f32(Q(1,2)))==2
    # Native mapping before guide averaging is observable for each selection type.
    witnesses=[]
    for s,rgb0,rgb1,depth0,depth1 in (
        (scalar(0,(0,Q(1,4),Q(3,4),1)),(0,)*3,(f32(1),)*3,0,0),
        (color(),(f32(-1),0,0),(f32(1),0,0),0,0),
        (scalar(2,(0,Q(1,4),Q(3,4),1)),(0,)*3,(0,)*3,0,f32(1))):
        native=reduced([evaluate(s,rgb0,depth0)[0],evaluate(s,rgb1,depth1)[0]],2,1,2)[0]
        averaged=tuple(encode(double(double(decode(a)+decode(b))/2)) for a,b in zip(rgb0,rgb1))
        depth=encode(double(double(decode(depth0)+decode(depth1))/2))
        wrong=evaluate(s,averaged,depth)[0];assert native!=wrong;witnesses.append((native,wrong))
    invalid=[]
    for kind in (0,2):
        for edges0 in ((0,1,0,1),(-1,0,1,1) if kind==2 else (-65537,0,1,1),(0,0,1,2) if kind==2 else (0,0,1,65537)):
            invalid.append(scalar(kind,edges0))
    for kwargs in (dict(scales=(0,1,1)),dict(scales=(power(-9),1,1)),dict(center=(65537,0,0)),
                   dict(inner=-1),dict(inner=2,outer=1),dict(outer=power(-9)),dict(outer=65537)):
        invalid.append(color(**kwargs))
    invalid.extend((dict(scalar(0),invert=1),dict(scalar(0),space=2),dict(scalar(0),controls=(0x7ff0000000000000,)+(0,)*7)))
    for s in invalid:
        try: validate(s)
        except ValueError: pass
        else: raise AssertionError('invalid settings accepted')
    for s,rgb,depth in ((scalar(0),(0x7fc00000,0,0),0),(color(),(0,0x7f800000,0),0),
                        (scalar(2),(0,0,0),0xbf800000),(scalar(2),(0,0,0),0x40000000),
                        (scalar(2),(0,0,0),0x7fc00000)):
        try: evaluate(s,rgb,depth)
        except ValueError: pass
        else: raise AssertionError('invalid guide accepted')
    return len(invalid)+5,witnesses


def generate():
    guards,witnesses=checks();rows=cases();frames,cells=make_frames()
    lines=['// Generated by range_mask_oracle.py; independent exact Fraction IEEE stages.',
           '#pragma once','#include <array>','#include <cstdint>','#include <span>',
           'namespace range_mask_reference {',
           'struct Settings { int kind,space; std::array<std::uint64_t,8> controls; bool invert; };',
           'struct Case { Settings settings; std::array<std::uint32_t,3> rgb; std::uint32_t depth,base,selection,output; std::uint64_t luminance; };',
           'struct Frame { std::uint32_t x,y,width,height; Settings settings; std::span<const std::uint32_t> input,guide,native,mip1,mip2; };']
    words=lambda values:'{'+','.join('0x%08xu'%v for v in values)+'}'
    setting=lambda s:'{%d,%d,{%s},%s}'%(s['kind'],s['space'],','.join('0x%016xull'%v for v in s['controls']),str(s['invert']).lower())
    lines.append('inline constexpr Case cases[] = {')
    for s,rgb,depth,base in rows:
        selection,y=evaluate(s,rgb,depth);output=multiply(base,selection)
        assert 0<=decode(selection)<=1 and 0<=decode(output)<=1
        lines.append('{%s,%s,0x%08xu,0x%08xu,0x%08xu,0x%08xu,0x%016xull},'%(setting(s),words(rgb),depth,base,selection,output,y))
    lines.append('};');frame_rows=[]
    for i,f in enumerate(frames):
        for suffix in ('input','guide','native','mip1','mip2'):
            lines.append('inline constexpr std::uint32_t frame%d_%s[] = %s;'%(i,suffix,words(f[suffix])))
        frame_rows.append('{%du,%du,%du,%du,%s,frame%d_input,frame%d_guide,frame%d_native,frame%d_mip1,frame%d_mip2},'%
                          (f['x'],f['y'],f['width'],f['height'],setting(f['settings']),i,i,i,i,i))
    lines+=['inline constexpr Frame frames[] = {']+frame_rows+['};']
    for i,op in enumerate(operations()):
        lines+=['inline constexpr char operation%d_json[] = R"json(%s)json";'%(i,op),
                'inline constexpr char operation%d_sha256[] = "%s";'%(i,hashlib.sha256(op.encode()).hexdigest())]
    lines+=['}','']
    return '\n'.join(lines),dict(cases=len(rows),frames=len(frames),cell_rois=cells,guard_rejections=guards,
                                native_before_mip_witnesses=witnesses,weight_words=WEIGHTS)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
    content,counts=generate();target=Path(__file__).with_name('range_mask_v1.hpp')
    if args.check: assert target.read_text(encoding='utf-8')==content,'fixture differs from independent oracle'
    else:
        with target.open('w',encoding='utf-8',newline='\n') as stream: stream.write(content)
    print(json.dumps(counts,sort_keys=True),flush=True)


if __name__=='__main__': main()
