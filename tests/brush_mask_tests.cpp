#include "BrushMask.hpp"
#include "CoverageDelivery.hpp"
#include "EditGraph.hpp"
#include "TileScheduler.hpp"
#include "reference/brush_mask_v1.hpp"

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

void require(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
bool native_final(RenderLevel level) {return level.mip==0 && level.quality==RenderQuality::Final;}
template<class Exception=std::invalid_argument,class Callback> void rejects(Callback action) {
    try {action();} catch (const Exception&) {return;}
    throw std::runtime_error("expected brush rejection did not occur");
}
BrushMaskSettings decode(std::span<const brush_mask_reference::Stroke> strokes) {
    BrushMaskSettings settings;
    for (const auto& row:strokes) {
        BrushStroke stroke;
        stroke.mode=row.mode==0?BrushMode::Paint:BrushMode::Erase;
        stroke.radius=std::bit_cast<double>(row.radius);stroke.hardness=std::bit_cast<double>(row.hardness);
        stroke.flow=std::bit_cast<double>(row.flow);stroke.opacity=std::bit_cast<double>(row.opacity);
        for (const auto& point:row.points)
            stroke.points.push_back({std::bit_cast<double>(point.x),std::bit_cast<double>(point.y),
                                     std::bit_cast<double>(point.pressure)});
        settings.strokes.push_back(std::move(stroke));
    }
    return settings;
}
std::vector<float> values(std::span<const std::uint32_t> words) {
    std::vector<float> output;for (auto word:words) output.push_back(std::bit_cast<float>(word));return output;
}
bool exact(const std::vector<float>& output,std::span<const std::uint32_t> words) {
    return output.size()==words.size() && std::equal(output.begin(),output.end(),words.begin(),
        [](float value,std::uint32_t word) {return std::bit_cast<std::uint32_t>(value)==word;});
}
std::shared_ptr<const CoverageNode> source(Rect bounds={0,0,7,5}) {
    return std::make_shared<CoverageRasterNode>(CoverageImage({bounds,0},std::vector<float>(
        static_cast<std::size_t>(bounds.width)*bounds.height,0.25f)));
}
BrushMaskSettings example() {
    BrushStroke stroke;stroke.radius=2;stroke.hardness=0.5;stroke.flow=0.75;
    stroke.points={{0.5,0.5,1},{4.5,2.5,0.5}};return {{stroke}};
}

std::string id(unsigned value) {
    char tail[16];std::snprintf(tail,sizeof(tail),"%012u",value);
    return std::string("00000000-0000-4000-8000-")+tail;
}
EditValue::Object parameters(const BrushMaskSettings& settings) {
    EditValue::Array strokes;
    for (const auto& stroke:settings.strokes) {
        EditValue::Array points;
        for (const auto& point:stroke.points)
            points.push_back(EditValue{EditValue::Array{EditValue{point.x},EditValue{point.y},EditValue{point.pressure}}});
        strokes.push_back(EditValue{EditValue::Object{
            {"mode",EditValue{std::string(stroke.mode==BrushMode::Paint?"paint":"erase")}},
            {"radius",EditValue{stroke.radius}},{"hardness",EditValue{stroke.hardness}},
            {"flow",EditValue{stroke.flow}},{"opacity",EditValue{stroke.opacity}},
            {"points",EditValue{std::move(points)}}}});
    }
    return {{"strokes",EditValue{std::move(strokes)}}};
}
struct GraphFixture {
    EditManifest manifest;
    std::vector<BoundEditSource> bindings;
    explicit GraphFixture(const brush_mask_reference::Frame& row=brush_mask_reference::frames[0]) {
        const Rect bounds{row.x,row.y,row.width,row.height};
        auto input=std::make_shared<CoverageRasterNode>(CoverageImage({bounds,0},values(row.input)));
        EditSource identity;identity.id=id(1);identity.kind=EditSourceKind::CoverageRasterF32;
        identity.content_sha256=*input->source_fingerprint();
        manifest.format_version=4;manifest.sources={identity};manifest.output_id=id(2);
        EditOperation brush;brush.id=id(2);brush.type_id="rawengine.mask_brush";brush.processing_version=2;
        brush.input_domain=brush.output_domain=EditDomain::Coverage;brush.inputs={{"mask",id(1)}};
        brush.parameters=parameters(decode(row.strokes));manifest.operations={brush};
        bindings={{identity,nullptr,bounds,input}};
    }
};
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}

