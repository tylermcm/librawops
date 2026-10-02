#include "ToneOps.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace rawengine;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected saturation exception");
}
std::shared_ptr<const Node> source(const std::vector<float>& pixels,WorkingSpace space,unsigned w,unsigned h=1) {
    return std::make_shared<RasterSourceNode>(RasterImage({w,h,0,space},pixels));
}
void numeric_truth_and_identity() {
    const std::vector<float> pixels{1,0,0,0,1,0,0,0,1,-1,.5f,2,3,-2,1};
    for (const auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        const std::array<double,3> weights=space==WorkingSpace::LinearProPhotoD50
            ? std::array<double,3>{.28807112822929337,.7118432178101014,.00008565396060525903}
            : std::array<double,3>{.26270021201126703,.677998071518871,.059301716469861945};
        auto input=source(pixels,space,5);
        for (double amount:{0.,.5,1.,2.,4.}) {
            SaturationSettings settings{amount}; SaturationNode node(input,settings); settings.amount=0;
            const auto output=node.render({0,0,5,1});
            require(output.descriptor==input->output_descriptor(),"saturation changed descriptor");
            for (unsigned p=0;p<5;++p) {
                const double y=weights[0]*pixels[p*3]+weights[1]*pixels[p*3+1]+weights[2]*pixels[p*3+2];
                double actual_y=0;
                for (unsigned c=0;c<3;++c) {
                    const double expected=(1-amount)*y+amount*pixels[p*3+c];
                    require(std::abs(output.rgb[p*3+c]-expected)<=3e-7*std::max(1.,std::abs(expected)),"independent affine saturation truth differs");
                    actual_y+=weights[c]*output.rgb[p*3+c];
                }
                require(std::abs(actual_y-y)<1e-6,"saturation did not preserve luminance within float32 rounding");
                if (amount==0) require(output.rgb[p*3]==output.rgb[p*3+1] && output.rgb[p*3]==output.rgb[p*3+2],"zero amount is not grayscale");
            }
            if (amount==1) require(std::memcmp(output.rgb.data(),pixels.data(),pixels.size()*sizeof(float))==0,"identity changed bits");
            require(Renderer{}.render_image(node,{0,0,5,1},RenderRequest{{0,0,5,1},2}).rgb==output.rgb,"tile partition differs");
        }
        const float max=std::numeric_limits<float>::max();
        const std::vector<float> extreme{-0.0f,0.0f,-0.0f,max,-max,1e-30f,max,max,max,-max,-max,-max};
        auto input_extreme=source(extreme,space,4);
        const auto identity=SaturationNode(input_extreme,{}).render({0,0,4,1});
        require(std::memcmp(identity.rgb.data(),extreme.data(),extreme.size()*sizeof(float))==0,"extreme identity changed bits");
        auto neutral=source({-0.0f,0.0f,-0.0f,max,max,max,-max,-max,-max},space,3);
        const auto original=neutral->render({0,0,3,1});
        for (double amount:{0.,.3,4.}) {
            const auto output=SaturationNode(neutral,{amount}).render({0,0,3,1});
            require(std::memcmp(output.rgb.data(),original.rgb.data(),original.rgb.size()*sizeof(float))==0,"neutral bypass changed bits");
        }
    }
}
void reduction_and_errors() {
    std::vector<float> pixels;
    for (unsigned i=0;i<15;++i) pixels.insert(pixels.end(),{float(i)/8-.5f,float(i%4)/4,float(i%7)/3});
    auto input=source(pixels,WorkingSpace::LinearRec2020D65,5,3);
    SaturationNode node(input,{1.75});
    for (unsigned mip:{1u,2u}) {
        RenderLevel level{mip,RenderQuality::Preview}; Rect roi{0,0,mip==1?3u:2u,mip==1?2u:1u};
        const auto reduced=input->render_level(roi,level);
        auto reference=source(reduced.rgb,WorkingSpace::LinearRec2020D65,roi.width,roi.height);
        require(node.render_level(roi,level).rgb==SaturationNode(reference,{1.75}).render(roi).rgb,"saturation must map requested-level input");
        require(node.input_region_level(roi,roi,level).width==roi.width,"saturation adds halo");
    }
    for (double invalid:{-0.1,4.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { SaturationNode invalid_node(input,{invalid}); });
    rejects([&] { SaturationNode invalid_node(nullptr,{}); });
    auto camera=std::make_shared<RawUnpackNode>(RawImage(1,1,{32768}));
    rejects([&] { SaturationNode invalid_node(camera,{}); });
    rejects([&] { node.render_level({0,0,1,1},{3,RenderQuality::Preview}); });
    rejects([&] { node.render_level({0,0,1,1},{1,RenderQuality::Final}); });
    const float max=std::numeric_limits<float>::max();
    SaturationNode overflow(source({max,-max,0},WorkingSpace::LinearProPhotoD50,1),{4});
    rejects([&] { overflow.render({0,0,1,1}); });
    class Bad final:public Node {
    public:
        unsigned fault=0,channel=0;
        ImageDescriptor output_descriptor() const noexcept override { return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50); }
        Tile render(Rect r) const override {
            Tile tile{r,{1,2,3},output_descriptor()};
            if (fault==0) tile.rgb.pop_back();
            if (fault==1) ++tile.bounds.x;
            if (fault==2) tile.descriptor=ImageDescriptor::camera_linear();
            if (fault==3) tile.rgb[channel]=std::numeric_limits<float>::quiet_NaN();
            if (fault==4) tile.rgb[channel]=std::numeric_limits<float>::infinity();
            if (fault==5) tile.rgb[channel]=-std::numeric_limits<float>::infinity();
            return tile;
        }
    };
    auto bad=std::make_shared<Bad>(); SaturationNode checked(bad,{});
    for (unsigned i=0;i<3;++i) { bad->fault=i; rejects([&] { checked.render({0,0,1,1}); }); }
    for (unsigned c=0;c<3;++c) for (unsigned fault=3;fault<6;++fault) for (double amount:{0.,1.,2.}) {
        bad->fault=fault; bad->channel=c; SaturationNode invalid(bad,{amount});
        rejects([&] { invalid.render({0,0,1,1}); });
    }
}
}
int main() {
    try { numeric_truth_and_identity(); reduction_and_errors(); std::cout<<"Saturation tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
