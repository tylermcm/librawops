"""Independent staged Fraction oracle for linear/radial/polygon mask geometry."""
from fractions import Fraction as Q
from pathlib import Path
import argparse,hashlib,json,random
from alpha_numeric_oracle import decode,encode,double,power
from coverage_source_oracle import reduced


def b64(value):return encode(Q(value),53,11)
def d64(word):return decode(word,53,11)
def point(x,y):return (b64(x),b64(y))
def linear(a,b,invert=False):return dict(kind=0,geometry=(*a,*b,0,0,0),rings=(),invert=invert)
def radial(center,axes,inner=0,invert=False):
    return dict(kind=1,geometry=(*center,*(b64(v) for v in axes),b64(inner)),rings=(),invert=invert)
def polygon(rings,invert=False):
    return dict(kind=2,geometry=(0,)*7,rings=tuple(tuple(point(*p) for p in ring) for ring in rings),invert=invert)


def validate(shape):
    geometry=list(map(d64,shape['geometry']))
    if shape['kind']==0:
        sx,sy,ex,ey=geometry[:4];vx,vy=double(ex-sx),double(ey-sy)
        den=double(double(vx*vx)+double(vy*vy))
        if den<power(-16):raise ValueError('short/degenerate ramp')
    elif shape['kind']==1:
        cx,cy,ux,uy,vx,vy,inner=geometry
        determinant=double(double(ux*vy)-double(vx*uy))
        if not determinant:raise ValueError('singular ellipse')
        # Values beyond 257 cannot round into the admitted <=256 coefficient;
        # reject before the finite IEEE encoder could overflow on tiny det.
        inverse=[v/determinant for v in (vy,-vx,-uy,ux)]
        if any(abs(v)>257 or abs(double(v))>256 for v in inverse):
            raise ValueError('inverse amplification')
        if not 0<=inner<=1:raise ValueError('inner out of range')
    else:
        assert shape['kind']==2
        if not 1<=len(shape['rings'])<=4096 or any(not 3<=len(r)<=65536 for r in shape['rings']):
            raise ValueError('invalid polygon ring counts')
        if sum(map(len,shape['rings']))>1048576:raise ValueError('polygon point count')
    coordinates=geometry[:4] if shape['kind']==0 else geometry[:6] if shape['kind']==1 else [d64(v) for r in shape['rings'] for p in r for v in p]
    if any(abs(v)>power(33) for v in coordinates):raise ValueError('coordinate range')


def shape_word(shape,px,py):
    g=list(map(d64,shape['geometry']))
    if shape['kind']==0:
        ax,ay,bx,by=g[:4]
        vx,vy=double(bx-ax),double(by-ay)
        den=double(double(vx*vx)+double(vy*vy))
        dx,dy=double(px-ax),double(py-ay)
        num=double(double(dx*vx)+double(dy*vy))
        value=Q(0) if num<=0 else Q(1) if num>=den else double(num/den)
    elif shape['kind']==1:
        cx,cy,ux,uy,vx,vy,inner=g
        det=double(double(ux*vy)-double(vx*uy))
        a,b,c,d=[double(v/det) for v in (vy,-vx,-uy,ux)]
        dx,dy=double(px-cx),double(py-cy)
        u=double(double(a*dx)+double(b*dy));v=double(double(c*dx)+double(d*dy))
        r2=double(double(u*u)+double(v*v));inner2=double(inner*inner)
        value=Q(0) if r2>=1 else Q(1) if r2<=inner2 else max(Q(0),min(Q(1),double(double(1-r2)/double(1-inner2))))
    else:
        parity=False;boundary=False
        for ring in shape['rings']:
            for a,b in zip(ring,ring[1:]+ring[:1]):
                ax,ay=map(d64,a);bx,by=map(d64,b)
                dx,dy=double(px-ax),double(py-ay);vx,vy=double(bx-ax),double(by-ay)
                cross=double(double(dx*vy)-double(dy*vx))
                if cross==0 and min(ax,bx)<=px<=max(ax,bx) and min(ay,by)<=py<=max(ay,by):
                    boundary=True;break
                if (ay>py)!=(by>py):
                    if py==ay:intersection=ax
                    elif py==by:intersection=bx
                    else:
                        t=double(double(py-ay)/double(by-ay))
                        intersection=double(ax+double(t*double(bx-ax)))
                    if px<intersection:parity=not parity
            if boundary:break
        value=Q(int(boundary or parity))
    word=encode(value)
    return encode(double(1-decode(word))) if shape['invert'] else word


