#include "ParametricMask.hpp"
#include "CoverageDelivery.hpp"
#include "EditGraph.hpp"
#include "TileScheduler.hpp"
#include "reference/parametric_mask_v1.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace rawengine;
namespace {
void require(bool condition,const char* message) {if (!condition) throw std::runtime_error(message);}
template<class Exception=std::invalid_argument,class Callback> void rejects(Callback action) {
    try {action();} catch (const Exception&) {return;}
    throw std::runtime_error("expected parametric rejection did not occur");
}
using Settings=parametric_mask_reference::Settings;
double value(std::uint64_t word) {return std::bit_cast<double>(word);}
LinearGradientMaskSettings linear(const Settings& s) {
    return {{value(s.geometry[0]),value(s.geometry[1])},{value(s.geometry[2]),value(s.geometry[3])},s.invert};
}
RadialGradientMaskSettings radial(const Settings& s) {
    return {{value(s.geometry[0]),value(s.geometry[1])},
            {value(s.geometry[2]),value(s.geometry[3]),value(s.geometry[4]),value(s.geometry[5])},value(s.geometry[6]),s.invert};
}
PolygonMaskSettings polygon(const Settings& s) {
    PolygonMaskSettings result;result.invert=s.invert;
    for (const auto& ring:s.rings) {
        std::vector<MaskPoint> points;for (auto p:ring) points.push_back({value(p.x),value(p.y)});
        result.rings.push_back(std::move(points));
    }
    return result;
}
std::shared_ptr<const CoverageNode> node(std::shared_ptr<const CoverageNode> input,const Settings& s) {
    if (s.kind==0) return std::make_shared<CoverageLinearGradientNode>(input,linear(s));
    if (s.kind==1) return std::make_shared<CoverageRadialGradientNode>(input,radial(s));
    return std::make_shared<CoveragePolygonNode>(input,polygon(s));
}
std::vector<float> values(std::span<const std::uint32_t> words) {
    std::vector<float> result;for (auto word:words) result.push_back(std::bit_cast<float>(word));return result;
}
bool exact(const std::vector<float>& output,std::span<const std::uint32_t> words) {
    return output.size()==words.size() && std::equal(output.begin(),output.end(),words.begin(),
        [](float v,std::uint32_t word) {return std::bit_cast<std::uint32_t>(v)==word;});
}
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}
std::string id(unsigned value) {
    char tail[16];std::snprintf(tail,sizeof(tail),"%012u",value);return std::string("00000000-0000-4000-8000-")+tail;
}
EditValue point_value(double x,double y) {return EditValue{EditValue::Array{EditValue{x},EditValue{y}}};}
EditValue::Object parameters(const Settings& s) {
    if (s.kind==0) return {{"start",point_value(value(s.geometry[0]),value(s.geometry[1]))},
        {"end",point_value(value(s.geometry[2]),value(s.geometry[3]))},{"invert",EditValue{s.invert}}};
    if (s.kind==1) return {{"center",point_value(value(s.geometry[0]),value(s.geometry[1]))},
        {"axes",EditValue{EditValue::Array{EditValue{value(s.geometry[2])},EditValue{value(s.geometry[3])},
                                         EditValue{value(s.geometry[4])},EditValue{value(s.geometry[5])}}}},
        {"inner",EditValue{value(s.geometry[6])}},{"invert",EditValue{s.invert}}};
    EditValue::Array rings;
    for (const auto& ring:s.rings) {
        EditValue::Array points;for (auto p:ring) points.push_back(point_value(value(p.x),value(p.y)));
        rings.push_back(EditValue{std::move(points)});
    }
    return {{"rings",EditValue{std::move(rings)}},{"invert",EditValue{s.invert}}};
}
struct GraphFixture {
    EditManifest manifest;
    std::vector<BoundEditSource> bindings;
    explicit GraphFixture(const parametric_mask_reference::Frame& row) {
        const Rect bounds{row.x,row.y,row.width,row.height};
        auto input=std::make_shared<CoverageRasterNode>(CoverageImage({bounds,0},values(row.input)));
        EditSource source;source.id=id(1);source.kind=EditSourceKind::CoverageRasterF32;
        source.content_sha256=*input->source_fingerprint();
        manifest.format_version=4;manifest.sources={source};manifest.output_id=id(2);
        EditOperation op;op.id=id(2);op.processing_version=2;op.input_domain=op.output_domain=EditDomain::Coverage;
        op.type_id=row.settings.kind==0?"rawengine.mask_linear_gradient":row.settings.kind==1?
                   "rawengine.mask_radial_gradient":"rawengine.mask_polygon";
        op.parameters=parameters(row.settings);op.inputs={{"mask",id(1)}};manifest.operations={op};
        bindings={{source,nullptr,bounds,input}};
    }
};

