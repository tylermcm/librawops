"""Pre-native independent Fraction/IEEE-stage brush replay fixtures.

No engine imports, native floating arithmetic, casts, or imaging packages.
The existing IEEE encoder and coverage reduction oracle are reused unchanged.
"""
from pathlib import Path
from fractions import Fraction as Q
import argparse
import hashlib
import json
import random
from alpha_numeric_oracle import decode, encode, double, power
from coverage_source_oracle import reduced


def b64(value):
    return encode(Q(value), 53, 11)


def d64(word):
    return decode(word, 53, 11)


def point(x, y, pressure=1):
    return tuple(b64(v) for v in (x, y, pressure))


def stroke(points, mode=0, radius=2, hardness=0, flow=1, opacity=1):
    return dict(mode=mode, radius=b64(radius), hardness=b64(hardness),
                flow=b64(flow), opacity=b64(opacity), points=tuple(points))


def kernel(px, py, cx, cy, radius, hardness):
    qx, qy = double(px-cx), double(py-cy)
    distance = double(double(qx*qx) + double(qy*qy))
    r2 = double(radius*radius)
    inner2 = double(double(hardness*hardness)*r2)
    if distance >= r2:
        return Q(0)
    if distance <= inner2:
        return Q(1)
    return max(Q(0), min(Q(1), double(double(r2-distance)/double(r2-inner2))))


def segment_center(px, py, a, b):
    ax, ay, ap = map(d64, a)
    bx, by, bp = map(d64, b)
    vx, vy = double(bx-ax), double(by-ay)
    dx, dy = double(px-ax), double(py-ay)
    den = double(double(vx*vx)+double(vy*vy))
    if den == 0:
        return ax, ay, max(ap,bp)
    num = double(double(dx*vx)+double(dy*vy))
    if num <= 0:
        return ax, ay, ap
    if num >= den:
        return bx, by, bp
    t = double(num/den)
    cx, cy = double(ax+double(t*vx)), double(ay+double(t*vy))
    pressure = max(Q(0),min(Q(1),double(ap+double(t*double(bp-ap)))))
    return cx, cy, pressure


def alpha_at(px, py, settings):
    radius, hardness = d64(settings['radius']), d64(settings['hardness'])
    primitives = [tuple(map(d64,p)) for p in settings['points']]
    primitives += [segment_center(px,py,a,b)
                   for a,b in zip(settings['points'],settings['points'][1:])]
    maximum = Q(0)
    for cx,cy,pressure in primitives:
        maximum = max(maximum,double(kernel(px,py,cx,cy,radius,hardness)*pressure))
    weight = double(double(maximum*d64(settings['flow']))*d64(settings['opacity']))
    return encode(weight)


def composite(base, alpha, mode):
    if alpha == 0:
        return base
    if alpha == 0x3f800000:
        return 0x3f800000 if mode == 0 else 0
    a, w = decode(base),decode(alpha)
    if mode == 0:
        return encode(double(a+double(w*double(1-a))))
    assert mode == 1
    return encode(double(a*double(1-w)))


def replay(base, px, py, strokes):
    for settings in strokes:
        base = composite(base,alpha_at(px,py,settings),settings['mode'])
    return base


def pixel(base, x, y, strokes):
    return replay(base,Q(x)+Q(1,2),Q(y)+Q(1,2),strokes)