def multiply(base,shape):
    if shape==0:return 0
    if shape==0x3f800000:return base
    return encode(double(decode(base)*decode(shape)))


def prototypes():
    return [linear(point(0,0),point(4,0)),linear(point(4,0),point(0,0)),
        linear(point(-2,-3),point(5,7)),linear(point(0,0),point(power(-8),0)),
        linear(point(-power(33),-power(33)),point(power(33),power(33))),
        radial(point(0,0),(2,0,0,1)),radial(point(0,0),(2,0,1,1),Q(1,2)),
        radial(point(0,0),(-2,0,0,1),1),radial(point(0,0),(0,2,-1,0)),
        radial(point(Q(1,2),Q(1,2)),(power(-8),0,0,power(-8))),
        radial(point(power(33),-power(33)),(power(33),0,0,power(33)),Q(2**53-1,2**53)),
        polygon([[(0,0),(4,0),(4,4),(0,4)]]),
        polygon([[(0,0),(8,0),(8,8),(0,8)],[(2,2),(6,2),(6,6),(2,6)]]),
        polygon([[(0,0),(4,0),(4,1),(1,1),(1,4),(0,4)]]),
        polygon([[(0,0),(4,4),(0,4),(4,0)]]),
        polygon([[(0,0),(0,0),(4,0),(4,4),(0,4)]]),
        polygon([[(0,0),(2,2),(4,4)]]),
        polygon([[(Q(-1,3),Q(-1,7)),(Q(8,3),Q(1,7)),(Q(5,3),Q(9,7))]])]


def make_cases():
    coverage=[0,0x80000000,1,2,0x007fffff,0x00800000,0x3dcccccd,0x3e800000,0x3f000000,0x3f7fffff,0x3f800000]
    positions=[(-1,-1),(0,0),(Q(1,2),Q(1,2)),(1,0),(1,1),(2,0),(2,2),(4,0),(4,4),(6,6),(8,8),(Q(1,10),Q(3,10))]
    cases=[];shapes=prototypes()
    for settings in shapes:
        validate(settings)
        for invert in (False,True):
            shape=dict(settings,invert=invert)
            for x,y in positions:
                cases.append((shape,coverage[len(cases)%len(coverage)],b64(x),b64(y)))
            for base in coverage:
                cases.append((shape,base,b64(Q(1,2)),b64(Q(1,2))))
    rng=random.Random(0xCA542026)
    for i in range(160):
        settings=dict(shapes[i%len(shapes)],invert=bool(i%2))
        cases.append((settings,rng.choice(coverage),b64(Q(rng.randrange(-32,65),7)),b64(Q(rng.randrange(-32,65),11))))
    return cases


def frame_metadata():
    rows=[]
    for width,height in [(7,5),(1,9),(9,1),(3,3),(8,6),(1,1)]:
        for shifted in (False,True):
            ox,oy=(11,17) if shifted else (0,0)
            if width==height==1 and shifted:ox=oy=0xfffffffe
            shapes=[linear(point(ox+Q(1,2),oy+Q(1,2)),point(ox+width+Q(1,2),oy+height+Q(1,2))),
                    radial(point(ox+Q(width,2),oy+Q(height,2)),(Q(width,2),0,Q(width,4),Q(height,2)),Q(1,4)),
                    polygon([[(ox-Q(1,2),oy-Q(1,2)),(ox+width-Q(1,2),oy-Q(1,2)),
                              (ox+width-Q(1,2),oy+height-Q(1,2)),(ox-Q(1,2),oy+height-Q(1,2))],
                             [(ox+Q(width,4),oy+Q(height,4)),(ox+Q(3*width,4),oy+Q(height,4)),
                              (ox+Q(3*width,4),oy+Q(3*height,4)),(ox+Q(width,4),oy+Q(3*height,4))]])]
            for kind,shape in enumerate(shapes):
                shape=dict(shape,invert=shifted and kind!=1)
                initial=[encode(Q(i%5,4)) for i in range(width*height)]
                if width==height==3:initial=[1]*9
                rows.append(((ox,oy),width,height,initial,shape))
    return rows