void saved_graphs() {
    for (const auto& row:parametric_mask_reference::frames) {
        GraphFixture f(row);const auto json=serialize_edit_manifest(f.manifest);
        require(serialize_edit_manifest(parse_edit_manifest(json))==json,"parametric manifest JSON round trip changed");
        ExecutableEditGraph graph(parse_edit_manifest(json),f.bindings);
        require(graph.output_is_coverage(),"parametric graph lost scalar output");rejects([&] {graph.output_handle();});
        for (unsigned mip=0;mip<=2;++mip) {
            const RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
            const auto bounds=graph.coverage_output().output_bounds(level);
            const auto expected=mip==0?row.native:mip==1?row.mip1:row.mip2;
            for (unsigned size:{1u,2u,8u})
                require(exact(CoverageRenderer{}.render_image(graph.coverage_output(),RenderRequest{bounds,size,level}).coverage,expected),
                        "saved parametric graph differs from frozen frame");
            for (unsigned y=0;y<bounds.height;++y) for (unsigned x=0;x<bounds.width;++x) {
                const Rect roi{bounds.x+x,bounds.y+y,1,1};const auto scale=1u<<mip;
                const Rect support=mip?Rect{row.x+x*scale,row.y+y*scale,std::min(scale,row.width-x*scale),
                                            std::min(scale,row.height-y*scale)}:roi;
                const auto regions=graph.required_source_regions(roi,level);
                require(regions.size()==1 && same(regions.at(id(1)),support),"parametric graph lost native source-ID footprint");
            }
        }
    }
    const char* canonical[]={parametric_mask_reference::operation0_json,parametric_mask_reference::operation1_json,
                             parametric_mask_reference::operation2_json};
    for (unsigned kind=0;kind<3;++kind) {
        GraphFixture f(parametric_mask_reference::frames[kind]);auto json=serialize_edit_manifest(f.manifest);
        const auto start=json.find("\"operations\":[")+14;const auto end=json.find("],\"output\"",start);
        require(end!=std::string::npos,"missing parametric operation array");json.replace(start,end-start,canonical[kind]);
        const auto parsed=parse_edit_manifest(json);
        require(serialize_edit_manifest(parsed).find(canonical[kind])!=std::string::npos,"parametric operation changed independent canonical bytes");
        ExecutableEditGraph graph(parsed,f.bindings);
        graph.coverage_output().render({0,0,7,5});
    }
}

