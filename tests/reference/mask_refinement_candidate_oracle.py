"""Candidate exploration: exact Fraction IEEE stages, no engine/native FP math.

This is not a frozen fixture or implementation acceptance test. It exercises the
proposed three algorithms before their contract is frozen.
"""
from fractions import Fraction as Q
from math import comb
from pathlib import Path
import hashlib
import json
from alpha_numeric_oracle import decode, encode, double, power
from coverage_source_oracle import reduced
from range_mask_oracle import WEIGHTS, d64


def word(v): return encode(Q(v))


def coverage(words):
    result = [decode(v) for v in words]
    if any(not 0 <= v <= 1 for v in result): raise ValueError('coverage')
    return result


def density(words, value):
    p = coverage(words)
    if not 0 <= value <= 1: raise ValueError('density')
    if value == 1: return list(words)
    if value == 0: return [word(1)] * len(words)
    return [word(max(Q(0), min(Q(1), double(1-double(value*double(1-a)))))) for a in p]


def window(x, y, r, w, h):
    return [(xx, yy) for yy in range(max(0,y-r),min(h,y+r+1))
            for xx in range(max(0,x-r),min(w,x+r+1))]


def feather(words, w, h, r):
    p = coverage(words)
    if len(p) != w*h or w < 1 or h < 1 or type(r) is not int or not 0 <= r <= 32:
        raise ValueError('feather settings/extent')
    if r == 0: return list(words)
    weights = [double(Q(comb(2*r,t))) for t in range(2*r+1)]

    def weighted(samples):
        if all(v == samples[0][0] for v,_ in samples): return samples[0][0]
        numerator = denominator = Q(0)
        for v,wt in samples:
            numerator = double(numerator+double(wt*v))
            denominator = double(denominator+wt)
        return double(numerator/denominator)

    horizontal = [weighted([(p[y*w+xx],weights[xx-x+r])
                             for xx in range(max(0,x-r),min(w,x+r+1))])
                  for y in range(h) for x in range(w)]
    return [word(max(Q(0), min(Q(1), weighted([
        (horizontal[yy*w+x],weights[yy-y+r])
        for yy in range(max(0,y-r),min(h,y+r+1))]))))
        for y in range(h) for x in range(w)]


def difference(rgb, j, k, space):
    wr,wb = map(d64,WEIGHTS[space])
    dr,dg,db = [double(rgb[j][c]-rgb[k][c]) for c in range(3)]
    return double(double(dg+double(wr*double(dr-dg)))+double(wb*double(db-dg)))


def refine(words, guides, w, h, r, epsilon, space):
    p = coverage(words)
    if (len(p) != w*h or len(guides) != 3*w*h or w < 1 or h < 1 or
        type(r) is not int or not 0 <= r <= 8 or
        not power(-24) <= epsilon <= 65536 or space not in (0,1)):
        raise ValueError('refinement settings/extent')
    g = [tuple(decode(v) for v in guides[i:i+3]) for i in range(0,len(guides),3)]
    if r == 0: return list(words)
    coefficients = []
    for y in range(h):
        for x in range(w):
            k = y*w+x
            js = [yy*w+xx for xx,yy in window(x,y,r,w,h)]
            sg = sp = Q(0)
            for j in js:
                sg = double(sg+difference(g,j,k,space))
                sp = double(sp+double(p[j]-p[k]))
            mg,mp = double(sg/len(js)),double(sp/len(js))
            variance = covariance = Q(0)
            for j in js:
                dy = double(difference(g,j,k,space)-mg)
                dp = double(double(p[j]-p[k])-mp)
                variance = double(variance+double(dy*dy))
                covariance = double(covariance+double(dy*dp))
            variance,covariance = double(variance/len(js)),double(covariance/len(js))
            a = double(covariance/double(variance+epsilon))
            coefficients.append((mg,mp,a))
    result = []
    for y in range(h):
        for x in range(w):
            i = y*w+x
            ks = [yy*w+xx for xx,yy in window(x,y,r,w,h)]
            correction = Q(0)
            for k in ks:
                mg,mp,a = coefficients[k]
                base = double(double(p[k]-p[i])+mp)
                value = base if a == 0 else double(base+double(a*double(difference(g,i,k,space)-mg)))
                correction = double(correction+value)
            correction = double(correction/len(ks))
            result.append(words[i] if correction == 0 else
                          word(max(Q(0),min(Q(1),double(p[i]+correction)))))
    return result