def make_cases():
    cases = []
    values = [0,0x80000000,1,2,0x007fffff,0x00800000,0x3dcccccd,
              0x3e800000,0x3f000000,0x3f7fffff,0x3f800000]
    # Hand-friendly centers, support circumference, softness and radius extremes.
    for mode in (0,1):
        for hardness in (0,Q(1,2),1,Q(2**53-1,2**53)):
            for x,y in [(0,0),(1,0),(1,1),(2,0),(3,0)]:
                s = stroke([point(Q(1,2),Q(1,2))],mode=mode,hardness=hardness)
                base = values[len(cases)%len(values)]
                cases.append((base,b64(Q(x)+Q(1,2)),b64(Q(y)+Q(1,2)),(s,)))
    extreme_paths = [
        stroke([point(Q(1,2),Q(1,2))],radius=power(-8)),
        stroke([point(Q(1,2),Q(1,2))],radius=power(20),hardness=Q(3,4)),
        stroke([point(-power(33),power(33)),point(power(33),-power(33))],radius=power(20)),
        stroke([(0,0,0),(1,1,b64(1))],radius=1), # den underflows to zero.
        stroke([point(Q(1,2),Q(1,2),power(-1074))]),
        stroke([point(Q(1,2),Q(1,2))],flow=power(-149)),
        stroke([point(Q(1,2),Q(1,2))],flow=power(-150)),
        stroke([point(Q(1,2),Q(1,2))],flow=power(-149),opacity=Q(1,2)),
        stroke([point(0,0),point(0,0,0),point(4,0,Q(1,2)),point(4,4),point(0,0)]),
        stroke([point(-8,2,0),point(8,2)],radius=Q(3,4),flow=Q(3,4),opacity=Q(1,2)),
    ]
    for settings in extreme_paths:
        for mode in (0,1):
            s = dict(settings,mode=mode)
            for base in values:
                cases.append((base,b64(Q(1,2)),b64(Q(1,2)),(s,)))
    rng = random.Random(0xB1252026)
    for i in range(160):
        px,py = Q(rng.randrange(-8,17),2),Q(rng.randrange(-8,17),2)
        controls = [Q(0),Q(1,8),Q(1,2),Q(7,8),Q(1),power(-1074)]
        paths = [point(Q(rng.randrange(-8,17),2),Q(rng.randrange(-8,17),2),rng.choice(controls))
                 for _ in range(1+i%4)]
        s = stroke(paths,mode=i%2,radius=rng.choice([Q(1,256),Q(3,4),Q(3,2),Q(7,2),Q(8)]),
                   hardness=rng.choice(controls[:-1]),flow=rng.choice(controls),opacity=rng.choice(controls))
        strokes = [s]
        if i%3 == 0:
            strokes.append(stroke([point(px,py,Q(1,2))],mode=1-i%2,flow=Q(1,2)))
        cases.append((rng.choice(values),b64(px),b64(py),tuple(strokes)))
    # Ordered strokes are distinct from a single stroke's maximum envelope.
    paint = stroke([point(Q(1,2),Q(1,2))],flow=Q(1,2))
    erase = dict(paint,mode=1)
    for sequence in [(),(paint,),(paint,paint),(erase,paint),(paint,erase),(paint,erase,paint)]:
        cases.append((0,b64(Q(1,2)),b64(Q(1,2)),sequence))
    return cases


def make_frames():
    frames = []
    dimensions = [(7,5),(1,9),(9,1),(3,3),(8,6),(1,1)]
    for index,(width,height) in enumerate(dimensions):
        for shifted in (False,True):
            origin = (11,17) if shifted else (0,0)
            if index == 5 and shifted:
                origin = (0xfffffffe,0xfffffffe)
            ox,oy = origin
            initial = [encode(Q(i%5,4)) for i in range(width*height)]
            if index == 3:
                initial = [1]*9
            paths = [point(ox+Q(1,2),oy+Q(1,2)),
                     point(ox+width-Q(1,2),oy+height-Q(1,2),Q(1,2))]
            strokes = [stroke(paths,radius=Q(3,2),hardness=Q(1,2),flow=Q(3,4)),
                       stroke([point(ox-1,oy+2),point(ox+width+1,oy+2)],mode=1,
                              radius=Q(5,4),flow=Q(1,2)),
                       stroke([point(ox+Q(1,2),oy+Q(1,2))],radius=Q(1,256),flow=power(-149))]
            if index == 4:
                strokes[0] = stroke([point(ox+1,oy+1),point(ox+5,oy+1),
                                     point(ox+5,oy+4),point(ox+1,oy+1)],
                                    radius=Q(7,4),hardness=0,flow=Q(1,2),opacity=Q(3,4))
            native = [pixel(initial[y*width+x],ox+x,oy+y,strokes)
                      for y in range(height) for x in range(width)]
            frames.append((origin,width,height,initial,strokes,native,
                           reduced(native,width,height,2),reduced(native,width,height,4)))
    return frames


