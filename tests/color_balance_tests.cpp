#include "ToneOps.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool yes,const char* why) {if(!yes)throw std::runtime_error(why);}
template<class F>void rejects(F f) {
    try {f();}catch(const std::invalid_argument&){return;}
    throw std::runtime_error("expected color balance rejection");
}
std::shared_ptr<const Node> source(std::vector<float> p,unsigned w=1,WorkingSpace space=WorkingSpace::LinearProPhotoD50) {
    return std::make_shared<RasterSourceNode>(RasterImage({w,1,0,space},std::move(p)));
}
void controls() {
    const float maximum=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
    const std::vector<float> pixels{-0.f,0.f,-0.f,maximum,-maximum,tiny,-maximum,maximum,-tiny};
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        auto input=source(pixels,3,space);
        for(bool preserve:{false,true}) {
            ColorBalanceSettings s;s.preserve_luminance=preserve;
            auto out=ColorBalanceNode(input,s).render({0,0,3,1});
            require(std::memcmp(out.rgb.data(),pixels.data(),pixels.size()*sizeof(float))==0,"zero controls preserve extreme/sign bits");
        }
        for(double amount:{-1.,.5,1.}) {
            ColorBalanceSettings s;s.shadows.fill(amount);s.midtones.fill(amount);s.highlights.fill(amount);
            auto out=ColorBalanceNode(input,s).render({0,0,3,1});
            require(std::memcmp(out.rgb.data(),pixels.data(),pixels.size()*sizeof(float))==0,"projected common-offset identity");
        }
        ColorBalanceSettings tint;tint.shadows={1,0,0};
        auto out=ColorBalanceNode(source({0,0,0},1,space),tint).render({0,0,1,1});
        const double wr=space==WorkingSpace::LinearProPhotoD50 ? .28807112822929337 : .26270021201126703;
        require(out.rgb==std::vector<float>{float(1-wr),float(-wr),float(-wr)},"neutral/black tint and Y projection");
        tint.preserve_luminance=false;
        out=ColorBalanceNode(source({-0.f,-0.f,-0.f},1,space),tint).render({0,0,1,1});
        require(out.rgb[0]==1 && std::signbit(out.rgb[1]) && std::signbit(out.rgb[2]),"component-zero copies original bits");
        tint.shadows[0]=std::ldexp(1.,-140);
        require(ColorBalanceNode(source({0,0,0},1,space),tint).render({0,0,1,1}).rgb[0]>0,"tiny offset executes");
        ColorBalanceSettings s;s.shadows={1,-1,.5};s.midtones={-1,1,-.5};s.highlights={1,-1,.5};
        const std::vector<float> neutral{.5f,.5f,.5f};
        require(ColorBalanceNode(source(neutral,1,space),s).render({0,0,1,1}).rgb==neutral,"exact weighted cancellation");
        s={};s.midtones={1,-1,.5};
        const std::vector<float> inactive{-0.f,0.f,-0.f,2,2,2};
        out=ColorBalanceNode(source(inactive,2,space),s).render({0,0,2,1});
        require(std::memcmp(out.rgb.data(),inactive.data(),inactive.size()*sizeof(float))==0,"inactive endpoint controls");
        s={};s.midtones={1,0,0};s.preserve_luminance=false;
        auto base=source({0,0,0,1,1,1},2,space);ColorBalanceNode node(base,s);s.midtones[0]=0;
        require(node.render_level({0,0,1,1},{1,RenderQuality::Preview}).rgb==std::vector<float>{1,.5f,.5f},"owned settings/map requested level before nonlinear edit");
        require(node.input_node()==base.get() && node.output_descriptor()==base->output_descriptor(),"ownership/descriptor");
        require(node.input_region_level({0,0,1,1},{0,0,2,1},{1,RenderQuality::Preview}).width==1,"no halo");
        require(Renderer{}.render_image(node,{0,0,2,1},RenderRequest{{0,0,2,1},1}).rgb==node.render({0,0,2,1}).rgb,"tile parity");
        rejects([&]{node.render_level({0,0,1,1},{1,RenderQuality::Final});});
        rejects([&]{node.render_level({0,0,1,1},{3,RenderQuality::Preview});});
        // A valid bounded offset is rounded away near float32 extremes.
        s.shadows={-1,1,-1};s.midtones={1,-1,1};s.highlights={1,-1,1};
        out=ColorBalanceNode(input,s).render({0,0,3,1});
        for(float value:out.rgb)require(std::isfinite(value),"bounded mapping of extreme finite inputs");
    }
    for(unsigned field=0;field<3;++field)for(double value:{-1.01,1.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        ColorBalanceSettings s;(field==0 ? s.shadows:field==1 ? s.midtones:s.highlights)[2]=value;
        rejects([&]{validate_color_balance_settings(s);});
    }
    rejects([]{ColorBalanceNode node(nullptr);});
    rejects([]{ColorBalanceNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})));});
}
void malformed_producer() {
    class Bad final:public Node {
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor() const noexcept override {return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}
        Tile render(Rect r)const override {
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
    for(unsigned fault=0;fault<6;++fault)for(bool preserve:{false,true}) {
        bad->fault=fault;ColorBalanceSettings s;s.preserve_luminance=preserve;
        rejects([&]{ColorBalanceNode(bad,s).render({0,0,1,1});});
        s.shadows.fill(.5);s.midtones.fill(.5);s.highlights.fill(.5);
        rejects([&]{ColorBalanceNode(bad,s).render({0,0,1,1});});
    }
}
}
int main() {
    try {controls();malformed_producer();std::cout<<"Color balance native tests passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