void saved_guards() {
    for (unsigned kind=0;kind<3;++kind) {
        GraphFixture f(parametric_mask_reference::frames[kind]);
        auto reject=[&](const EditManifest& changed) {rejects<std::exception>([&] {ExecutableEditGraph graph(changed,f.bindings);});};
        for (bool enabled:{true,false}) {
            auto base=f.manifest;base.operations[0].enabled=enabled;
            for (unsigned variant=0;variant<13;++variant) {
                auto changed=base;auto& op=changed.operations[0];
                if (variant==0) op.inputs={{"image",id(1)}};
                if (variant==1) op.masks={{"coverage",id(1)}};
                if (variant==2) op.opacity=0.5;
                if (variant==3) op.blend_mode="multiply";
                if (variant==4) op.extra_fields={{"extra",EditValue{true}}};
                if (variant==5) op.schema_version=2;
                if (variant==6) op.processing_version=1;
                if (variant==7) op.input_domain=EditDomain::SceneLinearProPhotoD50;
                if (variant==8) op.output_domain=EditDomain::SceneLinearProPhotoD50;
                if (variant==9) op.parameters.at("invert")=EditValue{std::int64_t{1}};
                if (variant==10) op.parameters.emplace("extra",EditValue{0.0});
                if (variant==11) op.parameters.erase("invert");
                if (variant==12) op.inputs={{"mask",id(2)}};
                reject(changed);
            }
            for (unsigned variant=0;variant<5;++variant) {
                auto changed=base;auto& params=changed.operations[0].parameters;
                if (kind==0) {
                    if (variant==0) params.at("start")=EditValue{false};
                    if (variant==1) params.at("end")=point_value(0.5,0.5); // Matches this fixture's start.
                    if (variant==2) params.at("end")=EditValue{EditValue::Array{EditValue{true},EditValue{1.0}}};
                    if (variant==3) params.at("start")=EditValue{EditValue::Array{EditValue{0.0}}};
                    if (variant==4) params.at("end")=point_value(0x1p34,0);
                } else if (kind==1) {
                    if (variant==0) params.at("center")=EditValue{nullptr};
                    if (variant==1) params.at("axes")=EditValue{EditValue::Array{EditValue{1.0},EditValue{0.0},EditValue{2.0},EditValue{0.0}}};
                    if (variant==2) params.at("axes")=EditValue{EditValue::Array{EditValue{1.0},EditValue{true},EditValue{0.0},EditValue{1.0}}};
                    if (variant==3) params.at("axes")=EditValue{EditValue::Array{EditValue{1.0}}};
                    if (variant==4) params.at("inner")=EditValue{1.1};
                } else {
                    if (variant==0) params.at("rings")=EditValue{false};
                    if (variant==1) params.at("rings")=EditValue{EditValue::Array{}};
                    if (variant==2) params.at("rings")=EditValue{EditValue::Array{EditValue{true}}};
                    if (variant==3) params.at("rings")=EditValue{EditValue::Array{EditValue{EditValue::Array{point_value(0,0),point_value(1,1)}}}};
                    if (variant==4) params.at("rings")=EditValue{EditValue::Array{EditValue{EditValue::Array{
                        point_value(0,0),point_value(1,1),EditValue{EditValue::Array{EditValue{0.0},EditValue{true}}}}}}};
                }
                reject(changed);
            }
        }
        for (unsigned format:{1u,2u,3u}) {auto changed=f.manifest;changed.format_version=format;reject(changed);}
        auto disabled=f.manifest;disabled.operations[0].enabled=false;
        ExecutableEditGraph bypass(disabled,f.bindings);
        require(exact(bypass.coverage_output().render({0,0,7,5}).coverage,parametric_mask_reference::frames[kind].input),
                "disabled parametric node failed upstream bit alias");
        auto bindings=f.bindings;bindings[0].identity.content_sha256[0]^=1;
        rejects([&] {ExecutableEditGraph graph(f.manifest,bindings);});
        bindings=f.bindings;bindings[0].bounds.x=1;
        rejects([&] {ExecutableEditGraph graph(f.manifest,bindings);});
        auto rgb=std::make_shared<RasterSourceNode>(RasterImage({7,5,0,WorkingSpace::LinearProPhotoD50},std::vector<float>(105,1)));
        auto typed=f.manifest;typed.sources[0].kind=EditSourceKind::SceneLinearRasterF32;
        typed.sources[0].working_space=WorkingSpace::LinearProPhotoD50;typed.sources[0].content_sha256=*rgb->source_fingerprint();
        rejects([&] {ExecutableEditGraph graph(typed,{{typed.sources[0],rgb,{0,0,7,5}}});});
    }
}