def checks(frames):
    center = (Q(1,2),Q(1,2))
    s = stroke([point(*center)])
    assert alpha_at(*center,s) == 0x3f800000
    assert alpha_at(Q(3,2),Q(1,2),s) == encode(Q(3,4))
    assert alpha_at(Q(3,2),Q(3,2),s) == encode(Q(1,2))
    assert alpha_at(Q(5,2),Q(1,2),s) == 0
    half = dict(s,flow=b64(Q(1,2)))
    assert replay(0,*center,[half,half]) == encode(Q(3,4))
    assert replay(0,*center,[half,dict(half,mode=1)]) == encode(Q(1,4))
    assert replay(0,*center,[dict(half,mode=1),half]) == encode(Q(1,2))
    assert composite(1,0x3f000000,1) == 0 # half-min-subnormal nearest-even.
    assert composite(0x80000000,0,0) == 0x80000000
    # The round segment fills between distant dabs and keeps true round caps.
    segment = stroke([point(0,0),point(8,0)],radius=1,hardness=1)
    assert alpha_at(Q(4),Q(0),segment) == 0x3f800000
    assert alpha_at(Q(4),Q(1),segment) == 0
    assert alpha_at(Q(-1,2),Q(0),segment) == 0x3f800000
    assert alpha_at(Q(-1),Q(0),segment) == 0
    # Pressure interpolation away from endpoint dabs, with finite staged geometry.
    variable = stroke([point(0,0,0),point(8,0,1)],radius=1,hardness=1)
    assert alpha_at(Q(4),Q(0),variable) == 0x3f000000
    duplicate = dict(s,points=(s['points'][0],)*3)
    for x,y in [(Q(1,2),Q(1,2)),(Q(3,2),Q(3,2)),(Q(7,2),Q(1,2))]:
        assert alpha_at(x,y,s) == alpha_at(x,y,duplicate)
    roi_count = 0
    for origin,width,height,initial,strokes,native,mip1,mip2 in frames:
        ox,oy = origin
        for scale,expected in [(1,native),(2,mip1),(4,mip2)]:
            rw,rh = (width+scale-1)//scale,(height+scale-1)//scale
            for y in range(rh):
                for x in range(rw):
                    # Independently replay only this cell's requested native block.
                    samples = [pixel(initial[v*width+u],ox+u,oy+v,strokes)
                               for v in range(y*scale,min((y+1)*scale,height))
                               for u in range(x*scale,min((x+1)*scale,width))]
                    if scale == 1:
                        actual = samples[0]
                    else:
                        total = Q(0)
                        for sample in samples:
                            total = double(total+decode(sample))
                        actual = encode(double(total/len(samples)))
                    assert actual == expected[y*rw+x]
                    assert 0 <= decode(actual) <= 1
                    roi_count += 1
    # Native painting then reduction differs from simply painting the reduced mask.
    assert frames[0][6] != [pixel(v,x,y,frames[0][4])
                           for y in range(3) for x,v in enumerate(frames[0][6][y*4:y*4+4])]
    return roi_count