def make_frames():
    rows=[]
    for origin,width,height,initial,shape in frame_metadata():
        ox,oy=origin;validate(shape)
        native=[multiply(initial[y*width+x],shape_word(shape,Q(ox+x)+Q(1,2),Q(oy+y)+Q(1,2)))
                for y in range(height) for x in range(width)]
        rows.append((origin,width,height,initial,shape,native,reduced(native,width,height,2),reduced(native,width,height,4)))
    return rows


def checks(frames):
    ramp=linear(point(0,0),point(4,0))
    assert shape_word(ramp,Q(2),Q(3))==0x3f000000
    assert shape_word(ramp,Q(-1),Q(99))==0
    assert shape_word(ramp,Q(5),Q(-99))==0x3f800000
    ellipse=radial(point(0,0),(2,0,0,1))
    assert shape_word(ellipse,Q(0),Q(0))==0x3f800000
    assert shape_word(ellipse,Q(1),Q(0))==encode(Q(3,4))
    assert shape_word(ellipse,Q(2),Q(0))==0
    assert shape_word(radial(point(0,0),(2,0,1,1)),Q(3,2),Q(1,2))==0x3f000000
    hole=prototypes()[12]
    assert shape_word(hole,Q(1),Q(1))==0x3f800000
    assert shape_word(hole,Q(4),Q(4))==0
    assert shape_word(hole,Q(2),Q(4))==0x3f800000
    concave=prototypes()[13]
    assert shape_word(concave,Q(2),Q(2))==0
    assert shape_word(concave,Q(1,2),Q(2))==0x3f800000
    reversed_hole=dict(hole,rings=tuple(tuple(reversed(r)) for r in hole['rings']))
    for x,y in [(1,1),(4,4),(2,4),(8,8),(9,9)]:
        assert shape_word(hole,Q(x),Q(y))==shape_word(reversed_hole,Q(x),Q(y))
    assert multiply(0x80000000,0x3f800000)==0x80000000
    assert multiply(1,0x3f000000)==0
    for shape in [linear(point(0,0),point(0,0)),linear(point(0,0),point(power(-9),0)),
                  radial(point(0,0),(1,0,2,0)),radial(point(0,0),(power(-9),0,0,1))]:
        try:validate(shape)
        except ValueError:pass
        else:raise AssertionError('invalid geometry admitted')
    count=0
    for origin,width,height,initial,shape,native,mip1,mip2 in frames:
        ox,oy=origin
        for scale,expected in [(1,native),(2,mip1),(4,mip2)]:
            rw,rh=(width+scale-1)//scale,(height+scale-1)//scale
            for y in range(rh):
                for x in range(rw):
                    total=Q(0);samples=[]
                    for v in range(y*scale,min((y+1)*scale,height)):
                        for u in range(x*scale,min((x+1)*scale,width)):
                            samples.append(multiply(initial[v*width+u],shape_word(shape,Q(ox+u)+Q(1,2),Q(oy+v)+Q(1,2))))
                    for value in samples:total=double(total+decode(value))
                    actual=samples[0] if scale==1 else encode(double(total/len(samples)))
                    assert actual==expected[y*rw+x] and 0<=decode(actual)<=1
                    count+=1
    return count


def operations():
    params=[dict(start=[0,0],end=[4,0],invert=False),
            dict(center=[0,0],axes=[2,0,1,1],inner=0.5,invert=False),
            dict(rings=[[[0,0],[4,0],[4,4],[0,4]]],invert=False)]
    names=['rawengine.mask_linear_gradient','rawengine.mask_radial_gradient','rawengine.mask_polygon']
    return [json.dumps(dict(blend_mode='normal',enabled=True,id='00000000-0000-4000-8000-000000000002',
        input_domain='coverage',output_domain='coverage',inputs=dict(mask='00000000-0000-4000-8000-000000000001'),
        masks={},opacity=1.0,parameters=p,processing_version=2,schema_version=1,type=name),sort_keys=True,separators=(',',':'))
        for p,name in zip(params,names)]


