#include "EditGraph.hpp"
#include "GeometryOps.hpp"
#include "SpatialOps.hpp"
#include "ToneOps.hpp"
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
using namespace rawengine;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected saved-control rejection");}
EditValue list(std::initializer_list<double> values){EditValue::Array a;for(double v:values)a.push_back(EditValue{v});return EditValue{a};}
template<class C>EditValue list(const C& values){EditValue::Array a;for(double v:values)a.push_back(EditValue{v});return EditValue{a};}
template<class Curve>EditValue knots(const Curve& curve){EditValue::Array a;for(auto p:curve.knots)a.push_back(list({p.x,p.y}));return EditValue{a};}
EditValue integer(unsigned x){return EditValue{std::int64_t(x)};}
struct Control{const char* name;EditValue::Object parameters;std::shared_ptr<const Node> direct;Rect canvas;};
void verify(WorkingSpace space){
    constexpr Rect bounds{0,0,9,7};std::vector<float> data(9*7*3);for(unsigned i=0;i<data.size();++i)data[i]=float(int(i*19%37)-7)/16;
    auto input=std::make_shared<RasterSourceNode>(RasterImage({9,7,0,space},data));
    EditSource source;source.id="96000000-0000-0000-0000-000000000001";source.working_space=space;source.content_sha256=*input->source_fingerprint();
    std::vector<Control> controls;const double amount=.123456789;
    auto add=[&](const char* name,EditValue::Object parameters,std::shared_ptr<const Node> direct,Rect canvas=Rect{0,0,9,7}){controls.push_back({name,std::move(parameters),std::move(direct),canvas});};
    add("saturation",{{"amount",EditValue{1-1e-8}}},std::make_shared<SaturationNode>(input,SaturationSettings{1-1e-8}));
    add("vibrance",{{"amount",EditValue{amount}}},std::make_shared<VibranceNode>(input,VibranceSettings{amount}));
    add("grayscale",{},std::make_shared<GrayscaleNode>(input));
    GradingSettings grading;grading.lift={amount,-amount,0};grading.gain={1.23456789,1,1.125};grading.gamma={.987654321,1.125,1};
    add("grading",{{"lift",list(grading.lift)},{"gain",list(grading.gain)},{"gamma",list(grading.gamma)}},std::make_shared<GradingNode>(input,grading));
    DehazeSettings dehaze{amount,{.123456789,.234567891,.345678912}};
    add("dehaze",{{"amount",EditValue{amount}},{"atmospheric_light",list(dehaze.atmospheric_light)}},std::make_shared<DehazeNode>(input,dehaze));
    TonalRangeSettings tonal{amount,-.234567891,.345678912,-.456789123};
    add("tonal_range",{{"blacks",EditValue{tonal.blacks}},{"shadows",EditValue{tonal.shadows}},{"highlights",EditValue{tonal.highlights}},{"whites",EditValue{tonal.whites}}},std::make_shared<TonalRangeNode>(input,tonal));
    ChannelMixerSettings channel;channel.matrix={1,amount,0,0,1,-amount,amount,0,1};
    add("channel_mixer",{{"matrix",list(channel.matrix)}},std::make_shared<ChannelMixerNode>(input,channel));
    ColorBalanceSettings balance;balance.shadows={amount,-amount,0};balance.highlights={0,amount,-amount};
    add("color_balance",{{"shadows",list(balance.shadows)},{"midtones",list(balance.midtones)},{"highlights",list(balance.highlights)},{"preserve_luminance",EditValue{true}}},std::make_shared<ColorBalanceNode>(input,balance));
    ColorMixerSettings mixer;mixer.hue_shift[2]=amount;mixer.saturation_delta[4]=amount;mixer.luminance_delta[6]=-amount;
    add("color_mixer",{{"hue_shift",list(mixer.hue_shift)},{"saturation_delta",list(mixer.saturation_delta)},{"luminance_delta",list(mixer.luminance_delta)}},std::make_shared<ColorMixerNode>(input,mixer));
    CurvesSettings curves;for(auto& c:curves.channels)c.knots={{-.25,-.123456789},{.234567891,.345678912},{1.5,1.456789123}};
    add("curves",{{"red",knots(curves.channels[0])},{"green",knots(curves.channels[1])},{"blue",knots(curves.channels[2])}},std::make_shared<CurvesNode>(input,curves));
    LevelsSettings levels;for(auto& c:levels.channels)c={-.123456789,1.23456789,-.234567891,1.345678912};
    add("levels",{{"input_black",list({-.123456789,-.123456789,-.123456789})},{"input_white",list({1.23456789,1.23456789,1.23456789})},{"output_black",list({-.234567891,-.234567891,-.234567891})},{"output_white",list({1.345678912,1.345678912,1.345678912})}},std::make_shared<LevelsNode>(input,levels));
    ExtendedCurvesSettings extended;extended.interpolation=CurveInterpolation::ShapePreservingCubic;extended.master.knots={{0,0},{.5,.234567891},{1,1}};
    for(auto& c:extended.channels)c.knots={{-.25,-.123456789},{.234567891,.345678912},{1.5,1.456789123}};
    add("curves_extended",{{"master",knots(extended.master)},{"red",knots(extended.channels[0])},{"green",knots(extended.channels[1])},{"blue",knots(extended.channels[2])},{"interpolation",EditValue{std::string("shape_preserving_cubic")}}},std::make_shared<ExtendedCurvesNode>(input,extended));
    GammaLevelsSettings gamma;gamma.channels=levels.channels;gamma.gamma={.987654321,1.125,1};
    add("levels_gamma",{{"input_black",list({-.123456789,-.123456789,-.123456789})},{"input_white",list({1.23456789,1.23456789,1.23456789})},{"output_black",list({-.234567891,-.234567891,-.234567891})},{"output_white",list({1.345678912,1.345678912,1.345678912})},{"gamma",list(gamma.gamma)}},std::make_shared<GammaLevelsNode>(input,gamma));
    Lut1DSettings lut1;lut1.input_min=-.123456789;lut1.input_max=1.23456789;for(auto& c:lut1.channels)c={-.234567891,amount,1.345678912};
    EditValue::Array channels;for(auto& c:lut1.channels)channels.push_back(list(c));EditValue::Object lut1params{{"input_min",EditValue{lut1.input_min}},{"input_max",EditValue{lut1.input_max}},{"channels",EditValue{channels}}};
    add("lut1d",lut1params,std::make_shared<Lut1DNode>(input,lut1));add("lut1d_large",lut1params,std::make_shared<LargeLut1DNode>(input,lut1));
    Lut3DSettings lut3;lut3.values[3]=.987654321;EditValue::Object lut3params{{"size",integer(lut3.size)},{"input_min",list(lut3.input_min)},{"input_max",list(lut3.input_max)},{"values",list(lut3.values)}};
    add("lut3d",lut3params,std::make_shared<Lut3DNode>(input,lut3));add("lut3d_large",lut3params,std::make_shared<LargeLut3DNode>(input,lut3));
    add("clarity",{{"amount",EditValue{amount}},{"radius",integer(1)}},std::make_shared<ClarityNode>(input,bounds,ClaritySettings{amount,1}));
    add("guided_filter_working_y",{{"radius",integer(1)},{"epsilon",EditValue{0.000123456789}}},std::make_shared<WorkingYGuidedFilterNode>(input,bounds,WorkingYGuidedFilterSettings{1,0.000123456789}));
    add("texture",{{"amount",EditValue{amount}},{"scale",integer(1)}},std::make_shared<TextureNode>(input,bounds,TextureSettings{amount,1}));
    add("sharpen",{{"amount",EditValue{amount}},{"radius",integer(1)}},std::make_shared<SharpenNode>(input,bounds,SharpenSettings{amount,1}));
    add("rotate",{{"angle_degrees",EditValue{90-1e-8}}},std::make_shared<RotateNode>(input,bounds,RotateSettings{90-1e-8}));
    ProjectiveSettings projective{11,9,{1,amount,0,-amount,1,0,amount,0,1}};
    add("projective",{{"width",integer(11)},{"height",integer(9)},{"source_from_output",list(projective.source_from_output)}},std::make_shared<ProjectiveNode>(input,bounds,projective),{0,0,11,9});
    add("cubic_resize",{{"width",integer(11)},{"height",integer(9)}},std::make_shared<CubicResizeNode>(input,bounds,CubicResizeSettings{11,9}),{0,0,11,9});
    for(const auto& control:controls){
        EditOperation op;op.id="96000000-0000-0000-0000-000000000090";op.type_id=std::string("rawengine.")+control.name;op.processing_version=2;op.input_domain=op.output_domain=space==WorkingSpace::LinearProPhotoD50?EditDomain::SceneLinearProPhotoD50:EditDomain::SceneLinearRec2020D65;op.inputs={{"image",source.id}};op.parameters=control.parameters;
        EditManifest manifest;manifest.working_space=space;manifest.sources={source};manifest.operations={op};manifest.output_id=op.id;
        auto encoded=serialize_edit_manifest(manifest);auto parsed=parse_edit_manifest(encoded);require(serialize_edit_manifest(parsed)==encoded,"canonical schema replay");
        auto cache=std::make_shared<TileCache>(1<<20);ExecutableEditGraph graph(parsed,{{source,input,bounds}},nullptr,cache);
        require(graph.output_bounds().width==control.canvas.width&&graph.output_bounds().height==control.canvas.height,"saved geometry extent");
        for(unsigned mip=0;mip<=2;++mip){
            const auto s=1u<<mip;RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};Rect rect{1,0,std::max(1u,(control.canvas.width+s-1)/s-1),(control.canvas.height+s-1)/s};
            auto direct=control.direct->render_level(rect,level),saved=graph.output().render_level(rect,level);require(saved.bounds.x==direct.bounds.x&&saved.rgb.size()==direct.rgb.size()&&!std::memcmp(saved.rgb.data(),direct.rgb.data(),saved.rgb.size()*4),control.name);
            const auto upstream=control.direct->input_level(level);
            const auto upstream_scale=1u<<upstream.mip;
            const Rect upstream_bounds{0,0,(bounds.width+upstream_scale-1)/upstream_scale,(bounds.height+upstream_scale-1)/upstream_scale};
            auto footprint=control.direct->input_region_level(rect,upstream_bounds,level);auto regions=graph.required_source_regions(rect,level);require(regions.size()==1,"one-source footprint identity");auto actual=regions.begin()->second;
            // Source-region inspection returns native coordinates regardless of
            // whether the control consumes native or requested-level samples.
            if(upstream.mip){
                footprint.x*=upstream_scale;footprint.y*=upstream_scale;footprint.width=std::min(footprint.width*upstream_scale,bounds.width-footprint.x);footprint.height=std::min(footprint.height*upstream_scale,bounds.height-footprint.y);
            }
            require(actual.x==footprint.x&&actual.y==footprint.y&&actual.width==footprint.width&&actual.height==footprint.height,"graph/direct native source support");
            auto before=cache->stats();auto again=graph.output().render_level(rect,level);require(cache->stats().misses==before.misses&&!std::memcmp(saved.rgb.data(),again.rgb.data(),saved.rgb.size()*4),"cache replay across levels");
        }
        for(bool enabled:{false,true}){
            auto bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].parameters.emplace("extra",integer(1));rejects([&]{ExecutableEditGraph g(bad,{{source,input,bounds}});});
            bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].schema_version=2;rejects([&]{ExecutableEditGraph g(bad,{{source,input,bounds}});});
        }
    }
    std::cout<<"Verified "<<controls.size()<<" direct/saved control bindings in working space "<<unsigned(space)<<'\n';
}
}
void verify_pipeline_bindings();
int main(){try{verify(WorkingSpace::LinearProPhotoD50);verify(WorkingSpace::LinearRec2020D65);verify_pipeline_bindings();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