def canonical_operation():
    operation = dict(id='00000000-0000-4000-8000-000000000002',type='rawengine.mask_brush',
        schema_version=1,processing_version=2,enabled=True,input_domain='coverage',
        output_domain='coverage',parameters=dict(strokes=[dict(mode='paint',radius=2,
        hardness=0.5,flow=0.75,opacity=1,points=[[0.5,0.5,1],[4.5,2.5,0.5]])]),
        inputs=dict(mask='00000000-0000-4000-8000-000000000001'),masks={},
        blend_mode='normal',opacity=1.0)
    return json.dumps(operation,sort_keys=True,separators=(',',':'))


def generate():
    cases,frames = make_cases(),make_frames()
    roi_count = checks(frames)
    lines = ['// Generated by brush_mask_oracle.py; exact Fraction/IEEE stages.',
             '#pragma once','#include <cstdint>','#include <span>',
             'namespace brush_mask_reference {',
             'struct Point { std::uint64_t x,y,pressure; };',
             'struct Stroke { int mode; std::uint64_t radius,hardness,flow,opacity; std::span<const Point> points; };',
             'struct Case { std::uint32_t base; std::uint64_t x,y; std::span<const Stroke> strokes; std::uint32_t output; };',
             'struct Frame { std::uint32_t x,y,width,height; std::span<const Stroke> strokes; std::span<const std::uint32_t> input,native,mip1,mip2; };']
    def emit_strokes(name,settings):
        if not settings:
            return '{}'
        for i,s in enumerate(settings):
            rows = ['{'+','.join('0x%016xull'%v for v in p)+'}' for p in s['points']]
            lines.append('inline constexpr Point %s_p%d[] = {%s};'%(name,i,','.join(rows)))
        rows = []
        for i,s in enumerate(settings):
            controls = ','.join('0x%016xull'%s[k] for k in ('radius','hardness','flow','opacity'))
            rows.append('{%d,%s,%s_p%d}'%(s['mode'],controls,name,i))
        lines.append('inline constexpr Stroke %s[] = {%s};'%(name,','.join(rows)))
        return name
    case_rows = []
    for i,(base,x,y,settings) in enumerate(cases):
        name = emit_strokes('s%d'%i,settings)
        output = replay(base,d64(x),d64(y),settings)
        assert 0 <= decode(output) <= 1
        case_rows.append('{0x%08xu,0x%016xull,0x%016xull,%s,0x%08xu},'%(base,x,y,name,output))
    lines += ['inline constexpr Case cases[] = {']+case_rows+['};']
    frame_rows = []
    for i,(origin,width,height,initial,settings,native,mip1,mip2) in enumerate(frames):
        name = emit_strokes('frame%d_strokes'%i,settings)
        for suffix,words in [('input',initial),('native',native),('mip1',mip1),('mip2',mip2)]:
            lines.append('inline constexpr std::uint32_t frame%d_%s[] = {%s};'%
                         (i,suffix,','.join('0x%08xu'%v for v in words)))
        frame_rows.append('{%du,%du,%du,%du,%s,frame%d_input,frame%d_native,frame%d_mip1,frame%d_mip2},'%
                          (*origin,width,height,name,i,i,i,i))
    operation = canonical_operation()
    lines += ['inline constexpr Frame frames[] = {']+frame_rows+['};',
              'inline constexpr char operation_json[] = R"json('+operation+')json";',
              'inline constexpr char operation_sha256[] = "'+hashlib.sha256(operation.encode()).hexdigest()+'";',
              '} // namespace brush_mask_reference','']
    return '\n'.join(lines),len(cases),len(frames),roi_count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check',action='store_true')
    args = parser.parse_args()
    content,count,frames,rois = generate()
    target = Path(__file__).with_name('brush_mask_v1.hpp')
    if args.check:
        assert target.read_text(encoding='utf-8') == content,'fixture differs from independent oracle'
    else:
        with target.open('w',encoding='utf-8',newline='\n') as stream:
            stream.write(content)
    print('%d staged brush cases / %d native-mip frames / %d independent cell ROI checks verified'%
          (count,frames,rois),flush=True)


if __name__ == '__main__':
    main()
