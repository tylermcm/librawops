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
    throw std::runtime_error("expected vibrance exception");
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
        for (double amount:{-1.,-.5,0.,.5,1.}) {
            VibranceSettings settings{amount}; VibranceNode node(input,settings); settings.amount=1;
            const auto output=node.render({0,0,5,1});
            require(output.descriptor==input->output_descriptor(),"vibrance changed descriptor");
            for (unsigned p=0;p<5;++p) {
                const double y=weights[0]*pixels[p*3]+weights[1]*pixels[p*3+1]+weights[2]*pixels[p*3+2];
                double actual_y=0;
                for (unsigned c=0;c<3;++c) {
                    const auto begin=pixels.begin()+p*3;
                    const double delta=double(*std::max_element(begin,begin+3))-double(*std::min_element(begin,begin+3));
                    const double weight=std::abs(y)/(std::abs(y)+delta);
                    const double expected=(1-(1+amount*weight))*y+(1+amount*weight)*pixels[p*3+c];
                    require(std::abs(output.rgb[p*3+c]-expected)<=3e-7*std::max(1.,std::abs(expected)),"independent affine vibrance truth differs");
                    actual_y+=weights[c]*output.rgb[p*3+c];
            }
                require(std::abs(actual_y-y)<1e-6,"vibrance did not preserve luminance within float32 rounding");
                }
            if (amount==0) require(std::memcmp(output.rgb.data(),pixels.data(),pixels.size()*sizeof(float))==0,"identity changed bits");
            require(Renderer{}.render_image(node,{0,0,5,1},RenderRequest{{0,0,5,1},2}).rgb==output.rgb,"tile partition differs");
        }
        const float max=std::numeric_limits<float>::max();
        const std::vector<float> extreme{-0.0f,0.0f,-0.0f,max,-max,1e-30f,max,max,max,-max,-max,-max};
        auto input_extreme=source(extreme,space,4);
        const auto identity=VibranceNode(input_extreme,{}).render({0,0,4,1});
        require(std::memcmp(identity.rgb.data(),extreme.data(),extreme.size()*sizeof(float))==0,"extreme identity changed bits");
        auto neutral=source({-0.0f,0.0f,-0.0f,max,max,max,-max,-max,-max},space,3);
        const auto original=neutral->render({0,0,3,1});
        for (double amount:{-1.,.3,1.}) {
            const auto output=VibranceNode(neutral,{amount}).render({0,0,3,1});
            require(std::memcmp(output.rgb.data(),original.rgb.data(),original.rgb.size()*sizeof(float))==0,"neutral bypass changed bits");
        }
    }
}
void reduction_and_errors() {
    std::vector<float> pixels;
    for (unsigned i=0;i<15;++i) pixels.insert(pixels.end(),{float(i)/8-.5f,float(i%4)/4,float(i%7)/3});
    auto input=source(pixels,WorkingSpace::LinearRec2020D65,5,3);
    VibranceNode node(input,{.75});
    for (unsigned mip:{1u,2u}) {
        RenderLevel level{mip,RenderQuality::Preview}; Rect roi{0,0,mip==1?3u:2u,mip==1?2u:1u};
        const auto reduced=input->render_level(roi,level);
        auto reference=source(reduced.rgb,WorkingSpace::LinearRec2020D65,roi.width,roi.height);
        require(node.render_level(roi,level).rgb==VibranceNode(reference,{.75}).render(roi).rgb,"vibrance must map requested-level input");
        require(node.input_region_level(roi,roi,level).width==roi.width,"vibrance adds halo");
    }
    for (double invalid:{-1.1,1.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { VibranceNode invalid_node(input,{invalid}); });
    rejects([&] { VibranceNode invalid_node(nullptr,{}); });
    auto camera=std::make_shared<RawUnpackNode>(RawImage(1,1,{32768}));
    rejects([&] { VibranceNode invalid_node(camera,{}); });
    rejects([&] { node.render_level({0,0,1,1},{3,RenderQuality::Preview}); });
    rejects([&] { node.render_level({0,0,1,1},{1,RenderQuality::Final}); });
    const float max=std::numeric_limits<float>::max();
    VibranceNode overflow(source({max,-max,0},WorkingSpace::LinearProPhotoD50,1),{1});
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
    auto bad=std::make_shared<Bad>(); VibranceNode checked(bad,{});
    for (unsigned i=0;i<3;++i) { bad->fault=i; rejects([&] { checked.render({0,0,1,1}); }); }
    for (unsigned c=0;c<3;++c) for (unsigned fault=3;fault<6;++fault) for (double amount:{-1.,0.,1.}) {
        bad->fault=fault; bad->channel=c; VibranceNode invalid(bad,{amount});
        rejects([&] { invalid.render({0,0,1,1}); });
    }
}
void adaptive_metamorphic_controls() {
    for (auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        // Frozen float32 colors found by an offline integer-relation search.
        // The exact pinned evaluation produces Y0 without neutral RGB.
        const bool prophoto=space==WorkingSpace::LinearProPhotoD50;
        const std::vector<float> zero_y=prophoto
            ? std::vector<float>{1.578406810760498f,2.1256375312805176f,-22974.f}
            : std::vector<float>{2.3305158615112305f,-.028333187103271484f,-10.f};
        const double wr=prophoto ? .28807112822929337 : .26270021201126703;
        const double wb=prophoto ? .00008565396060525903 : .059301716469861945;
        const double y=(double(zero_y[1])+wr*(double(zero_y[0])-zero_y[1]))+wb*(double(zero_y[2])-zero_y[1]);
        require(y==0 && zero_y[0]!=zero_y[1],"zero-Y colored fixture drifted");
        auto zero_input=source(zero_y,space,1);
        for (double amount:{-1.,-.5,0.,.5,1.}) {
            auto bypass=VibranceNode(zero_input,{amount}).render({0,0,1,1});
            require(std::memcmp(bypass.rgb.data(),zero_y.data(),12)==0,"zero-Y colored bypass changed bits");
        }
        // A neutral reduced pair must bypass, while its individually adapted
        // colors do not average back to that neutral (nonlinear order truth).
        auto input=source({1,0,0,0,1,1,1,0,0,0,1,1},space,2,2);
        VibranceNode node(input,{1});
        auto reduced=node.render_level({0,0,1,1},{1,RenderQuality::Preview});
        require(reduced.rgb==std::vector<float>({.5f,.5f,.5f}),"reduced neutral bypass failed");
        auto native=node.render({0,0,2,2});
        double mean=0; for (unsigned i=0;i<4;++i) mean+=native.rgb[i*3]/4.;
        require(std::abs(mean-.5)>.01,"adaptive native-before-reduce counterexample vanished");
        const float tiny=std::numeric_limits<float>::denorm_min()*128;
        auto small=source({tiny,0,0},space,1);
        auto scaled=source({tiny*8,0,0},space,1);
        auto a=VibranceNode(small,{1}).render({0,0,1,1});
        auto b=VibranceNode(scaled,{1}).render({0,0,1,1});
        require(a.rgb[0]!=tiny && a.rgb[1]<0,"hidden epsilon lost subnormal chroma map");
        for (unsigned c=0;c<3;++c)
            require(std::abs(double(b.rgb[c])-double(a.rgb[c])*8)<=8*std::numeric_limits<float>::denorm_min(),"subnormal scale equivariance outside rounding");
        // A tiny nonzero amount rounds to scale1, preserving even zero bits.
        auto rounded=VibranceNode(small,{1e-30}).render({0,0,1,1});
        const auto original=small->render({0,0,1,1});
        require(std::memcmp(rounded.rgb.data(),original.rgb.data(),12)==0,"computed identity changed bits");
        auto color=source({.25f,.5f,.75f},space,1);
        auto negative=VibranceNode(color,{-1}).render({0,0,1,1});
        require(negative.rgb[0]!=negative.rgb[1],"minus one incorrectly promises grayscale");
    }
}

}
int main() {
    try { numeric_truth_and_identity(); reduction_and_errors(); adaptive_metamorphic_controls(); std::cout<<"Vibrance tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