void graph_replay() {
    for (const auto& row:brush_mask_reference::frames) {
        GraphFixture f(row);const auto json=serialize_edit_manifest(f.manifest);
        require(serialize_edit_manifest(parse_edit_manifest(json))==json,"brush saved JSON round trip changed");
        ExecutableEditGraph graph(parse_edit_manifest(json),f.bindings);
        require(graph.output_is_coverage(),"brush graph lost scalar output type");
        rejects([&] {graph.output_handle();});
        for (unsigned mip=0;mip<=2;++mip) {
            const RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
            const auto bounds=graph.coverage_output().output_bounds(level);
            const auto expected=mip==0?row.native:mip==1?row.mip1:row.mip2;
            require(exact(graph.coverage_output().render_level(bounds,level).coverage,expected),"saved brush graph differs from frozen frame");
            for (unsigned y=0;y<bounds.height;++y) for (unsigned x=0;x<bounds.width;++x) {
                const Rect roi{bounds.x+x,bounds.y+y,1,1};
                const auto regions=graph.required_source_regions(roi,level);
                const CoverageBrushNode direct(f.bindings[0].coverage_node,decode(row.strokes));
                require(regions.size()==1 && same(regions.at(id(1)),direct.input_region_level(roi,f.bindings[0].bounds,level)),
                        "saved brush source-ID planning lost native painted footprint");
            }
        }
    }
    GraphFixture canonical;
    auto json=serialize_edit_manifest(canonical.manifest);
    const auto prefix=json.find("\"operations\":[");
    require(prefix!=std::string::npos,"cannot locate canonical operation array");
    const auto start=prefix+14;
    const auto end=json.find("],\"output\"",start);
    require(end!=std::string::npos,"cannot locate canonical operation");
    json.replace(start,end-start,brush_mask_reference::operation_json);
    require(serialize_edit_manifest(parse_edit_manifest(json)).find(brush_mask_reference::operation_json)!=std::string::npos,
            "saved brush operation changed independent canonical bytes");

    // A brush forces upstream algebra to native before reducing every source ID.
    GraphFixture f;auto second=f.bindings[0];second.identity.id=id(3);
    f.manifest.sources.push_back(second.identity);f.bindings.push_back(second);
    EditOperation combine;combine.id=id(4);combine.type_id="rawengine.mask_combine";combine.processing_version=2;
    combine.input_domain=combine.output_domain=EditDomain::Coverage;combine.inputs={{"base",id(1)},{"layer",id(3)}};
    combine.parameters={{"mode",EditValue{std::string("intersect")}}};
    f.manifest.operations.insert(f.manifest.operations.begin(),combine);
    f.manifest.operations.back().inputs={{"mask",id(4)}};
    ExecutableEditGraph composed(f.manifest,f.bindings);
    const auto regions=composed.required_source_regions({1,0,1,1},{2,RenderQuality::Preview});
    require(regions.size()==2 && same(regions.at(id(1)),Rect{4,0,3,4}) && same(regions.at(id(3)),Rect{4,0,3,4}),
            "brush upstream shared mask source-ID footprint failed");
    f.manifest.operations.back().enabled=false;
    ExecutableEditGraph disabled(f.manifest,f.bindings);
    auto direct=CoverageCombineNode(f.bindings[0].coverage_node,f.bindings[1].coverage_node,MaskCombineMode::Intersect);
    require(disabled.coverage_output().render_level({0,0,2,2},{2,RenderQuality::Preview}).coverage==
            direct.render_level({0,0,2,2},{2,RenderQuality::Preview}).coverage,"disabled brush changed upstream level semantics");
}