void cache_history_jobs() {
    for (unsigned kind=0;kind<3;++kind) {
        GraphFixture f(parametric_mask_reference::frames[kind]);auto cache=std::make_shared<TileCache>(1024*1024);
        ExecutableEditGraph first(f.manifest,f.bindings,nullptr,cache);
        const auto before=first.coverage_output().render({0,0,7,5}).coverage;auto stats=cache->stats();
        first.coverage_output().render({0,0,7,5});require(cache->stats().hits>stats.hits,"parametric output cache miss on repeat");
        auto changed=f.manifest;changed.operations[0].parameters.at("invert")=EditValue{true};
        ExecutableEditGraph second(changed,f.bindings,nullptr,cache);stats=cache->stats();
        const auto after=second.coverage_output().render({0,0,7,5}).coverage;
        require(before!=after && cache->stats().hits>stats.hits && cache->stats().misses>stats.misses,
                "parametric settings reused stale output or lost upstream cache reuse");
        auto copied=first.coverage_output().render({0,0,7,5});copied.coverage[0]=1;
        require(first.coverage_output().render({0,0,7,5}).coverage==before,"parametric cache return is mutable alias");
        cache->clear();require(cache->stats().entries==0,"parametric cache clear failed");
        require(first.coverage_output().render({0,0,7,5}).coverage==before,"parametric clear changed output");
        auto tiny=std::make_shared<TileCache>(1);ExecutableEditGraph uncached(f.manifest,f.bindings,nullptr,tiny);
        uncached.coverage_output().render({0,0,7,5});require(tiny->stats().entries==0,"parametric exceeded shared cache budget");
        EditHistory history(f.manifest,f.bindings,{8,1024*1024},nullptr,cache);const auto original=history.current();
        history.commit(changed);const auto newest=history.current()->id;
        history.undo();require(history.current()->graph->coverage_output().render({0,0,7,5}).coverage==before,"parametric undo failed");
        history.redo();require(history.current()->id==newest,"parametric redo failed");
        auto restored=EditHistory::restore(history.serialize(),f.bindings,nullptr,cache);
        require(restored->current()->graph->coverage_output().render({0,0,7,5}).coverage==after,"parametric history restore changed output");
        auto invalid=changed;invalid.operations[0].parameters.at("invert")=EditValue{std::string("false")};
        rejects([&] {history.commit(invalid);});require(history.current()->id==newest,"invalid parametric commit published history");
        TileScheduler scheduler(2,8);
        auto pending=scheduler.submit(original->graph->coverage_output_handle(),RenderRequest{{0,0,7,5},1,{}});
        f.bindings.clear();f.manifest.operations.clear();require(pending.get().coverage==before,"parametric job lost pinned source/settings");
        auto token=std::make_shared<CancellationToken>();token->cancel();
        auto failed=scheduler.submit(second.coverage_output_handle(),RenderRequest{{0,0,7,5},1,{}},RenderPriority::Normal,token);
        rejects<RenderCancelled>([&] {failed.get();});
        require(scheduler.submit(second.coverage_output_handle(),RenderRequest{{0,0,7,5},2,{}}).get().coverage==after,
                "parametric job did not recover after cancellation");
    }
}

