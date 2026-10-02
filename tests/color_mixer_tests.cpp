#include "ToneOps.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool yes,const char* why) { if (!yes) throw std::runtime_error(why); }
template<class F> void rejects(F f) {
    try { f(); } catch(const std::invalid_argument&) { return; }
    throw std::runtime_error("expected color mixer rejection");
}
std::shared_ptr<const Node> source(std::vector<float> p,unsigned w=1,
        WorkingSpace space=WorkingSpace::LinearProPhotoD50) {
    return std::make_shared<RasterSourceNode>(RasterImage({w,1,0,space},std::move(p)));
}
void direct_api() {
    const float max=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
    const std::vector<float> p{-0.f,0.f,-0.f,max,max,max,-max,-max,-max,tiny,tiny,tiny,max,-max,tiny};
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        auto input=source(p,5,space);auto unchanged=ColorMixerNode(input).render({0,0,5,1});
        require(std::memcmp(p.data(),unchanged.rgb.data(),p.size()*sizeof(float))==0,"zero settings bits");
        ColorMixerSettings s;s.hue_shift.fill(60);s.saturation_delta.fill(1);s.luminance_delta.fill(-1);
        auto neutrals=ColorMixerNode(source({-0.f,0.f,-0.f,max,max,max,-max,-max,-max,tiny,tiny,tiny},4,space),s).render({0,0,4,1});
        require(std::memcmp(p.data(),neutrals.rgb.data(),12*sizeof(float))==0,"extreme/zero neutral bits");
        s={};s.saturation_delta.fill(.5);auto input2=source({-.5f,.75f,2.f},1,space);
        auto mixer=ColorMixerNode(input2,s);s.saturation_delta.fill(-1);
        require(mixer.render({0,0,1,1}).rgb==SaturationNode(input2,{1.5}).render({0,0,1,1}).rgb,"owned settings/uniform saturation parity");
        require(mixer.output_descriptor()==input2->output_descriptor(),"descriptor");
        require(mixer.input_node()==input2.get(),"input lifetime");
        require(mixer.input_region_level({0,0,1,1},{0,0,2,1},{1,RenderQuality::Preview}).width==1,"no halo");
        rejects([&]{mixer.render_level({0,0,1,1},{1,RenderQuality::Final});});
        rejects([&]{mixer.render_level({0,0,1,1},{3,RenderQuality::Preview});});
        s={};s.luminance_delta.fill(1);
        std::vector<float> zero=space==WorkingSpace::LinearProPhotoD50 ? std::vector<float>{1.578406810760498f,2.1256375312805176f,-22974.f} :
            std::vector<float>{2.3305158615112305f,-.028333187103271484f,-10.f};
        require(ColorMixerNode(source(zero,1,space),s).render({0,0,1,1}).rgb==zero,"computed zero Y exact bypass");
        s={};s.hue_shift[3]=60;std::vector<float> red{1,0,-0.f};
        auto out=ColorMixerNode(source(red,1,space),s).render({0,0,1,1});
        require(std::memcmp(red.data(),out.rgb.data(),12)==0,"inactive band bits");
        s={};s.hue_shift[0]=30;s.hue_shift[1]=-30;s.saturation_delta[0]=.5;s.saturation_delta[1]=-.5;
        s.luminance_delta[0]=1;s.luminance_delta[1]=-1;std::vector<float> mid{1,.25f,-0.f};
        out=ColorMixerNode(source(mid,1,space),s).render({0,0,1,1});
        require(std::memcmp(mid.data(),out.rgb.data(),12)==0,"weighted effective cancellation bits");
        s={};s.hue_shift.fill(std::ldexp(1.,-20));
        require(ColorMixerNode(source({1,0,0},1,space),s).render({0,0,1,1}).rgb!=std::vector<float>{1,0,0},"tiny hue edit executes");
        s={};s.saturation_delta.fill(1);
        rejects([&]{ColorMixerNode(source({max,0,0},1,space),s).render({0,0,1,1});});
        // Chroma intermediate exceeds float32, but final desaturated RGB fits.
        s.saturation_delta.fill(-.75);
        out=ColorMixerNode(source({max,-max,-max},1,space),s).render({0,0,1,1});
        for(float v:out.rgb)require(std::isfinite(v),"double intermediate cancellation");
    }
    rejects([]{ColorMixerNode node(nullptr);});
    rejects([]{ColorMixerNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})));});
    for(unsigned field=0;field<3;++field)for(double v:{-61.,61.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        ColorMixerSettings s;(field==0 ? s.hue_shift:field==1 ? s.saturation_delta:s.luminance_delta)[7]=v;
        rejects([&]{validate_color_mixer_settings(s);});
    }
}
void hostile_upstream() {
    class Bad final:public Node {
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor() const noexcept override {return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}
        Tile render(Rect r) const override {
            Tile t{r,{1,2,3},output_descriptor()};
            if(fault==0)t.bounds.x++;
            if(fault==1)t.rgb.pop_back();
            if(fault==2)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
            if(fault==3)t.rgb[0]=std::numeric_limits<float>::infinity();
            if(fault==4)t.rgb[1]=std::numeric_limits<float>::quiet_NaN();
            if(fault==5)t.rgb[2]=-std::numeric_limits<float>::infinity();
            return t;
        }
    };
    auto bad=std::make_shared<Bad>();
    for(unsigned fault=0;fault<6;++fault) {bad->fault=fault;rejects([&]{ColorMixerNode(bad).render({0,0,1,1});});}
}
}
int main() {
    try {direct_api();hostile_upstream();std::cout<<"Color mixer native tests passed\n";}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