void graph_guards() {
    GraphFixture original;
    auto reject=[&](EditManifest manifest) {rejects<std::exception>([&] {ExecutableEditGraph graph(manifest,original.bindings);});};
    for (bool enabled:{true,false}) {
        auto base=original.manifest;base.operations[0].enabled=enabled;
        for (unsigned variant=0;variant<12;++variant) {
            auto changed=base;auto& op=changed.operations[0];
            if (variant==0) op.inputs={{"image",id(1)}};
            if (variant==1) op.masks={{"coverage",id(1)}};
            if (variant==2) op.opacity=0.5;
            if (variant==3) op.blend_mode="multiply";
            if (variant==4) op.extra_fields={{"unknown",EditValue{true}}};
            if (variant==5) op.schema_version=2;
            if (variant==6) op.processing_version=1;
            if (variant==7) op.input_domain=EditDomain::SceneLinearProPhotoD50;
            if (variant==8) op.output_domain=EditDomain::SceneLinearProPhotoD50;
            if (variant==9) op.parameters={{"strokes",EditValue{false}}};
            if (variant==10) op.parameters.emplace("extra",EditValue{0.0});
            if (variant==11) op.inputs={{"mask",id(2)}};
            reject(changed);
        }
        for (unsigned variant=0;variant<10;++variant) {
            auto changed=base;
            auto& array=std::get<EditValue::Array>(changed.operations[0].parameters.at("strokes").data);
            auto& stroke=std::get<EditValue::Object>(array[0].data);
            if (variant==0) stroke.at("mode")=EditValue{std::string("add")};
            if (variant==1) stroke.at("radius")=EditValue{true};
            if (variant==2) stroke.at("flow")=EditValue{std::string("1")};
            if (variant==3) stroke.at("opacity")=EditValue{nullptr};
            if (variant==4) stroke.emplace("extra",EditValue{0.0});
            if (variant==5) stroke.erase("hardness");
            if (variant==6) stroke.at("points")=EditValue{EditValue::Array{}};
            if (variant==7) stroke.at("points")=EditValue{EditValue::Array{EditValue{false}}};
            if (variant==8) stroke.at("points")=EditValue{EditValue::Array{EditValue{EditValue::Array{EditValue{0.0},EditValue{0.0}}}}};
            if (variant==9) stroke.at("points")=EditValue{EditValue::Array{EditValue{EditValue::Array{EditValue{0.0},EditValue{0.0},EditValue{true}}}}};
            reject(changed);
        }
    }
    for (unsigned format:{1u,2u,3u}) {auto changed=original.manifest;changed.format_version=format;reject(changed);}
    auto bad_bindings=original.bindings;bad_bindings[0].bounds.x=1;
    rejects([&] {ExecutableEditGraph graph(original.manifest,bad_bindings);});
    bad_bindings=original.bindings;bad_bindings[0].identity.content_sha256[0]^=1;
    rejects([&] {ExecutableEditGraph graph(original.manifest,bad_bindings);});
}