struct Gate {
    std::promise<void> entered,release;
    std::shared_future<void> released=release.get_future().share();
    std::atomic<bool> opened=false;
    void open() {if (!opened.exchange(true)) release.set_value();}
};
struct ReleaseOnExit {std::shared_ptr<Gate> gate;~ReleaseOnExit() {gate->open();}};
class GatedInput final:public CoverageNode {
public:
    GatedInput(std::shared_ptr<const CoverageNode> input,std::shared_ptr<Gate> gate,bool fail=false)
        :input_(std::move(input)),gate_(std::move(gate)),fail_(fail) {}
    CoverageTile render_level(Rect roi,RenderLevel level) const override {
        if (visits_.fetch_add(1)==0) {
            gate_->entered.set_value();gate_->released.get();if (fail_) throw std::bad_alloc();
        }
        return input_->render_level(roi,level);
    }
    bool supports_level(RenderLevel level) const noexcept override {return input_->supports_level(level);}
    Rect native_bounds() const noexcept override {return input_->native_bounds();}
    std::optional<std::array<std::uint8_t,32>> source_fingerprint() const override {return input_->source_fingerprint();}
    Rect required_native_region(Rect roi,RenderLevel level) const override {return input_->required_native_region(roi,level);}
private:
    std::shared_ptr<const CoverageNode> input_;
    std::shared_ptr<Gate> gate_;bool fail_;
    mutable std::atomic<unsigned> visits_=0;
};
void entered(std::future<void>& started) {
    require(started.wait_for(std::chrono::seconds(10))==std::future_status::ready,"parametric gate timed out");started.get();
}
void generation_and_recovery() {
    for (unsigned kind=0;kind<3;++kind) {
        {
            GraphFixture f(parametric_mask_reference::frames[kind]);auto cache=std::make_shared<TileCache>(1024*1024);
            auto gate=std::make_shared<Gate>();auto started=gate->entered.get_future();
            f.bindings[0].coverage_node=std::make_shared<GatedInput>(f.bindings[0].coverage_node,gate);
            ExecutableEditGraph graph(f.manifest,f.bindings,nullptr,cache);
            auto pending=std::async(std::launch::async,[&] {return graph.coverage_output().render({0,0,7,5});});
            ReleaseOnExit release{gate};entered(started);cache->clear();gate->open();
            require(exact(pending.get().coverage,parametric_mask_reference::frames[kind].native),"parametric in-flight clear changed frozen output");
            require(cache->stats().entries==0,"old generation parametric work repopulated cleared cache");
            require(exact(graph.coverage_output().render({0,0,7,5}).coverage,parametric_mask_reference::frames[kind].native),"parametric new-generation recovery changed output");
            require(cache->stats().entries>0,"parametric new generation failed cache admission");
        }
        for (bool fail:{true,false}) {
            GraphFixture f(parametric_mask_reference::frames[kind]);auto gate=std::make_shared<Gate>();auto started=gate->entered.get_future();
            TileScheduler scheduler(1,2);ReleaseOnExit release{gate};
            auto input=std::make_shared<GatedInput>(f.bindings[0].coverage_node,gate,fail);
            auto generated=node(input,parametric_mask_reference::frames[kind].settings);
            const RenderRequest request{{0,0,7,5},2,{}};
            auto first=fail?scheduler.submit(generated,request):scheduler.submit_latest("geometry",generated,request);
            entered(started);
            auto next=fail?scheduler.submit(generated,request):scheduler.submit_latest("geometry",generated,request);
            gate->open();
            if (fail) rejects<std::bad_alloc>([&] {first.get();});else rejects<RenderCancelled>([&] {first.get();});
            require(exact(next.get().coverage,parametric_mask_reference::frames[kind].native),"queued parametric recovery/supersession changed output");
            require(exact(scheduler.submit(generated,request).get().coverage,parametric_mask_reference::frames[kind].native),"later parametric job failed after source fault/replacement");
        }
    }
}

