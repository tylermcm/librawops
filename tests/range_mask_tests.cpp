#include "RangeMask.hpp"
#include "CoverageDelivery.hpp"
#include "EditGraph.hpp"
#include "TileScheduler.hpp"
#include "reference/range_mask_v1.hpp"

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
using Settings=range_mask_reference::Settings;
void require(bool ok,const char* message) {if (!ok) throw std::runtime_error(message);}
template<class Call> void rejects(Call call) {
    try {call();} catch (const std::exception&) {return;}
    throw std::runtime_error("expected range rejection did not occur");
}
double number(std::uint64_t word) {return std::bit_cast<double>(word);}
float sample(std::uint32_t word) {return std::bit_cast<float>(word);}
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}
ScalarRangeSettings scalar(const Settings& s) {return {{number(s.controls[0]),number(s.controls[1]),number(s.controls[2]),number(s.controls[3])},s.invert};}
ColorRangeSettings color(const Settings& s) {
    return {{number(s.controls[0]),number(s.controls[1]),number(s.controls[2])},
            {number(s.controls[3]),number(s.controls[4]),number(s.controls[5])},number(s.controls[6]),number(s.controls[7]),s.invert};
}
ImageDescriptor descriptor(const Settings& s) {return ImageDescriptor::scene_linear(s.space?WorkingSpace::LinearRec2020D65:WorkingSpace::LinearProPhotoD50);}
std::vector<float> values(std::span<const std::uint32_t> words) {
    std::vector<float> result;for (auto word:words) result.push_back(sample(word));return result;
}
struct ScalarSource final : CoverageNode {
    Rect bounds;std::vector<float> data;int damage=0;mutable Rect request{};mutable RenderLevel level{};
    ScalarSource(Rect r,std::vector<float> v):bounds(r),data(std::move(v)) {}
    Rect native_bounds() const noexcept override {return bounds;}
    bool supports_level(RenderLevel l) const noexcept override {return l.mip==0;}
    CoverageTile render_level(Rect r,RenderLevel l) const override {
        request=r;level=l;CoverageTile result{r,{}};
        for (std::uint32_t y=0;y<r.height;++y) for (std::uint32_t x=0;x<r.width;++x)
            result.coverage.push_back(data.at(static_cast<std::size_t>(r.y-bounds.y+y)*bounds.width+r.x-bounds.x+x));
        if (damage==1) ++result.bounds.x;
        if (damage==2) result.coverage.pop_back();
        if (damage==3) result.coverage.back()=std::numeric_limits<float>::quiet_NaN();
        if (damage==4) result.coverage.back()=-1;
        return result;
    }
};
struct RgbSource final : Node {
    Rect bounds;std::vector<float> data;ImageDescriptor description;int damage=0;mutable Rect request{};mutable RenderLevel level{};
    RgbSource(Rect r,std::vector<float> v,ImageDescriptor d):bounds(r),data(std::move(v)),description(d) {}
    ImageDescriptor output_descriptor() const noexcept override {return description;}
    Tile render(Rect r) const override {return render_level(r,{});}
    bool supports_level(RenderLevel l) const noexcept override {return l.mip==0;}
    Tile render_level(Rect r,RenderLevel l) const override {
        request=r;level=l;Tile result{r,{},description};
        for (std::uint32_t y=0;y<r.height;++y) for (std::uint32_t x=0;x<r.width;++x) {
            const auto i=(static_cast<std::size_t>(r.y-bounds.y+y)*bounds.width+r.x-bounds.x+x)*3;
            for (unsigned c=0;c<3;++c) result.rgb.push_back(data.at(i+c));
        }
        if (damage==1) ++result.bounds.x;
        if (damage==2) result.rgb.pop_back();
        if (damage==3) result.rgb.back()=std::numeric_limits<float>::quiet_NaN();
        if (damage==4) result.descriptor=ImageDescriptor{};
        return result;
    }
};
std::shared_ptr<const CoverageNode> node(const Settings& s,std::shared_ptr<const ScalarSource> mask,
                                       std::shared_ptr<const RgbSource> rgb,std::shared_ptr<const ScalarSource> depth) {
    if (s.kind==0) return std::make_shared<CoverageLuminanceRangeNode>(mask,rgb,mask->bounds,scalar(s));
    if (s.kind==1) return std::make_shared<CoverageColorRangeNode>(mask,rgb,mask->bounds,color(s));
    return std::make_shared<CoverageDepthRangeNode>(mask,depth,scalar(s));
}
void scalar_cases() {
    for (const auto& row:range_mask_reference::cases) {
        const auto& s=row.settings;const auto rgb=std::array<float,3>{sample(row.rgb[0]),sample(row.rgb[1]),sample(row.rgb[2])};
        const float selection=s.kind==0?evaluate_luminance_range_mask(rgb,descriptor(s),scalar(s)):
                              s.kind==1?evaluate_color_range_mask(rgb,descriptor(s),color(s)):
                              evaluate_depth_range_mask(sample(row.depth),scalar(s));
        require(std::bit_cast<std::uint32_t>(selection)==row.selection,"frozen scalar selection differs");
        auto mask=std::make_shared<ScalarSource>(Rect{3,5,1,1},std::vector<float>{sample(row.base)});
        auto guide=std::make_shared<RgbSource>(mask->bounds,std::vector<float>(rgb.begin(),rgb.end()),descriptor(s));
        auto depth=std::make_shared<ScalarSource>(mask->bounds,std::vector<float>{sample(row.depth)});
        require(std::bit_cast<std::uint32_t>(node(s,mask,guide,depth)->render(mask->bounds).coverage[0])==row.output,
                "frozen actual node product differs");
    }
}
void frames() {
    for (const auto& row:range_mask_reference::frames) {
        auto mask=std::make_shared<ScalarSource>(Rect{row.x,row.y,row.width,row.height},values(row.input));
        auto guide=std::make_shared<RgbSource>(mask->bounds,row.settings.kind==2?std::vector<float>(row.width*row.height*3):values(row.guide),descriptor(row.settings));
        auto depth=std::make_shared<ScalarSource>(mask->bounds,row.settings.kind==2?values(row.guide):std::vector<float>(row.width*row.height));
        auto output=node(row.settings,mask,guide,depth);
        for (auto level:{RenderLevel{},RenderLevel{0,RenderQuality::Preview},RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            const auto bounds=output->output_bounds(level);const auto expected=level.mip==0?row.native:level.mip==1?row.mip1:row.mip2;
            for (unsigned tile:{1u,2u,8u}) for (std::uint32_t y=0;y<bounds.height;y+=tile) for (std::uint32_t x=0;x<bounds.width;x+=tile) {
                Rect roi{bounds.x+x,bounds.y+y,std::min(tile,bounds.width-x),std::min(tile,bounds.height-y)};
                const auto result=output->render_level(roi,level);require(same(result.bounds,roi),"tile origin differs");
                for (std::uint32_t v=0;v<roi.height;++v) for (std::uint32_t u=0;u<roi.width;++u)
                    require(std::bit_cast<std::uint32_t>(result.coverage[v*roi.width+u])==expected[(y+v)*bounds.width+x+u],"frozen tile pixel differs");
                const auto native=output->input_region_level(roi,mask->bounds,level);
                require(same(mask->request,native),"mask request differs from planner");
                require(same(row.settings.kind==2?depth->request:guide->request,native),"guide request differs from planner");
                require(mask->level.mip==0 && mask->level.quality==RenderQuality::Final,"mask is not native Final");
                const auto requested=row.settings.kind==2?depth->level:guide->level;
                require(requested.mip==0 && requested.quality==RenderQuality::Final,"guide is not native Final");
            }
            // Every nonempty rectangular ROI, not just original tile partitions.
            for (std::uint32_t y=0;y<bounds.height;++y) for (std::uint32_t x=0;x<bounds.width;++x)
                for (std::uint32_t h=1;h<=bounds.height-y;++h) for (std::uint32_t w=1;w<=bounds.width-x;++w) {
                    auto result=output->render_level({bounds.x+x,bounds.y+y,w,h},level);
                    for (std::uint32_t v=0;v<h;++v) for (std::uint32_t u=0;u<w;++u)
                        require(std::bit_cast<std::uint32_t>(result.coverage[v*w+u])==expected[(y+v)*bounds.width+x+u],"frozen rectangular ROI differs");
                }
        }
        rejects([&]{output->render_level(mask->bounds,{1,RenderQuality::Final});});
        rejects([&]{output->render({mask->bounds.x,mask->bounds.y,0,1});});
        rejects([&]{output->input_region_level(mask->bounds,{0,0,1,1},{});});
    }
}
void guards() {
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    for (double v:{nan,std::numeric_limits<double>::infinity(),-65537.,65537.}) {
        ScalarRangeSettings s;s.edges[0]=v;rejects([&]{validate_luminance_range_settings(s);});
    }
    for (double v:{nan,-1.,2.}) {ScalarRangeSettings s;s.edges[0]=v;rejects([&]{validate_depth_range_settings(s);});}
    rejects([]{validate_luminance_range_settings({{0,1,0,1},false});});
    for (double v:{nan,0.,0x1p-9,65537.}) {ColorRangeSettings s;s.scales[1]=v;rejects([&]{validate_color_range_settings(s);});}
    for (double v:{nan,-1.,2.}) {ColorRangeSettings s;s.inner=v;rejects([&]{validate_color_range_settings(s);});}
    ColorRangeSettings s;s.outer=0;rejects([&]{validate_color_range_settings(s);});
    rejects([]{evaluate_luminance_range_mask({0,0,0},ImageDescriptor{},{});});
    rejects([]{evaluate_color_range_mask({0,0,0},ImageDescriptor{},{});});
    for (float v:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        rejects([&]{evaluate_luminance_range_mask({0,v,0},ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50),{});});
        rejects([&]{evaluate_color_range_mask({v,0,0},ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50),{});});
        rejects([&]{evaluate_depth_range_mask(v,{});});
    }
    for (int kind:{0,1,2}) {
        auto mask=std::make_shared<ScalarSource>(Rect{0,0,2,1},std::vector<float>{0,0});
        auto rgb=std::make_shared<RgbSource>(mask->bounds,std::vector<float>(6),ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50));
        auto depth=std::make_shared<ScalarSource>(mask->bounds,std::vector<float>{0,0});
        Settings settings{};settings.kind=kind;settings.controls=kind==1?std::array<std::uint64_t,8>{0,0,0,0x3ff0000000000000,0x3ff0000000000000,0x3ff0000000000000,0,0x3ff0000000000000}:
            std::array<std::uint64_t,8>{0,0,0x3ff0000000000000,0x3ff0000000000000,0,0,0,0};
        auto output=node(settings,mask,rgb,depth);
        for (int damage:{1,2,3,4}) {
            if (kind==2) depth->damage=damage;else rgb->damage=damage;
            rejects([&]{output->render(mask->bounds);});depth->damage=rgb->damage=0;
            mask->damage=damage;rejects([&]{output->render(mask->bounds);});mask->damage=0;
        }
        rejects([&]{CoverageDepthRangeNode missing(mask,nullptr,{});});
        rejects([&]{CoverageColorRangeNode missing(nullptr,rgb,mask->bounds,{});});
        rejects([&]{CoverageLuminanceRangeNode wrong(mask,rgb,{0,0,1,1},{});});
    }
    // Caller settings are copied before subsequent mutation.
    auto mask=std::make_shared<ScalarSource>(Rect{0,0,1,1},std::vector<float>{1});
    auto depth=std::make_shared<ScalarSource>(mask->bounds,std::vector<float>{.5f});
    auto rgb=std::make_shared<RgbSource>(mask->bounds,std::vector<float>{.5f,.5f,.5f},ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50));
    ScalarRangeSettings settings;CoverageLuminanceRangeNode l(mask,rgb,mask->bounds,settings);CoverageDepthRangeNode d(mask,depth,settings);
    settings.edges={0,0,0,0};require(l.render(mask->bounds).coverage[0]==1 && d.render(mask->bounds).coverage[0]==1,"scalar settings not copied");
    ColorRangeSettings cs;CoverageColorRangeNode c(mask,rgb,mask->bounds,cs);cs.center={65536,65536,65536};
    require(c.render(mask->bounds).coverage[0]==.25f,"color settings not copied");
    auto empty=std::make_shared<ScalarSource>(Rect{0,0,0,1},std::vector<float>{});
    rejects([&]{CoverageDepthRangeNode bad(empty,empty,{});});
    auto overflow=std::make_shared<ScalarSource>(Rect{1,0,UINT32_MAX,1},std::vector<float>{});
    rejects([&]{CoverageDepthRangeNode bad(overflow,overflow,{});});
    auto huge=std::make_shared<ScalarSource>(Rect{0,0,UINT32_MAX,UINT32_MAX},std::vector<float>{});
    CoverageDepthRangeNode huge_depth(huge,huge,{});
    rejects([&]{huge_depth.render(huge->bounds);});
    require(huge->request.width==0,"capacity guard sampled upstream");
    auto huge_rgb=std::make_shared<RgbSource>(huge->bounds,std::vector<float>{},ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50));
    CoverageColorRangeNode huge_color(huge,huge_rgb,huge->bounds,{});
    rejects([&]{huge_color.render(huge->bounds);});
    require(huge_rgb->request.width==0 && huge->request.width==0,"RGB capacity guard sampled upstream");
}
std::string id(unsigned n) {
    char buffer[48];std::snprintf(buffer,sizeof(buffer),"a3000000-0000-0000-0000-%012u",n);return buffer;
}
EditValue numbers(std::span<const std::uint64_t> words) {
    EditValue::Array result;for (auto word:words) result.push_back(EditValue{number(word)});return EditValue{std::move(result)};
}
EditOperation operation(const Settings& s) {
    EditOperation op;op.id=id(3);op.processing_version=2;op.input_domain=op.output_domain=EditDomain::Coverage;
    op.type_id=s.kind==0?"rawengine.mask_luminance_range":s.kind==1?"rawengine.mask_color_range":"rawengine.mask_depth_range";
    op.inputs={{"mask",id(1)},{s.kind==2?"depth":"image",id(2)}};
    op.parameters={{"invert",EditValue{s.invert}}};
    if (s.kind!=1) op.parameters.emplace("edges",numbers(std::span(s.controls).first<4>()));
    else {
        op.parameters.emplace("center",numbers(std::span(s.controls).first<3>()));
        op.parameters.emplace("scales",numbers(std::span(s.controls).subspan<3,3>()));
        op.parameters.emplace("inner",EditValue{number(s.controls[6])});op.parameters.emplace("outer",EditValue{number(s.controls[7])});
    }
    return op;
}
struct GraphFixture {
    EditManifest manifest;std::vector<BoundEditSource> bindings;Rect bounds;
    explicit GraphFixture(const range_mask_reference::Frame& frame) {
        const bool depth=frame.settings.kind==2;
        bounds={depth?frame.x:0,depth?frame.y:0,frame.width,frame.height};
        manifest.format_version=4;manifest.working_space=frame.settings.space?WorkingSpace::LinearRec2020D65:WorkingSpace::LinearProPhotoD50;
        auto mask=std::make_shared<CoverageRasterNode>(CoverageImage({bounds,0},values(frame.input)));
        EditSource source;source.id=id(1);source.kind=EditSourceKind::CoverageRasterF32;source.content_sha256=*mask->source_fingerprint();
        manifest.sources.push_back(source);bindings.push_back({source,nullptr,bounds,mask});
        source.id=id(2);
        if (depth) {
            auto guide=std::make_shared<CoverageRasterNode>(CoverageImage({bounds,0},values(frame.guide)));
            source.content_sha256=*guide->source_fingerprint();manifest.sources.push_back(source);bindings.push_back({source,nullptr,bounds,guide});
        } else {
            auto guide=std::make_shared<RasterSourceNode>(RasterImage({bounds.width,bounds.height,0,manifest.working_space},values(frame.guide)));
            source.kind=EditSourceKind::SceneLinearRasterF32;source.working_space=manifest.working_space;source.content_sha256=*guide->source_fingerprint();
            manifest.sources.push_back(source);bindings.push_back({source,guide,bounds});
        }
        manifest.operations={operation(frame.settings)};manifest.output_id=id(3);
    }
};
bool exact_graph(std::span<const float> actual,std::span<const std::uint32_t> expected) {
    return actual.size()==expected.size() && std::equal(actual.begin(),actual.end(),expected.begin(),[](float value,std::uint32_t word) {
        return std::bit_cast<std::uint32_t>(value)==(word==0x80000000u?0:word);
    });
}
void saved_graphs() {
    for (const auto& frame:range_mask_reference::frames) {
        GraphFixture fixture(frame);auto cache=std::make_shared<TileCache>(1024*1024);
        ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(fixture.manifest)),fixture.bindings,nullptr,cache);
        rejects([&]{graph.source_bounds();});rejects([&]{graph.required_source_region(fixture.bounds);});
        for (auto level:{RenderLevel{},RenderLevel{0,RenderQuality::Preview},RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            const auto bounds=graph.coverage_output().output_bounds(level);auto expected=level.mip==0?frame.native:level.mip==1?frame.mip1:frame.mip2;
            for (unsigned tile:{1u,2u,8u}) {
                auto result=CoverageRenderer{}.render_image(graph.coverage_output(),RenderRequest{bounds,tile,level});
                require(exact_graph(result.coverage,expected),"saved graph frozen frame differs");
            }
            for (std::uint32_t y=0;y<bounds.height;++y) for (std::uint32_t x=0;x<bounds.width;++x) {
                Rect roi{bounds.x+x,bounds.y+y,1,1};const auto regions=graph.required_source_regions(roi,level);
                const auto scale=1u<<level.mip;
                Rect required{fixture.bounds.x+x*scale,fixture.bounds.y+y*scale,std::min(scale,fixture.bounds.width-x*scale),std::min(scale,fixture.bounds.height-y*scale)};
                require(regions.size()==2 && same(regions.at(id(1)),required) && same(regions.at(id(2)),required),"saved mask/guide source-ID footprint differs");
            }
        }
        auto disabled=fixture.manifest;disabled.operations[0].enabled=false;
        ExecutableEditGraph bypass(disabled,fixture.bindings,nullptr,cache);
        require(bypass.required_source_regions(fixture.bounds).size()==1,"disabled guide sampled");
        require(same(bypass.source_bounds(),fixture.bounds),"disabled guide contributes source extent");
        auto bad=fixture.manifest;
        for (bool enabled:{false,true}) {
            bad.operations[0].enabled=enabled;bad.operations[0].inputs={{"mask",id(1)}};
            rejects([&]{ExecutableEditGraph invalid(bad,fixture.bindings);});
            bad=fixture.manifest;bad.operations[0].enabled=enabled;bad.operations[0].parameters["invert"]=EditValue{std::int64_t(1)};
            rejects([&]{ExecutableEditGraph invalid(bad,fixture.bindings);});
        }
        const auto original=graph.coverage_output().render(fixture.bounds);const auto hits=cache->stats();
        graph.coverage_output().render(fixture.bounds);require(cache->stats().hits>hits.hits,"range output not cached");
        auto changed=fixture.manifest;changed.operations[0].parameters["invert"]=EditValue{!frame.settings.invert};
        EditHistory history(fixture.manifest,fixture.bindings,{8,1024*1024},nullptr,cache);
        const auto revision=history.commit(changed);require(history.undo()==1 && history.redo()==revision,"range history navigation");
        auto restored=EditHistory::restore(history.serialize(),fixture.bindings,nullptr,cache);
        require(restored->serialize()==history.serialize(),"range history exact restore");
        TileScheduler scheduler(2,8);auto snapshot=history.revision(1);
        auto job=scheduler.submit(snapshot->graph->coverage_output_handle(),RenderRequest{fixture.bounds,2,{}});
        require(exact_graph(job.get().coverage,frame.native),"pinned range job differs");
    }
    for (unsigned kind=0;kind<3;++kind) {
        GraphFixture fixture(range_mask_reference::frames[kind==0?0:kind==1?2:4]);
        const char* canonical=kind==0?range_mask_reference::operation0_json:kind==1?range_mask_reference::operation1_json:range_mask_reference::operation2_json;
        auto wrapped=serialize_edit_manifest(fixture.manifest);const auto position=wrapped.find("\"operations\":[");
        const auto end=wrapped.find("],\"output\"",position);require(position!=std::string::npos && end!=std::string::npos,"canonical wrapper fields");
        wrapped.replace(position+14,end-(position+14),canonical);
        require(serialize_edit_manifest(parse_edit_manifest(wrapped)).find(canonical)!=std::string::npos,"canonical range bytes changed");
    }
}
struct Gate {
    std::promise<void> entered,release;std::shared_future<void> released=release.get_future().share();std::atomic<bool> opened=false;
    void open() {if (!opened.exchange(true)) release.set_value();}
};
struct ReleaseOnExit {std::shared_ptr<Gate> gate;~ReleaseOnExit() {gate->open();}};
class GatedRgb final : public Node {
public:
    GatedRgb(std::shared_ptr<const Node> input,std::shared_ptr<Gate> gate,bool fail):input_(std::move(input)),gate_(std::move(gate)),fail_(fail) {}
    Tile render(Rect roi) const override {return render_level(roi,{});}
    Tile render_level(Rect roi,RenderLevel level) const override {
        if (visits_.fetch_add(1)==0) {gate_->entered.set_value();gate_->released.get();if (fail_) throw std::bad_alloc();}
        return input_->render_level(roi,level);
    }
    bool supports_level(RenderLevel level) const noexcept override {return input_->supports_level(level);}
    ImageDescriptor output_descriptor() const noexcept override {return input_->output_descriptor();}
    std::optional<std::array<std::uint8_t,32>> source_fingerprint() const override {return input_->source_fingerprint();}
    std::optional<Rect> source_bounds() const override {return input_->source_bounds();}
private:
    std::shared_ptr<const Node> input_;std::shared_ptr<Gate> gate_;bool fail_;mutable std::atomic<unsigned> visits_=0;
};
class GatedDepth final : public CoverageNode {
public:
    GatedDepth(std::shared_ptr<const CoverageNode> input,std::shared_ptr<Gate> gate,bool fail):input_(std::move(input)),gate_(std::move(gate)),fail_(fail) {}
    CoverageTile render_level(Rect roi,RenderLevel level) const override {
        if (visits_.fetch_add(1)==0) {gate_->entered.set_value();gate_->released.get();if (fail_) throw std::bad_alloc();}
        return input_->render_level(roi,level);
    }
    bool supports_level(RenderLevel level) const noexcept override {return input_->supports_level(level);}
    Rect native_bounds() const noexcept override {return input_->native_bounds();}
    Rect required_native_region(Rect roi,RenderLevel level) const override {return input_->required_native_region(roi,level);}
    std::optional<std::array<std::uint8_t,32>> source_fingerprint() const override {return input_->source_fingerprint();}
private:
    std::shared_ptr<const CoverageNode> input_;std::shared_ptr<Gate> gate_;bool fail_;mutable std::atomic<unsigned> visits_=0;
};
void gated_guide(GraphFixture& f,std::shared_ptr<Gate> gate,bool fail=false) {
    if (f.bindings[1].coverage_node) f.bindings[1].coverage_node=std::make_shared<GatedDepth>(f.bindings[1].coverage_node,gate,fail);
    else f.bindings[1].node=std::make_shared<GatedRgb>(f.bindings[1].node,gate,fail);
}
void entered(std::future<void>& future) {
    require(future.wait_for(std::chrono::seconds(10))==std::future_status::ready,"range guide gate timed out");future.get();
}
void generation_recovery_and_budget() {
    for (unsigned index:{0u,2u,4u}) {
        const auto& frame=range_mask_reference::frames[index];
        {
            GraphFixture f(frame);auto gate=std::make_shared<Gate>();auto started=gate->entered.get_future();gated_guide(f,gate);
            auto cache=std::make_shared<TileCache>(1024*1024);ExecutableEditGraph graph(f.manifest,f.bindings,nullptr,cache);
            auto pending=std::async(std::launch::async,[&]{return graph.coverage_output().render(f.bounds);});ReleaseOnExit release{gate};
            entered(started);cache->clear();gate->open();
            require(exact_graph(pending.get().coverage,frame.native),"range in-flight clear changed pixels");
            require(cache->stats().entries==0,"old range/guide cache generation repopulated cleared cache");
            require(exact_graph(graph.coverage_output().render(f.bounds).coverage,frame.native),"range new generation failed");
            require(cache->stats().entries>0,"new range/guide generation not cached");
        }
        for (bool fail:{false,true}) {
            GraphFixture f(frame);auto gate=std::make_shared<Gate>();auto started=gate->entered.get_future();gated_guide(f,gate,fail);
            auto graph=std::make_shared<ExecutableEditGraph>(f.manifest,f.bindings);TileScheduler scheduler(1,2);ReleaseOnExit release{gate};
            const RenderRequest request{f.bounds,2,{}};auto output=graph->coverage_output_handle();
            auto first=fail?scheduler.submit(output,request):scheduler.submit_latest("range",output,request);
            entered(started);auto next=fail?scheduler.submit(output,request):scheduler.submit_latest("range",output,request);gate->open();
            bool expected=false;
            try {first.get();} catch (const std::bad_alloc&) {expected=fail;} catch (const RenderCancelled&) {expected=!fail;}
            require(expected,"range guide source fault/supersession has wrong future result");
            require(exact_graph(next.get().coverage,frame.native),"queued range recovery pixels changed");
            require(exact_graph(scheduler.submit(output,request).get().coverage,frame.native),"later range recovery pixels changed");
        }
        GraphFixture f(frame);auto tiny=std::make_shared<TileCache>(1);ExecutableEditGraph graph(f.manifest,f.bindings,nullptr,tiny);
        require(exact_graph(graph.coverage_output().render(f.bounds).coverage,frame.native),"range tiny budget changed pixels");
        require(tiny->stats().entries==0,"oversized range/guide admitted to tiny budget");
        TileScheduler scheduler(1,2);auto token=std::make_shared<CancellationToken>();token->cancel();
        auto cancelled=scheduler.submit(graph.coverage_output_handle(),RenderRequest{f.bounds,2,{}},RenderPriority::Normal,token);
        bool observed=false;try {cancelled.get();} catch (const RenderCancelled&) {observed=true;}
        require(observed,"range cancelled job rendered");
        require(exact_graph(scheduler.submit(graph.coverage_output_handle(),RenderRequest{f.bounds,2,{}}).get().coverage,frame.native),"range cancellation recovery");
    }
}
}
int main() {
    try {scalar_cases();frames();guards();saved_graphs();generation_recovery_and_budget();std::cout<<"12236 frozen range selections and actual node products / 30 frames with exhaustive ROI and saved graphs passed\n";return 0;}
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