void graph_cache_history_jobs() {
    GraphFixture f;auto cache=std::make_shared<TileCache>(1024*1024);
    ExecutableEditGraph first(f.manifest,f.bindings,nullptr,cache);
    const auto before=first.coverage_output().render({0,0,7,5}).coverage;
    auto stats=cache->stats();first.coverage_output().render({0,0,7,5});
    require(cache->stats().hits>stats.hits,"brush output did not reuse cached tile");
    auto changed=f.manifest;
    auto settings=decode(brush_mask_reference::frames[0].strokes);settings.strokes[0].flow=0;
    changed.operations[0].parameters=parameters(settings);
    ExecutableEditGraph second(changed,f.bindings,nullptr,cache);stats=cache->stats();
    const auto after=second.coverage_output().render({0,0,7,5}).coverage;
    require(before!=after && cache->stats().hits>stats.hits && cache->stats().misses>stats.misses,
            "changed brush reused stale output or failed upstream cache reuse");
    auto copied=first.coverage_output().render({0,0,7,5});copied.coverage[0]=0;
    require(first.coverage_output().render({0,0,7,5}).coverage==before,"brush cached return aliases caller storage");
    cache->clear();require(cache->stats().entries==0,"brush cache clear failed");
    require(first.coverage_output().render({0,0,7,5}).coverage==before,"brush cache clear changed output");
    auto no_cache=std::make_shared<TileCache>(1);ExecutableEditGraph uncached(f.manifest,f.bindings,nullptr,no_cache);
    uncached.coverage_output().render({0,0,7,5});require(no_cache->stats().entries==0,"brush exceeded shared cache budget");
    EditHistory history(f.manifest,f.bindings,{8,1024*1024},nullptr,cache);
    const auto pinned=history.current();history.commit(changed);const auto changed_id=history.current()->id;
    require(history.current()->graph->coverage_output().render({0,0,7,5}).coverage==after,"brush history commit changed output");
    history.undo();require(history.current()->graph->coverage_output().render({0,0,7,5}).coverage==before,"brush undo failed");
    history.redo();require(history.current()->id==changed_id,"brush redo failed");
    auto restored=EditHistory::restore(history.serialize(),f.bindings,nullptr,cache);
    require(restored->current()->graph->coverage_output().render({0,0,7,5}).coverage==after,"brush history restore failed");
    auto invalid=changed;invalid.operations[0].parameters={{"strokes",EditValue{false}}};
    rejects([&] {history.commit(invalid);});require(history.current()->id==changed_id,"invalid brush commit published history state");
    TileScheduler scheduler(2,8);
    auto future=scheduler.submit(pinned->graph->coverage_output_handle(),RenderRequest{{0,0,7,5},1,{}});
    f.bindings.clear();f.manifest.operations.clear();
    require(future.get().coverage==before,"brush job lost pinned source/stroke lifetime");
    auto cancelled=std::make_shared<CancellationToken>();cancelled->cancel();
    auto failed=scheduler.submit(second.coverage_output_handle(),RenderRequest{{0,0,7,5},1,{}},RenderPriority::Normal,cancelled);
    rejects<RenderCancelled>([&] {failed.get();});
    require(scheduler.submit(second.coverage_output_handle(),RenderRequest{{0,0,7,5},2,{}}).get().coverage==after,
            "brush job did not recover after cancellation");
}