def generate():
    cases,frames=make_cases(),make_frames();roi_count=checks(frames)
    lines=['// Generated by parametric_mask_oracle.py; exact Fraction/IEEE stages.',
           '#pragma once','#include <array>','#include <cstdint>','#include <span>',
           'namespace parametric_mask_reference {','struct Point { std::uint64_t x,y; };',
           'using Ring = std::span<const Point>;',
           'struct Settings { int kind; std::array<std::uint64_t,7> geometry; bool invert; std::span<const Ring> rings; };',
           'struct Case { Settings settings; std::uint32_t base; std::uint64_t x,y; std::uint32_t shape,output; };',
           'struct Frame { std::uint32_t x,y,width,height; Settings settings; std::span<const std::uint32_t> input,native,mip1,mip2; };']
    settings_names={}
    def settings(shape):
        key=(shape['kind'],shape['geometry'],shape['invert'],shape['rings'])
        if key in settings_names:return settings_names[key]
        name='s%d'%len(settings_names);settings_names[key]=name
        rings='{}'
        if shape['rings']:
            for i,ring in enumerate(shape['rings']):
                rows=['{0x%016xull,0x%016xull}'%p for p in ring]
                lines.append('inline constexpr Point %s_ring%d[] = {%s};'%(name,i,','.join(rows)))
            rings=name+'_rings'
            lines.append('inline constexpr Ring %s[] = {%s};'%(rings,','.join(name+'_ring%d'%i for i in range(len(shape['rings'])))))
        geometry='{'+','.join('0x%016xull'%word for word in shape['geometry'])+'}'
        lines.append('inline constexpr Settings %s = {%d,%s,%s,%s};'%(name,shape['kind'],geometry,str(shape['invert']).lower(),rings))
        return name
    case_rows=[]
    for shape,base,x,y in cases:
        name=settings(shape);word=shape_word(shape,d64(x),d64(y));output=multiply(base,word)
        assert 0<=decode(word)<=1 and 0<=decode(output)<=1
        case_rows.append('{%s,0x%08xu,0x%016xull,0x%016xull,0x%08xu,0x%08xu},'%(name,base,x,y,word,output))
    frame_rows=[]
    for i,(origin,width,height,initial,shape,native,mip1,mip2) in enumerate(frames):
        name=settings(shape)
        for suffix,words in [('input',initial),('native',native),('mip1',mip1),('mip2',mip2)]:
            lines.append('inline constexpr std::uint32_t frame%d_%s[] = {%s};'%(i,suffix,','.join('0x%08xu'%v for v in words)))
        frame_rows.append('{%du,%du,%du,%du,%s,frame%d_input,frame%d_native,frame%d_mip1,frame%d_mip2},'%(*origin,width,height,name,i,i,i,i))
    lines+=['inline constexpr Case cases[] = {']+case_rows+['};','inline constexpr Frame frames[] = {']+frame_rows+['};']
    for i,operation in enumerate(operations()):
        lines.append('inline constexpr char operation%d_json[] = R"json(%s)json";'%(i,operation))
        lines.append('inline constexpr char operation%d_sha256[] = "%s";'%(i,hashlib.sha256(operation.encode()).hexdigest()))
    lines+=['}','']
    return '\n'.join(lines),len(cases),len(frames),roi_count


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
    content,count,frames,rois=generate();target=Path(__file__).with_name('parametric_mask_v1.hpp')
    if args.check:assert target.read_text(encoding='utf-8')==content,'fixture differs from independent oracle'
    else:
        with target.open('w',encoding='utf-8',newline='\n') as stream:stream.write(content)
    print('%d staged parametric cases / %d native-mip frames / %d independent cell ROIs verified'%(count,frames,rois),flush=True)


if __name__=='__main__':main()