class Constant final:public CoverageNode {
public:
    Constant(Rect bounds,float value=0.25f,int fault=0):bounds_(bounds),value_(value),fault_(fault) {}
    CoverageTile render_level(Rect roi,RenderLevel level) const override {
        require(level.mip==0 && level.quality==RenderQuality::Final,"parametric node did not request native Final");
        ++calls;
        CoverageTile tile{roi,std::vector<float>(static_cast<std::size_t>(roi.width)*roi.height,value_)};
        if (fault_==1) ++tile.bounds.x;
        if (fault_==2) tile.coverage.pop_back();
        if (fault_==3) tile.coverage.back()=std::numeric_limits<float>::quiet_NaN();
        if (fault_==4) tile.coverage.back()=-0.1f;
        return tile;
    }
    bool supports_level(RenderLevel level) const noexcept override {return level.mip==0 && level.quality==RenderQuality::Final;}
    Rect native_bounds() const noexcept override {return bounds_;}
    mutable unsigned calls=0;
private:
    Rect bounds_;float value_;int fault_;
};
void primitives() {
    unsigned products=0;
    for (const auto& row:parametric_mask_reference::cases) {
        const double x=value(row.x),y=value(row.y);
        const auto generated=row.settings.kind==0?evaluate_linear_gradient_mask(x,y,linear(row.settings)):
            row.settings.kind==1?evaluate_radial_gradient_mask(x,y,radial(row.settings)):
                                 evaluate_polygon_mask(x,y,polygon(row.settings));
        require(std::bit_cast<std::uint32_t>(generated)==row.shape,"parametric primitive differs from independent staged shape");
        if (x==0.5 && y==0.5) {
            auto input=std::make_shared<Constant>(Rect{0,0,1,1},std::bit_cast<float>(row.base));
            require(std::bit_cast<std::uint32_t>(node(input,row.settings)->render({0,0,1,1}).coverage[0])==row.output,
                    "parametric node product/endpoints differ from frozen scalar case");
            ++products;
        }
    }
    require(products>=400,"insufficient direct native scalar product coverage");
    std::cout<<products<<" frozen center products verified\n";
}
void frames() {
    CoverageRenderer renderer;
    for (const auto& row:parametric_mask_reference::frames) {
        const Rect native{row.x,row.y,row.width,row.height};
        auto input=std::make_shared<CoverageRasterNode>(CoverageImage({native,0},values(row.input)));
        auto generated=node(input,row.settings);
        for (RenderLevel level:{RenderLevel{},RenderLevel{0,RenderQuality::Preview},
                               RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            const auto bounds=generated->output_bounds(level);
            const auto expected=level.mip==0?row.native:level.mip==1?row.mip1:row.mip2;
            for (auto size:{1u,2u,8u})
                require(exact(renderer.render_image(*generated,RenderRequest{bounds,size,level}).coverage,expected),
                        "parametric native/preview/mip/tile frame differs from frozen product");
            const auto input_level=generated->input_level(level);
            require(input_level.mip==0 && input_level.quality==RenderQuality::Final,"parametric input level changed");
            for (std::uint32_t y=0;y<bounds.height;++y) for (std::uint32_t x=0;x<bounds.width;++x)
                for (std::uint32_t h=1;h<=bounds.height-y;++h) for (std::uint32_t w=1;w<=bounds.width-x;++w) {
                    const Rect roi{bounds.x+x,bounds.y+y,w,h};
                    const auto output=generated->render_level(roi,level);
                    for (std::uint32_t v=0;v<h;++v) for (std::uint32_t u=0;u<w;++u)
                        require(std::bit_cast<std::uint32_t>(output.coverage[v*w+u])==expected[(y+v)*bounds.width+x+u],
                                "parametric rectangular ROI changed frozen product");
                    const auto scale=1u<<level.mip;
                    const Rect footprint=level.mip?Rect{native.x+x*scale,native.y+y*scale,
                        std::min(w*scale,native.width-x*scale),std::min(h*scale,native.height-y*scale)}:roi;
                    require(same(generated->input_region_level(roi,native,level),footprint),"parametric ROI lost native block support");
                }
        }
    }
    auto input=std::make_shared<Constant>(Rect{0,0,3,3});
    LinearGradientMaskSettings ls{{0,0},{4,0},false};CoverageLinearGradientNode l(input,ls);
    const auto old_l=l.render({0,0,3,3}).coverage;ls.end={0,4};ls.invert=true;
    require(l.render({0,0,3,3}).coverage==old_l,"linear node retained mutable caller settings");
    RadialGradientMaskSettings rs{{1,1},{2,0,0,2},0,false};CoverageRadialGradientNode r(input,rs);
    const auto old_r=r.render({0,0,3,3}).coverage;rs.axes[0]=0;rs.inner=1;rs.invert=true;
    require(r.render({0,0,3,3}).coverage==old_r,"radial node retained mutable caller settings");
    PolygonMaskSettings ps{{{{0,0},{2,0},{2,2},{0,2}}},false};CoveragePolygonNode p(input,ps);
    const auto old_p=p.render({0,0,3,3}).coverage;ps.rings[0][0]={99,99};ps.rings.clear();ps.invert=true;
    require(p.render({0,0,3,3}).coverage==old_p,"polygon node retained mutable caller rings");
}
void guards() {
    auto input=std::make_shared<Constant>(Rect{0,0,7,5});
    const auto ls=linear(parametric_mask_reference::cases[0].settings);
    const RadialGradientMaskSettings rs{};
    const PolygonMaskSettings ps{{{{0,0},{2,0},{2,2}}},false};
    rejects([&] {CoverageLinearGradientNode n(nullptr,ls);});
    rejects([&] {CoverageRadialGradientNode n(nullptr,rs);});
    rejects([&] {CoveragePolygonNode n(nullptr,ps);});
    for (double bad:{-0x1p34,0x1p34,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto l=ls;l.start.x=bad;rejects([&] {validate_linear_gradient_mask_settings(l);});
        auto r=rs;r.center.y=bad;rejects([&] {validate_radial_gradient_mask_settings(r);});
        r=rs;r.axes[2]=bad;rejects([&] {validate_radial_gradient_mask_settings(r);});
        auto p=ps;p.rings[0][0].y=bad;rejects([&] {validate_polygon_mask_settings(p);});
        rejects([&] {evaluate_linear_gradient_mask(bad,0,ls);});
        rejects([&] {evaluate_radial_gradient_mask(0,bad,rs);});
        rejects([&] {evaluate_polygon_mask(bad,0,ps);});
    }
    for (double length:{0.0,0x1p-9})
        rejects([&] {validate_linear_gradient_mask_settings({{0,0},{length,0},false});});
    validate_linear_gradient_mask_settings({{0,0},{0x1p-8,0},false});
    for (std::array<double,4> axes: {std::array<double,4>{1,0,2,0},std::array<double,4>{0x1p-9,0,0,1},
                                   std::array<double,4>{std::numeric_limits<double>::denorm_min(),0,0,1}}) {
        auto r=rs;r.axes=axes;rejects([&] {validate_radial_gradient_mask_settings(r);});
    }
    validate_radial_gradient_mask_settings({{0,0},{0x1p-8,0,0,0x1p-8},0,false});
    for (double inner:{-0.1,1.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto r=rs;r.inner=inner;rejects([&] {validate_radial_gradient_mask_settings(r);});
    }
    rejects([&] {validate_polygon_mask_settings({});});
    auto p=ps;p.rings[0].resize(2);rejects([&] {validate_polygon_mask_settings(p);});
    p=ps;p.rings.resize(4097,ps.rings[0]);rejects([&] {validate_polygon_mask_settings(p);});
    p=ps;p.rings[0].resize(65537);rejects([&] {validate_polygon_mask_settings(p);});
    p=ps;p.rings[0].resize(65536);p.rings.resize(17,p.rings[0]);rejects([&] {validate_polygon_mask_settings(p);});
    p.rings.resize(16);validate_polygon_mask_settings(p);
    for (auto shape:{parametric_mask_reference::cases[0].settings,parametric_mask_reference::cases[230].settings,
                    parametric_mask_reference::cases[506].settings}) {
        auto generated=node(input,shape);
        for (RenderLevel level:{RenderLevel{3,RenderQuality::Preview},RenderLevel{1,RenderQuality::Final},
                               RenderLevel{0,static_cast<RenderQuality>(99)}}) {
            require(!generated->supports_level(level),"parametric node admitted unsupported level");
            rejects([&] {generated->render_level({0,0,1,1},level);});
            rejects([&] {generated->input_level(level);});
        }
        rejects([&] {generated->render({7,0,1,1});});rejects([&] {generated->render({0,0,0,1});});
        rejects([&] {generated->input_region_level({0,0,1,1},{1,0,7,5},{});});
        auto huge=std::make_shared<Constant>(Rect{0,0,0xffffffffu,0xffffffffu});auto impossible=node(huge,shape);
        rejects([&] {impossible->render(impossible->native_bounds());});require(huge->calls==0,"parametric capacity checked after rendering input");
        for (int fault=1;fault<=4;++fault) {
            auto damaged=node(std::make_shared<Constant>(Rect{0,0,7,5},0.25f,fault),shape);
            rejects<std::exception>([&] {damaged->render({0,0,7,5});});
            rejects<std::exception>([&] {damaged->render_level({0,0,2,2},{2,RenderQuality::Preview});});
        }
        for (Rect bounds:{Rect{0,0,0,5},Rect{0xffffffffu,0,1,1}})
            rejects([&] {node(std::make_shared<Constant>(bounds),shape);});
    }
}
} // namespace
int main() {
    try {primitives();frames();guards();saved_graphs();saved_guards();cache_history_jobs();generation_and_recovery();
        std::cout<<"988 parametric shape fixtures,36 native/mip product frames,exhaustive rectangular ROIs,ownership/guards passed\n";return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