struct Gate {
    std::promise<void> entered,release;
    std::shared_future<void> released=release.get_future().share();
    std::atomic<bool> opened=false;
    void open() {if (!opened.exchange(true)) release.set_value();}
};
struct ReleaseOnExit {
    std::shared_ptr<Gate> gate;
    ~ReleaseOnExit() {gate->open();}
};
class GatedInput final:public CoverageNode {
public:
    GatedInput(std::shared_ptr<const CoverageNode> input,std::shared_ptr<Gate> gate,bool fail=false)
        :input_(std::move(input)),gate_(std::move(gate)),fail_(fail) {}
    CoverageTile render_level(Rect roi,RenderLevel level) const override {
        if (visits_.fetch_add(1)==0) {
            gate_->entered.set_value();gate_->released.get();
            if (fail_) throw std::bad_alloc();
        }
        return input_->render_level(roi,level);
    }
    bool supports_level(RenderLevel level) const noexcept override {return input_->supports_level(level);}
    Rect native_bounds() const noexcept override {return input_->native_bounds();}
    std::optional<std::array<std::uint8_t,32>> source_fingerprint() const override {return input_->source_fingerprint();}
    Rect required_native_region(Rect roi,RenderLevel level) const override {return input_->required_native_region(roi,level);}
private:
    std::shared_ptr<const CoverageNode> input_;
    std::shared_ptr<Gate> gate_;
    bool fail_;
    mutable std::atomic<unsigned> visits_=0;
};
void entered(std::future<void>& future) {
    require(future.wait_for(std::chrono::seconds(10))==std::future_status::ready,"brush source gate timed out");
    future.get();
}
void generation_and_job_recovery() {
    {
        GraphFixture f;auto cache=std::make_shared<TileCache>(1024*1024);
        auto gate=std::make_shared<Gate>();auto started=gate->entered.get_future();
        f.bindings[0].coverage_node=std::make_shared<GatedInput>(f.bindings[0].coverage_node,gate);
        ExecutableEditGraph graph(f.manifest,f.bindings,nullptr,cache);
        auto pending=std::async(std::launch::async,[&] {return graph.coverage_output().render({0,0,7,5});});
        ReleaseOnExit release{gate};
        entered(started);cache->clear();gate->open();
        require(exact(pending.get().coverage,brush_mask_reference::frames[0].native),"in-flight brush clear changed output");
        require(cache->stats().entries==0,"old generation brush/source work repopulated cleared cache");
        require(exact(graph.coverage_output().render({0,0,7,5}).coverage,brush_mask_reference::frames[0].native),"brush post-clear recovery changed output");
        require(cache->stats().entries>0,"new generation brush failed to populate cache");
    }
    for (bool fail:{true,false}) {
        auto gate=std::make_shared<Gate>();auto started=gate->entered.get_future();
        TileScheduler scheduler(1,2);ReleaseOnExit release{gate};
        auto input=std::make_shared<GatedInput>(source(),gate,fail);
        auto brush=std::make_shared<CoverageBrushNode>(input,example());
        const auto expected=CoverageBrushNode(source(),example()).render({0,0,7,5}).coverage;
        const RenderRequest request{{0,0,7,5},2,{}};
        auto first=fail?scheduler.submit(brush,request):scheduler.submit_latest("paint",brush,request);
        entered(started);
        auto next=fail?scheduler.submit(brush,request):scheduler.submit_latest("paint",brush,request);
        gate->open();
        if (fail) rejects<std::bad_alloc>([&] {first.get();});
        else rejects<RenderCancelled>([&] {first.get();});
        require(next.get().coverage==expected,"queued brush recovery/supersession changed immutable output");
        require(scheduler.submit(brush,request).get().coverage==expected,"later brush job failed after source fault/replacement");
    }
}

void scalar_cases() {
    for (const auto& row:brush_mask_reference::cases) {
        const auto output=apply_brush_mask(std::bit_cast<float>(row.base),std::bit_cast<double>(row.x),
                                          std::bit_cast<double>(row.y),decode(row.strokes));
        require(std::bit_cast<std::uint32_t>(output)==row.output,"brush primitive differs from independent staged bits");
    }
}