def checks():
    anchors = 0
    inputs = [0,0x80000000,1,3,word(Q(1,4)),word(Q(1,2)),word(1)]
    assert density(inputs,Q(1)) == inputs
    assert density(inputs,Q(0)) == [word(1)]*len(inputs)
    assert density([word(0),word(Q(1,2)),word(1)],Q(1,2)) == [word(Q(1,2)),word(Q(3,4)),word(1)]
    anchors += 3
    previous = density(inputs,Q(0))
    for d in (Q(1,8),Q(1,2),Q(7,8),Q(1)):
        current = density(inputs,d)
        assert all(decode(a) >= decode(b) for a,b in zip(previous,current))
        previous = current; anchors += 1
    for r in (0,1,2,8,32):
        for v in (0,1,3,word(Q(1,2)),word(1)):
            p = [v]*15
            assert feather(p,5,3,r) == p
            anchors += 1
    # Hand-derived [1,2,1] kernel: true border count/weight is 3, not 4.
    assert feather([0,word(1),0],3,1,1) == [word(Q(1,3)),word(Q(1,2)),word(Q(1,3))]
    anchors += 1
    # Constant guide => two independently clipped box averages.
    p = [0,word(1),0]
    expected = [word(Q(5,12)),word(Q(4,9)),word(Q(5,12))]
    for space in (0,1):
        assert refine(p,[0]*9,3,1,1,power(-12),space) == expected
        for r in (0,1,8):
            for value in (0,1,word(Q(1,2)),word(1)):
                guide = [0x7f7fffff,0xff7fffff,1]*3
                assert refine([value]*3,guide,3,1,r,power(-24),space) == [value]*3
                anchors += 1
        anchors += 1
    # Feather requires the complete halo: cropping the one-cell request changes it.
    assert feather([0,word(1),0],3,1,1)[1] != feather([word(1)],1,1,1)[0]
    anchors += 1
    # Native feather then reduction differs from reduction then feather.
    p = [word(1),0,0,0,0]
    before = reduced(feather(p,5,1,1),5,1,2)
    after = feather(reduced(p,5,1,2),3,1,1)
    assert before != after
    guided = refine(p,[0]*15,5,1,1,power(-12),0)
    assert decode(guided[2]) > 0
    assert refine(p[1:4],[0]*9,3,1,1,power(-12),0)[1] == 0
    anchors += 1
    guided_before = reduced(guided,5,1,2)
    guided_after = refine(reduced(p,5,1,2),[0]*9,3,1,1,power(-12),0)
    assert guided_before != guided_after
    # Extreme varying signed/headroom guide; every ordered stage remains finite.
    for space in (0,1):
        guide = [0x7f7fffff,0xff7fffff,1, 0xff7fffff,0x7f7fffff,0x80000001,
                 0x7f7fffff,0x7f7fffff,0xff7fffff]
        for epsilon in (power(-24),Q(65536)):
            out = refine([0,word(1),1],guide,3,1,1,epsilon,space)
            assert all(0 <= decode(v) <= 1 for v in out); anchors += 1
    guards = 0
    calls = []
    for bad in (0xbf800000,0x3f800001,0x7f800000,0x7fc00000):
        calls += [lambda bad=bad: density([bad],Q(0)),
                  lambda bad=bad: density([bad],Q(1)),
                  lambda bad=bad: feather([bad],1,1,0),
                  lambda bad=bad: refine([bad],[0]*3,1,1,0,power(-12),0)]
    for bad in (0x7f800000,0x7fc00000):
        calls.append(lambda bad=bad: refine([0],[bad,0,0],1,1,0,power(-12),0))
    for r in (-1,33,True,Q(1,2)):
        calls.append(lambda r=r: feather([0],1,1,r))
    for r in (-1,9,True,Q(1,2)):
        calls.append(lambda r=r: refine([0],[0]*3,1,1,r,power(-12),0))
    for call in calls:
        try: call()
        except (ValueError,AssertionError): guards += 1
        else: raise AssertionError('invalid candidate input admitted')
    return dict(hand_derived_checks=anchors,candidate_guard_rejections=guards,
                native_before_mip_witness=[before,after],
                guided_native_before_mip_witness=[guided_before,guided_after])


def main():
    report = checks()
    frames = []
    for w,h in ((1,1),(5,1),(1,5),(5,3)):
        p = [word(Q((i*3)%9,8)) for i in range(w*h)]
        guides = [word(v) for i in range(w*h) for v in (Q(i%3)-1,Q(i%5,4),Q(i%7,2))]
        operations = [('density',density(p,Q(1,2)))]
        operations += [('feather%d'%r,feather(p,w,h,r)) for r in (0,1,2,32)]
        operations += [('refine%d_space%d'%(r,s),refine(p,guides,w,h,r,power(-12),s))
                       for s in (0,1) for r in (0,1,8)]
        for name,out in operations:
            assert all(0 <= decode(v) <= 1 for v in out)
            frames.append(dict(width=w,height=h,operation=name,input=p,guide=guides,
                               native=out,mip1=reduced(out,w,h,2),mip2=reduced(out,w,h,4)))
    report.update(status='candidate exploration, not frozen or native acceptance',frames=len(frames))
    root = Path(__file__).resolve().parents[2]
    target = root/'build-msvc-release/research/mask-refinement-candidate-v1'
    target.mkdir(parents=True,exist_ok=True)
    fixture = json.dumps(frames,sort_keys=True,indent=2)+'\n'
    for name,content in (('frames.json',fixture),('report.json',json.dumps(dict(report,
        fixture_sha256=hashlib.sha256(fixture.encode()).hexdigest()),sort_keys=True,indent=2)+'\n')):
        with (target/name).open('w',encoding='utf-8',newline='\n') as stream: stream.write(content)
    print(json.dumps(report,sort_keys=True),flush=True)


if __name__ == '__main__': main()
