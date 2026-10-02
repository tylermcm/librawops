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
    throw std::runtime_error("expected LUT rejection");
}
std::shared_ptr<const Node> source(std::vector<float> p,unsigned w,unsigned h=1,
                                 WorkingSpace s=WorkingSpace::LinearProPhotoD50) {
    return std::make_shared<RasterSourceNode>(RasterImage({w,h,0,s},std::move(p)));
}
void explicit_truth_and_ownership() {
    // Explicit dyadic equations independent of native grid lookup/interpolation.
    for (auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        std::vector<float> p;
        for (float v:{-2.f,-1.f,-.5f,0.f,.5f,1.f,2.f,3.f}) p.insert(p.end(),3,v);
        Lut1DSettings s; s.input_min=-1; s.input_max=1;
        s.channels={{{-2,0,1},{2,1,-1},{.25,.25,.25}}};
        Lut1DNode lut(source(p,8,1,space),s); s.channels[0][0]=99;
        const auto out=lut.render({0,0,8,1});
        for (std::size_t i=0;i<p.size();i+=3) {
            const float v=p[i];
            require(out.rgb[i]==(v<0 ? 2*v:v),"red piecewise truth");
            require(out.rgb[i+1]==(v<0 ? 1-v:1-2*v),"descending green truth");
            require(out.rgb[i+2]==.25f,"constant extrapolation truth");
        }
        require(Renderer{}.render_image(lut,{0,0,8,1},RenderRequest{{0,0,8,1},3}).rgb==out.rgb,"partition differs");
        require(out.descriptor==ImageDescriptor::scene_linear(space),"descriptor changed");
    }
}
void identities_and_grid_limits() {
    const std::vector<float> p{-0.f,0.f,-0.f,std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),std::numeric_limits<float>::denorm_min()};
    auto input=source(p,2);
    for (unsigned n:{2u,3u,7u,256u}) {
        Lut1DSettings s; s.input_min=-2; s.input_max=3;
        for (auto& channel:s.channels) {
            channel.clear();
            for (unsigned i=0;i<n;++i) channel.push_back(i==0 ? -2. : i==n-1 ? 3. : -2.+5.*(double(i)/double(n-1)));
        }
        Lut1DNode lut(input,s); const auto out=lut.render({0,0,2,1});
        require(std::memcmp(out.rgb.data(),p.data(),p.size()*sizeof(float))==0,"identity bits changed");
    }
    Lut1DSettings s; s.channels[1]={1,0};
    const auto out=Lut1DNode(input,s).render({0,0,2,1});
    require(std::signbit(out.rgb[0]) && out.rgb[3]==p[3] && out.rgb[5]==p[5],"partial identity changed");
    s={}; s.channels[0][1]=1.+std::ldexp(1.,-22);
    require(Lut1DNode(source({1,1,1},1),s).render({0,0,1,1}).rgb[0]>1,"approximate identity bypass");
}
void reduction_and_validation() {
    Lut1DSettings s; s.channels={{{0,0,1},{0,0,1},{0,0,1}}};
    auto input=source({0,0,0,1,1,1},2); Lut1DNode node(input,s);
    const auto reduced=node.render_level({0,0,1,1},{1,RenderQuality::Preview});
    require(reduced.rgb==std::vector<float>(3,0),"LUT must follow upstream reduction");
    require(node.input_region_level({0,0,1,1},{0,0,2,1},{1,RenderQuality::Preview}).width==1,"LUT halo");
    rejects([&]{node.render_level({0,0,1,1},{1,RenderQuality::Final});});
    for (unsigned fault=0;fault<10;++fault) {
        Lut1DSettings bad;
        if (fault==0) bad.channels[0]={0};
        if (fault==1) bad.channels[1]={0,.5,1};
        if (fault==2) for (auto& c:bad.channels) c.resize(257);
        if (fault==3) bad.input_max=0;
        if (fault==4) bad.input_min=2;
        if (fault==5) bad.input_min=std::numeric_limits<double>::quiet_NaN();
        if (fault==6) bad.channels[2][0]=std::numeric_limits<double>::infinity();
        if (fault==7) bad.channels[0][0]=65537;
        if (fault==8) bad.input_max=1e-300;
        if (fault==9) {bad.input_min=1;bad.input_max=std::nextafter(1.,2.);for(auto& c:bad.channels)c={1,1,1};}
        rejects([&]{validate_lut1d_settings(bad);});
    }
    s={}; s.channels[0]={0,2};
    rejects([&]{Lut1DNode(source({std::numeric_limits<float>::max(),0,0},1),s).render({0,0,1,1});});
    rejects([&]{Lut1DNode bad(nullptr);});
    rejects([&]{Lut1DNode bad(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})));});
    class Bad final:public Node {
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor() const noexcept override {return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}
        Tile render(Rect r) const override {
            Tile t{r,{1,2,3},output_descriptor()};
            if(fault==0)t.rgb.pop_back(); if(fault==1)++t.bounds.x;
            if(fault==2)t.descriptor=ImageDescriptor::camera_linear();
            if(fault==3)t.rgb[1]=std::numeric_limits<float>::quiet_NaN();
            return t;
        }
    };
    auto bad=std::make_shared<Bad>(); Lut1DNode identity(bad);
    for(unsigned i=0;i<4;++i){bad->fault=i;rejects([&]{identity.render({0,0,1,1});});}
}
}
int main() {
    try {explicit_truth_and_ownership();identities_and_grid_limits();reduction_and_validation();
        std::cout<<"1D LUT tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