void frames() {
    CoverageRenderer renderer;
    for (const auto& row:brush_mask_reference::frames) {
        const Rect native{row.x,row.y,row.width,row.height};
        const auto input=std::make_shared<CoverageRasterNode>(CoverageImage({native,0},values(row.input)));
        CoverageBrushNode node(input,decode(row.strokes));
        for (const RenderLevel level:{RenderLevel{},RenderLevel{0,RenderQuality::Preview},
                                    RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            const auto bounds=node.output_bounds(level);
            const auto words=level.mip==0?row.native:level.mip==1?row.mip1:row.mip2;
            for (auto size:{1u,2u,8u}) {
                const auto output=renderer.render_image(node,RenderRequest{bounds,size,level});
                require(exact(output.coverage,words),"brush full/partition/native-preview/mip differs from frozen frame");
            }
            require(native_final(node.input_level(level)),"brush did not request native Final input");
            for (std::uint32_t y=0;y<bounds.height;++y) {
                for (std::uint32_t x=0;x<bounds.width;++x) {
                    const Rect roi{bounds.x+x,bounds.y+y,1,1};
                    const auto output=node.render_level(roi,level);
                    require(std::bit_cast<std::uint32_t>(output.coverage[0])==words[y*bounds.width+x],"brush pixel ROI changed frozen output");
                    const auto footprint=node.input_region_level(roi,native,level);
                    const auto scale=1u<<level.mip;
                    const auto nx=level.mip?native.x+x*scale:roi.x,ny=level.mip?native.y+y*scale:roi.y;
                    const auto width=level.mip?std::min(scale,native.width-x*scale):1u;
                    const auto height=level.mip?std::min(scale,native.height-y*scale):1u;
                    require(footprint.x==nx && footprint.y==ny && footprint.width==width && footprint.height==height,
                            "brush native block footprint differs from true edge support");
                }
            }
            // Arbitrary rectangular ROI partitions exercise native block origins.
            for (std::uint32_t y=0;y<bounds.height;++y) for (std::uint32_t x=0;x<bounds.width;++x)
                for (std::uint32_t h=1;h<=bounds.height-y;++h) for (std::uint32_t w=1;w<=bounds.width-x;++w) {
                    const auto output=node.render_level({bounds.x+x,bounds.y+y,w,h},level);
                    for (std::uint32_t v=0;v<h;++v) for (std::uint32_t u=0;u<w;++u)
                        require(std::bit_cast<std::uint32_t>(output.coverage[v*w+u])==words[(y+v)*bounds.width+x+u],
                                "brush rectangular ROI changed frozen output");
                }
        }
    }
    auto settings=example();CoverageBrushNode owned(source(),settings);
    const auto before=owned.render({0,0,7,5}).coverage;
    settings.strokes[0].points[0].pressure=0;settings.strokes.clear();
    require(owned.render({0,0,7,5}).coverage==before,"brush node retained mutable caller settings");
    // Ordered native replay differs from upstream reduce-first mask algebra.
    auto a=std::make_shared<CoverageRasterNode>(CoverageImage({{0,0,3,1},0},{0,0,1}));
    auto b=std::make_shared<CoverageRasterNode>(CoverageImage({{0,0,3,1},0},{1,1,0}));
    auto combined=std::make_shared<CoverageCombineNode>(a,b,MaskCombineMode::Intersect);
    CoverageBrushNode empty(combined,{});
    require(empty.render_level({0,0,1,1},{2,RenderQuality::Preview}).coverage[0]==0,
            "empty enabled brush did not reduce native replay");
    require(combined->render_level({0,0,1,1},{2,RenderQuality::Preview}).coverage[0]>0,
            "native-before-reduction fixture did not distinguish upstream reduce-first");
}

class Fault final:public CoverageNode {
public:
    explicit Fault(int mode):mode_(mode) {}
    CoverageTile render_level(Rect roi,RenderLevel level) const override {
        require(native_final(level),"brush forwarded reduced/preview input level");
        ++calls;
        if (mode_==5) throw std::bad_alloc();
        CoverageTile tile{roi,std::vector<float>(static_cast<std::size_t>(roi.width)*roi.height,0.5f)};
        if (mode_==1) ++tile.bounds.x;
        if (mode_==2) tile.coverage.pop_back();
        if (mode_==3) tile.coverage.back()=std::numeric_limits<float>::quiet_NaN();
        if (mode_==4) tile.coverage.back()=-0.1f;
        return tile;
    }
    bool supports_level(RenderLevel level) const noexcept override {return native_final(level);}
    Rect native_bounds() const noexcept override {return mode_==6?Rect{0,0,0xffffffffu,0xffffffffu}:Rect{0,0,7,5};}
    mutable unsigned calls=0;
private:
    int mode_;
};

void guards() {
    const auto input=source();const auto settings=example();
    rejects([&] {CoverageBrushNode node(nullptr,settings);});
    for (double bad:{-1.0,0.0,0x1p-9,0x1p21,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto changed=settings;changed.strokes[0].radius=bad;rejects([&] {validate_brush_mask_settings(changed);});
    }
    for (double bad:{-0.1,1.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        for (unsigned field=0;field<4;++field) {
            auto changed=settings;auto& s=changed.strokes[0];
            if (field==0) s.hardness=bad;else if (field==1) s.flow=bad;else if (field==2) s.opacity=bad;else s.points[0].pressure=bad;
            rejects([&] {CoverageBrushNode node(input,changed);});
        }
    for (double bad:{-0x1p34,0x1p34,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto changed=settings;changed.strokes[0].points[0].x=bad;
        rejects([&] {validate_brush_mask_settings(changed);});
        rejects([&] {apply_brush_mask(0,bad,0,settings);});
        rejects([&] {apply_brush_mask(0,0,bad,settings);});
        changed=settings;changed.strokes[0].points[0].y=bad;rejects([&] {validate_brush_mask_settings(changed);});
    }
    auto changed=settings;changed.strokes[0].mode=static_cast<BrushMode>(99);rejects([&] {validate_brush_mask_settings(changed);});
    changed=settings;changed.strokes[0].flow=0;changed.strokes[0].points.clear();rejects([&] {validate_brush_mask_settings(changed);});
    changed.strokes[0].points.resize(65537);rejects([&] {validate_brush_mask_settings(changed);});
    changed=settings;changed.strokes.resize(4097,settings.strokes[0]);rejects([&] {validate_brush_mask_settings(changed);});
    changed=settings;changed.strokes[0].points.resize(65536);changed.strokes.resize(17,changed.strokes[0]);
    rejects([&] {validate_brush_mask_settings(changed);});
    changed.strokes.resize(16);validate_brush_mask_settings(changed);
    for (float bad:{-0.1f,1.1f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        rejects([&] {apply_brush_mask(bad,0,0,{});});
    CoverageBrushNode node(input,settings);
    for (RenderLevel level:{RenderLevel{3,RenderQuality::Preview},RenderLevel{1,RenderQuality::Final},
                           RenderLevel{0,static_cast<RenderQuality>(99)}}) {
        require(!node.supports_level(level),"brush admitted unsupported render level");
        rejects([&] {node.render_level({0,0,1,1},level);});
        rejects([&] {node.input_level(level);});
    }
    rejects([&] {node.render({7,0,1,1});});rejects([&] {node.render({0,0,0,1});});
    rejects([&] {node.input_region_level({0,0,1,1},{1,0,7,5},{});});
    for (int mode=1;mode<=4;++mode) {
        auto fault=std::make_shared<Fault>(mode);
        for (auto zero_settings:{BrushMaskSettings{},settings}) {
            if (!zero_settings.strokes.empty()) zero_settings.strokes[0].opacity=0;
            CoverageBrushNode damaged(fault,zero_settings);
            rejects<std::exception>([&] {damaged.render({0,0,7,5});});
            rejects<std::exception>([&] {damaged.render_level({0,0,2,2},{2,RenderQuality::Preview});});
        }
    }
    auto fault=std::make_shared<Fault>(5);CoverageBrushNode failed(fault,settings);
    rejects<std::bad_alloc>([&] {failed.render({0,0,7,5});});
    auto huge=std::make_shared<Fault>(6);CoverageBrushNode impossible(huge,{});
    rejects([&] {impossible.render(impossible.native_bounds());});
    require(huge->calls==0,"brush checked impossible allocation after input render");
    auto final_only=std::make_shared<Fault>(0);CoverageBrushNode preview(final_only,settings);
    require(preview.supports_level({2,RenderQuality::Preview}),"brush incorrectly requires upstream reduced support");
    preview.render_level({0,0,2,2},{2,RenderQuality::Preview});
    require(final_only->calls==1,"brush failed to render final-only upstream");
}

} // namespace
int main() {
    try {scalar_cases();frames();guards();graph_replay();graph_guards();graph_cache_history_jobs();generation_and_job_recovery();
        std::cout<<"426 independent brush scalar cases,12 native/mip frames,exhaustive rectangular ROIs,ownership/actual/settings/request guards passed\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
