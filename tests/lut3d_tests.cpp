#include "ToneOps.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace rawengine;
namespace {
void require(bool yes,const char* why) { if (!yes) throw std::runtime_error(why); }
template<class F> void rejects(F f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected 3D LUT rejection");
}
std::shared_ptr<const Node> source(std::vector<float> p,unsigned w,unsigned h=1,
                                 WorkingSpace s=WorkingSpace::LinearProPhotoD50) {
    return std::make_shared<RasterSourceNode>(RasterImage({w,h,0,s},std::move(p)));
}
template<class F> Lut3DSettings table(unsigned n,F f,
        std::array<double,3> lo={-1,0,-2},std::array<double,3> hi={1,2,2}) {
    Lut3DSettings s; s.size=n;s.input_min=lo;s.input_max=hi;s.values.clear();
    std::array<std::vector<double>,3> axes;
    for(unsigned c=0;c<3;++c)for(unsigned i=0;i<n;++i)
        axes[c].push_back(i==0 ? lo[c]:i==n-1 ? hi[c]:lo[c]+(hi[c]-lo[c])*(double(i)/double(n-1)));
    for(double b:axes[2])for(double g:axes[1])for(double r:axes[0]) {
        auto rgb=f(r,g,b);s.values.insert(s.values.end(),rgb.begin(),rgb.end());
    }
    return s;
}
std::array<double,3> cross(double r,double g,double b) {
    return {r+.25*g-.125*b+.125*r*g,-.5*r+g+.25*b+.0625*g*b,
            .25*r-.125*g+b+.125*r*b+.03125*r*g*b};
}
void independent_polynomial_truth() {
    std::vector<float> pixels;
    for(float r:{-3.f,-1.f,-.5f,0.f,.5f,1.f,3.f})
        for(float g:{-2.f,0.f,.5f,1.f,1.5f,2.f,4.f})
            for(float b:{-4.f,-2.f,-1.f,0.f,1.f,2.f,4.f})pixels.insert(pixels.end(),{r,g,b});
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        auto s=table(3,cross);Lut3DNode lut(source(pixels,343,1,space),s);s.values[0]=9;
        const auto out=lut.render({0,0,343,1});
        for(std::size_t i=0;i<pixels.size();i+=3) {
            const auto expected=cross(pixels[i],pixels[i+1],pixels[i+2]);
            for(unsigned c=0;c<3;++c)require(out.rgb[i+c]==float(expected[c]),"cross-term polynomial/layout/extrapolation truth");
        }
        require(Renderer{}.render_image(lut,{0,0,343,1},RenderRequest{{0,0,343,1},7}).rgb==out.rgb,"tile parity");
        require(out.descriptor==ImageDescriptor::scene_linear(space),"descriptor changed");
    }
}
void identity_and_endpoint_bits() {
    const std::vector<float> p{-0.f,0.f,-0.f,std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),std::numeric_limits<float>::denorm_min()};
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        auto input=source(p,2,1,space);
        for(unsigned n:{2u,3u,7u,17u}) {
            auto s=table(n,[](double r,double g,double b){return std::array<double,3>{r,g,b};});
            const auto out=Lut3DNode(input,s).render({0,0,2,1});
            require(std::memcmp(out.rgb.data(),p.data(),p.size()*sizeof(float))==0,"identity bits");
        }
        auto tiny=table(2,[](double r,double g,double b){return std::array<double,3>{r,g,b};},{0,0,0},{1e-320,1e-320,1e-320});
        const auto out=Lut3DNode(input,tiny).render({0,0,2,1});
        require(std::memcmp(out.rgb.data(),p.data(),p.size()*sizeof(float))==0,"identity must bypass fractions");
    }
    auto partial=table(2,[](double r,double,double b){return std::array<double,3>{r,.25,b};});
    auto out=Lut3DNode(source(p,2),partial).render({0,0,2,1});
    require(std::signbit(out.rgb[0]) && out.rgb[3]==p[3] && out.rgb[5]==p[5] && out.rgb[1]==.25f,"partial bits");
    Lut3DSettings near;near.values[21]=1+std::ldexp(1.,-22);
    require(Lut3DNode(source({1,1,1},1),near).render({0,0,1,1}).rgb[0]>1,"approximate identity bypass");
    Lut3DSettings zero;zero.values.assign(24,0);zero.values[0]=-0.;
    auto signs=Lut3DNode(source({0,0,0,1,0,0,.5f,0,0},3),zero).render({0,0,3,1});
    require(std::signbit(signs.rgb[0]) && !std::signbit(signs.rgb[3]) && std::signbit(signs.rgb[6]),"endpoint/equal-zero helper order");
}
void requested_level_and_failures() {
    auto s=table(2,[](double r,double g,double b){return std::array<double,3>{r*g,g,b};},{0,0,0},{1,1,1});
    Lut3DNode node(source({0,1,0,1,0,0},2),s);
    require(node.render_level({0,0,1,1},{1,RenderQuality::Preview}).rgb[0]==.25f,"map requested level,not native-before-average");
    require(node.input_region_level({0,0,1,1},{0,0,2,1},{1,RenderQuality::Preview}).width==1,"added halo");
    rejects([&]{node.render_level({0,0,1,1},{1,RenderQuality::Final});});
    rejects([&]{node.render_level({0,0,1,1},{3,RenderQuality::Preview});});
    for(unsigned fault=0;fault<12;++fault) {
        Lut3DSettings bad;
        if(fault==0)bad.size=1;if(fault==1)bad.size=18;if(fault==2)bad.size=std::numeric_limits<unsigned>::max();
        if(fault==3)bad.values.pop_back();if(fault==4)bad.values.push_back(0);
        if(fault==5)bad.input_max[0]=0;if(fault==6)bad.input_min[1]=2;
        if(fault==7)bad.values[0]=std::numeric_limits<double>::quiet_NaN();
        if(fault==8)bad.input_max[2]=std::numeric_limits<double>::infinity();
        if(fault==9)bad.values[0]=65537;
        if(fault==10)bad.input_max[0]=1e-300;
        if(fault==11){bad=table(3,[](double,double,double){return std::array<double,3>{0,0,0};});bad.input_min[0]=1;bad.input_max[0]=std::nextafter(1.,2.);}
        rejects([&]{validate_lut3d_settings(bad);});
    }
    auto maximum=table(17,[](double,double,double){return std::array<double,3>{65536,-65536,0};});
    validate_lut3d_settings(maximum);
    s=table(2,[](double r,double,double){return std::array<double,3>{65536*r,0,0};},{0,0,0},{1,1,1});
    validate_lut3d_settings(s);
    rejects([&]{Lut3DNode(source({std::numeric_limits<float>::max(),0,0},1),s).render({0,0,1,1});});
    auto cancellation=table(2,[](double r,double g,double){return std::array<double,3>{2*r*(1-2*g),0,0};},{0,0,0},{1,1,1});
    require(Lut3DNode(source({std::numeric_limits<float>::max(),.5f,0},1),cancellation).render({0,0,1,1}).rgb[0]==0,
            "finite binary64 intermediates may exceed float32 range before final cancellation");
    auto infinite_fraction=table(2,[](double,double,double){return std::array<double,3>{1,1,1};},{0,0,0},{1e-300,1,1});
    rejects([&]{Lut3DNode(source({1e38f,0,0},1),infinite_fraction).render({0,0,1,1});});
    auto intermediate=table(2,[](double r,double g,double){return std::array<double,3>{r==0 || g==0 ? 0:1e-270,0,0};},{0,0,0},{1e-270,1e-270,1});
    rejects([&]{Lut3DNode(source({1e38f,1e38f,0},1),intermediate).render({0,0,1,1});});
    rejects([&]{Lut3DNode bad(nullptr);});
    rejects([&]{Lut3DNode bad(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})));});
    class Bad final:public Node {
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor() const noexcept override {return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}
        Tile render(Rect r) const override {
            Tile t{r,{1,2,3},output_descriptor()};
            if(fault==0)t.rgb.pop_back();if(fault==1)++t.bounds.x;if(fault==2)t.descriptor=ImageDescriptor::camera_linear();
            if(fault==3)t.rgb[2]=std::numeric_limits<float>::infinity();return t;
        }
    };
    auto bad=std::make_shared<Bad>();Lut3DNode identity(bad);
    for(unsigned i=0;i<4;++i){bad->fault=i;rejects([&]{identity.render({0,0,1,1});});}
}
}
int main() {
    try {independent_polynomial_truth();identity_and_endpoint_bits();requested_level_and_failures();
        std::cout<<"3D LUT tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
