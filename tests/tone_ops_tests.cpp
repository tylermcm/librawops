#include "ToneOps.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace rawengine;
namespace {
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected tone exception");
}
std::shared_ptr<const Node> source(std::vector<float> pixels,unsigned w,unsigned h=1) {
    return std::make_shared<RasterSourceNode>(RasterImage({w,h,0,WorkingSpace::LinearProPhotoD50},std::move(pixels)));
}
void explicit_curve_truth() {
    const std::vector<float> samples{-1,-.5f,-0.0f,0,.125f,.25f,.5f,.75f,1,1.5f,2};
    std::vector<float> pixels;
    for (float v:samples) pixels.insert(pixels.end(),3,v);
    CurvesSettings settings;
    settings.channels[0].knots={{0,0},{.25,.5},{1,1}};
    settings.channels[1].knots={{0,0},{.5,.25},{1,1}};
    settings.channels[2].knots={{0,1},{1,0}};
    CurvesNode node(source(pixels,11),settings);
    settings.channels[0].knots={{0,9},{1,9}}; // Caller mutation cannot change the node.
    const auto output=node.render({0,0,11,1});
    const float red[]{-2,-1,0,0,.25f,.5f,2.0f/3,5.0f/6,1,4.0f/3,5.0f/3};
    const float green[]{-.5f,-.25f,0,0,.0625f,.125f,.25f,.625f,1,1.75f,2.5f};
    for (std::size_t i=0;i<samples.size();++i) {
        require(output.rgb[i*3]==red[i] && output.rgb[i*3+1]==green[i] && output.rgb[i*3+2]==1-samples[i],
                "piecewise interpolation/extrapolation/knots differ from explicit truth");
    }
    require(Renderer{}.render_image(node,{0,0,11,1},RenderRequest{{0,0,11,1},2}).rgb==output.rgb,"curve tile partition differs");
    require(output.descriptor==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50),"curve changed descriptor");
}
void identity_levels_and_reduction() {
    const std::vector<float> pixels{-0.0f,-2,3,.25f,.5f,.75f,-.125f,1,2,std::numeric_limits<float>::max(),1e-20f,-1e20f};
    auto input=source(pixels,4);
    CurvesSettings identity;
    for (auto& c:identity.channels) c.knots={{-8,-8},{-1,-1},{.5,.5},{9,9}};
    CurvesNode curve(input,identity); LevelsNode levels(input,{});
    const auto a=curve.render({0,0,4,1}),b=levels.render({0,0,4,1});
    require(std::memcmp(a.rgb.data(),pixels.data(),pixels.size()*sizeof(float))==0 && a.rgb==b.rgb && std::signbit(b.rgb[0]),
            "identity curves/levels must preserve exact bits including signed zero and extreme headroom");
    CurvesSettings partly;
    partly.channels[1].knots={{0,0},{1,2}};
    CurvesNode partial(input,partly);
    require(std::signbit(partial.render({0,0,4,1}).rgb[0]),"individual identity channel changed signed zero");
    std::vector<float> ramp;
    for (int i=0;i<15;++i) ramp.insert(ramp.end(),3,float(i)/8-.5f);
    auto raster=source(ramp,5,3);
    LevelsSettings mapping;
    for (auto& c:mapping.channels) c={.25,.75,-1,2};
    LevelsNode mapped(raster,mapping);
    const auto result=mapped.render({0,0,5,3});
    for (std::size_t i=0;i<ramp.size();++i) require(result.rgb[i]==6*ramp[i]-2.5f,"affine levels truth differs");
    for (unsigned mip:{1u,2u}) {
        const RenderLevel level{mip,RenderQuality::Preview};
        const Rect roi{0,0,mip==1 ? 3u:2u,mip==1 ? 2u:1u};
        const auto upstream=raster->render_level(roi,level),output=mapped.render_level(roi,level);
        for (std::size_t i=0;i<output.rgb.size();++i) require(output.rgb[i]==6*upstream.rgb[i]-2.5f,"levels must follow upstream reduction");
        require(mapped.input_region_level(roi,roi,level).width==roi.width,"point edit added halo");
    }
}
void validation_and_bad_input() {
    CurvesSettings settings;
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    for (const auto& knots:{std::vector<CurvePoint>{{0,0}},{{0,0},{0,1}},{{1,0},{0,1}},{{0,0},{nan,1}},
                           {{0,0},{1,65537}},{{0,0},{1e-300,1}},std::vector<CurvePoint>(257)}) {
        settings.channels[0].knots=knots;
        rejects([&] { validate_curves_settings(settings); });
    }
    settings={}; settings.channels[0].knots.clear();
    for (unsigned i=0;i<256;++i) settings.channels[0].knots.push_back({double(i)/255,double(i)/255});
    validate_curves_settings(settings);
    for (auto invalid:{ChannelLevels{1,1,0,1},{2,1,0,1},{0,1,2,1},{0,1,0,nan}}) {
        LevelsSettings s; s.channels[0]=invalid;
        rejects([&] { validate_levels_settings(s); });
    }
    class Bad final:public Node {
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor() const noexcept override { return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50); }
        Tile render(Rect r) const override {
            Tile t{r,{1,2,3},output_descriptor()};
            if (fault==0) t.rgb.pop_back();
            if (fault==1) ++t.bounds.x;
            if (fault==2) t.descriptor=ImageDescriptor::camera_linear();
            if (fault==3) t.rgb[0]=std::numeric_limits<float>::infinity();
            return t;
        }
    };
    auto bad=std::make_shared<Bad>(); CurvesNode curve(bad,{});
    for (unsigned fault=0;fault<4;++fault) { bad->fault=fault; rejects([&] { curve.render({0,0,1,1}); }); }
    rejects([&] { curve.render_level({0,0,1,1},{1,RenderQuality::Preview}); });
    auto huge=source(std::vector<float>(3,std::numeric_limits<float>::max()),1);
    CurvesSettings twice; for (auto& c:twice.channels) c.knots={{0,0},{1,2}};
    CurvesNode overflow(huge,twice); rejects([&] { overflow.render({0,0,1,1}); });
    auto camera=std::make_shared<RawUnpackNode>(RawImage(1,1,{32768}));
    rejects([&] { CurvesNode rejected(camera,{}); });
}
} // namespace
int main() {
    try { explicit_curve_truth(); identity_levels_and_reduction(); validation_and_bad_input();
        std::cout<<"Tone operations tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
