#include "MaskRefinement.hpp"
#include "CoverageDelivery.hpp"
#include "EditGraph.hpp"
#include "TileScheduler.hpp"
#include "reference/mask_refinement_v1.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cfenv>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>

using namespace rawengine;
namespace {
using Frame=mask_refinement_reference::Frame;
void require(bool ok,const char* message) {if (!ok) throw std::runtime_error(message);}
template<class Call> void rejects(Call call) {
    try {call();} catch (const std::exception&) {return;}
    throw std::runtime_error("expected mask refinement rejection did not occur");
}
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}
float sample(std::uint32_t bits) {return std::bit_cast<float>(bits);}
double number(std::uint64_t bits) {return std::bit_cast<double>(bits);}
std::vector<float> values(std::span<const std::uint32_t> words) {
    std::vector<float> result;for (auto w:words) result.push_back(sample(w));return result;
}
struct Trace {
    mutable std::mutex mutex;mutable Rect last{};mutable RenderLevel level{};mutable std::atomic<unsigned> calls=0;
    void record(Rect r,RenderLevel l) const {std::lock_guard lock(mutex);last=r;level=l;++calls;}
    Rect request() const {std::lock_guard lock(mutex);require(level.mip==0 && level.quality==RenderQuality::Final,"input not native Final");return last;}
};
struct ScalarSource final : CoverageNode {
    Rect bounds;std::vector<float> data;int damage=0;bool supported=true;Trace trace;
    ScalarSource(Rect b,std::vector<float> v):bounds(b),data(std::move(v)) {}
    Rect native_bounds() const noexcept override {return bounds;}
    bool supports_level(RenderLevel l) const noexcept override {return supported && l.mip==0;}
    CoverageTile render_level(Rect r,RenderLevel l) const override {
        trace.record(r,l);CoverageTile out{r,{}};
        for (std::uint32_t y=0;y<r.height;++y) for (std::uint32_t x=0;x<r.width;++x)
            out.coverage.push_back(data.at(static_cast<std::size_t>(r.y-bounds.y+y)*bounds.width+r.x-bounds.x+x));
        if (damage==1) ++out.bounds.x;
        if (damage==2) out.coverage.pop_back();
        if (damage==3) out.coverage.back()=std::numeric_limits<float>::quiet_NaN();
        if (damage==4) out.coverage.back()=-1;
        if (damage==5) out.coverage.push_back(0);
        return out;
    }
};
struct RgbSource final : Node {
    Rect bounds;std::vector<float> data;ImageDescriptor description;int damage=0;bool supported=true;Trace trace;
    RgbSource(Rect b,std::vector<float> v,ImageDescriptor d):bounds(b),data(std::move(v)),description(d) {}
    ImageDescriptor output_descriptor() const noexcept override {return description;}
    bool supports_level(RenderLevel l) const noexcept override {return supported && l.mip==0;}
    Tile render(Rect r) const override {return render_level(r,{});}
    Tile render_level(Rect r,RenderLevel l) const override {
        trace.record(r,l);Tile out{r,{},description};
        for (std::uint32_t y=0;y<r.height;++y) for (std::uint32_t x=0;x<r.width;++x) {
            const auto i=(static_cast<std::size_t>(r.y-bounds.y+y)*bounds.width+r.x-bounds.x+x)*3;
            for (unsigned c=0;c<3;++c) out.rgb.push_back(data.at(i+c));
        }
        if (damage==1) ++out.bounds.x;
        if (damage==2) out.rgb.pop_back();
        if (damage==3) out.rgb.back()=std::numeric_limits<float>::infinity();
        if (damage==4) out.descriptor=ImageDescriptor{};
        if (damage==5) out.rgb.push_back(0);
        return out;
    }
};
std::shared_ptr<const CoverageNode> make(const Frame& f,std::shared_ptr<ScalarSource> mask,std::shared_ptr<RgbSource> rgb) {
    if (f.kind==0) return std::make_shared<CoverageDensityNode>(mask,MaskDensitySettings{number(f.control)});
    if (f.kind==1) return std::make_shared<CoverageFeatherNode>(mask,MaskFeatherSettings{static_cast<std::uint32_t>(f.radius)});
    return std::make_shared<CoverageRefineNode>(mask,rgb,mask->bounds,MaskRefineSettings{static_cast<std::uint32_t>(f.radius),number(f.control)});
}
bool exact(std::span<const float> actual,std::span<const std::uint32_t> expected) {
    if (actual.size()!=expected.size()) return false;
    for (std::size_t i=0;i<actual.size();++i) if (std::bit_cast<std::uint32_t>(actual[i])!=expected[i]) return false;
    return true;
}
struct Fixture {
    std::shared_ptr<ScalarSource> mask;std::shared_ptr<RgbSource> rgb;std::shared_ptr<const CoverageNode> node;
    explicit Fixture(const Frame& f) {
        const Rect b{f.x,f.y,f.width,f.height};mask=std::make_shared<ScalarSource>(b,values(f.input));
        rgb=std::make_shared<RgbSource>(b,values(f.guide),ImageDescriptor::scene_linear(f.space?WorkingSpace::LinearRec2020D65:WorkingSpace::LinearProPhotoD50));
        node=make(f,mask,rgb);
    }
};
void replay() {
    for (const auto& row:mask_refinement_reference::density_cases)
        require(std::bit_cast<std::uint32_t>(evaluate_mask_density(sample(row.input),{number(row.density)}))==row.output,"density scalar replay differs");
    for (const auto& f:mask_refinement_reference::frames) {
        Fixture fixture(f);
        for (RenderLevel l:std::array<RenderLevel,4>{{{},{0,RenderQuality::Preview},{1,RenderQuality::Preview},{2,RenderQuality::Preview}}}) {
            const auto b=fixture.node->output_bounds(l);const auto expected=l.mip==0?f.native:l.mip==1?f.mip1:f.mip2;
            const auto out=fixture.node->render_level(b,l);
            require(same(out.bounds,b)&&exact(out.coverage,expected),"full frozen refinement frame differs");
            for (unsigned tile:{1u,2u,8u}) {
                std::vector<float> assembly(expected.size());
                for (std::uint32_t y=0;y<b.height;y+=tile) for (std::uint32_t x=0;x<b.width;x+=tile) {
                    const Rect r{b.x+x,b.y+y,std::min(tile,b.width-x),std::min(tile,b.height-y)};
                    const auto part=fixture.node->render_level(r,l);require(same(part.bounds,r),"partition ROI mismatch");
                    for (unsigned yy=0;yy<r.height;++yy) for (unsigned xx=0;xx<r.width;++xx)
                        assembly[static_cast<std::size_t>(y+yy)*b.width+x+xx]=part.coverage[static_cast<std::size_t>(yy)*r.width+xx];
                }
                require(exact(assembly,expected),"partition changed refinement arithmetic");
            }
            // Exhaustive rectangular ROIs, not only one-cell outputs.
            for (std::uint32_t y=0;y<b.height;++y) for (std::uint32_t x=0;x<b.width;++x)
                for (std::uint32_t h=1;h<=b.height-y;++h) for (std::uint32_t w=1;w<=b.width-x;++w) {
                    const auto part=fixture.node->render_level({b.x+x,b.y+y,w,h},l);
                    for (unsigned yy=0;yy<h;++yy) for (unsigned xx=0;xx<w;++xx)
                        require(std::bit_cast<std::uint32_t>(part.coverage[static_cast<std::size_t>(yy)*w+xx])==expected[static_cast<std::size_t>(y+yy)*b.width+x+xx],"rectangular ROI arithmetic differs");
                }
        }
    }
    for (const auto& c:mask_refinement_reference::cells) {
        const auto& f=mask_refinement_reference::frames[c.frame];Fixture fixture(f);
        const RenderLevel l{static_cast<std::uint32_t>(c.mip),c.mip?RenderQuality::Preview:RenderQuality::Final};
        const Rect output{c.mip?c.x:f.x+c.x,c.mip?c.y:f.y+c.y,1,1};
        const Rect expected{c.input_region[0],c.input_region[1],c.input_region[2],c.input_region[3]};
        require(same(fixture.node->input_region_level(output,fixture.mask->bounds,l),expected),"frozen source footprint differs");
        const auto tile=fixture.node->render_level(output,l);
        require(std::bit_cast<std::uint32_t>(tile.coverage[0])==c.output,"frozen cell differs");
        require(same(fixture.mask->trace.request(),expected),"actual scalar input halo differs");
        if (f.kind==2) require(same(fixture.rgb->trace.request(),expected),"actual RGB input halo differs");
        else require(fixture.rgb->trace.calls==0,"unary refinement sampled guide");
    }
}
void guards() {
    auto mask=std::make_shared<ScalarSource>(Rect{17,23,3,3},std::vector<float>(9,0));
    auto rgb=std::make_shared<RgbSource>(mask->bounds,std::vector<float>(27,0),ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50));
    for (double d:{-1.,2.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) rejects([&]{CoverageDensityNode n(mask,{d});});
    for (unsigned r:{33u,0xffffffffu}) rejects([&]{CoverageFeatherNode n(mask,{r});});
    for (MaskRefineSettings s:std::array<MaskRefineSettings,5>{{{9,1},{0,0},{0,0x1p-25},{0,65537},{0,std::numeric_limits<double>::quiet_NaN()}}})
        rejects([&]{CoverageRefineNode n(mask,rgb,mask->bounds,s);});
    rejects([&]{CoverageDensityNode n(nullptr,{});});rejects([&]{CoverageFeatherNode n(nullptr,{});});
    rejects([&]{CoverageRefineNode n(mask,nullptr,mask->bounds,{});});
    rejects([&]{CoverageRefineNode n(mask,rgb,{0,0,3,3},{});});
    rgb->description=ImageDescriptor{};rejects([&]{CoverageRefineNode n(mask,rgb,mask->bounds,{});});
    rgb->description=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);
    // All actual needed samples reject even for zero mask/identity settings.
    for (int kind=0;kind<3;++kind) for (unsigned radius:{0u,1u}) {
        std::shared_ptr<const CoverageNode> n;
        if (!kind) n=std::make_shared<CoverageDensityNode>(mask,MaskDensitySettings{radius?1.:0.});
        else if (kind==1) n=std::make_shared<CoverageFeatherNode>(mask,MaskFeatherSettings{radius});
        else n=std::make_shared<CoverageRefineNode>(mask,rgb,mask->bounds,MaskRefineSettings{radius,0x1p-12});
        for (int damage=1;damage<=5;++damage) {mask->damage=damage;rejects([&]{n->render(mask->bounds);});}
        mask->damage=0;
        if (kind==2) {
            for (int damage=1;damage<=5;++damage) {rgb->damage=damage;rejects([&]{n->render(mask->bounds);});}
            rgb->damage=0;
        }
        for (RenderLevel l:std::array<RenderLevel,4>{{{1,RenderQuality::Final},{2,RenderQuality::Final},{3,RenderQuality::Preview},{0,static_cast<RenderQuality>(17)}}})
            rejects([&]{n->render_level(mask->bounds,l);});
        rejects([&]{n->render({16,23,1,1});});rejects([&]{n->render({17,23,0,1});});
        rejects([&]{n->input_region_level(mask->bounds,{0,0,3,3},{});});
        mask->supported=false;require(!n->supports_level({}),"unsupported scalar upstream admitted");mask->supported=true;
    }
    for (Rect b:std::array<Rect,3>{{{0,0,0,1},{0xffffffffu,0,1,1},{0,0xffffffffu,1,1}}}) {
        auto source=std::make_shared<ScalarSource>(b,std::vector<float>{});rejects([&]{CoverageDensityNode n(source,{});});
    }
    auto huge=std::make_shared<ScalarSource>(Rect{0,0,0xffffffffu,0xffffffffu},std::vector<float>{});
    auto giant=std::make_shared<RgbSource>(huge->bounds,std::vector<float>{},rgb->description);
    CoverageDensityNode d(huge,{});CoverageFeatherNode f(huge,{32});CoverageRefineNode r(huge,giant,huge->bounds,{8,1});
    for (const CoverageNode* n:{static_cast<const CoverageNode*>(&d),static_cast<const CoverageNode*>(&f),static_cast<const CoverageNode*>(&r)})
        rejects([&]{n->render(huge->bounds);});
    require(huge->trace.calls==0 && giant->trace.calls==0,"capacity rejection evaluated upstream");
    MaskDensitySettings ds{.5};CoverageDensityNode copied(mask,ds);ds.density=0;
    require(copied.render(mask->bounds).coverage[0]==.5f,"density settings not copied");
    MaskFeatherSettings fs{0};CoverageFeatherNode feather_copy(mask,fs);fs.radius=33;
    feather_copy.render(mask->bounds);
    MaskRefineSettings rs{0,1};CoverageRefineNode refine_copy(mask,rgb,mask->bounds,rs);rs.epsilon=0;
    refine_copy.render(mask->bounds);
    rgb->description=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
    rejects([&]{refine_copy.render(mask->bounds);});
}
void rounding_and_threads() {
    const int saved=std::fegetround();
    struct Restore {int mode;~Restore(){std::fesetround(mode);}} restore{saved};
    for (int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        require(std::fesetround(mode)==0,"cannot set requested rounding mode");
        for (unsigned index:{5u,10u,11u,16u,137u}) {
            const auto& f=mask_refinement_reference::frames[index];Fixture fixture(f);
            const auto whole=fixture.node->render(fixture.mask->bounds);
            for (std::uint32_t y=0;y<f.height;++y) for (std::uint32_t x=0;x<f.width;++x) {
                const auto part=fixture.node->render({f.x+x,f.y+y,1,1});
                require(std::bit_cast<std::uint32_t>(part.coverage[0])==std::bit_cast<std::uint32_t>(whole.coverage[static_cast<std::size_t>(y)*f.width+x]),"active rounding mode changes ROI equality");
            }
            require(std::fegetround()==mode,"refinement changed caller rounding control");
        }
    }
    std::fesetround(FE_TONEAREST);
    for (unsigned index:{93u,96u,104u,114u}) {
        const auto& f=mask_refinement_reference::frames[index];Fixture fixture(f);
        std::vector<std::future<CoverageTile>> jobs;
        for (unsigned worker=0;worker<8;++worker) jobs.push_back(std::async(std::launch::async,[&]{return fixture.node->render(fixture.mask->bounds);}));
        for (auto& job:jobs) require(exact(job.get().coverage,f.native),"concurrent immutable refinement changed output");
    }
}
std::string id(unsigned n) {
    char text[64];std::snprintf(text,sizeof(text),"a4000000-0000-0000-0000-%012u",n);return text;
}
EditOperation operation(const Frame& f) {
    EditOperation op;op.id=id(3);op.processing_version=2;op.input_domain=op.output_domain=EditDomain::Coverage;
    op.type_id=f.kind==0?"rawengine.mask_density":f.kind==1?"rawengine.mask_feather_binomial":"rawengine.mask_refine_working_y";
    op.inputs={{"mask",id(1)}};
    if (!f.kind) op.parameters={{"density",EditValue{number(f.control)}}};
    else op.parameters={{"radius",EditValue{std::int64_t(f.radius)}}};
    if (f.kind==2) {op.parameters.emplace("epsilon",EditValue{number(f.control)});op.inputs.emplace("image",id(2));}
    return op;
}
struct GraphFixture {
    EditManifest manifest;std::vector<BoundEditSource> bindings;Rect bounds;
    explicit GraphFixture(const Frame& f) {
        const bool guided=f.kind==2;bounds={guided?0:f.x,guided?0:f.y,f.width,f.height};
        manifest.format_version=4;manifest.working_space=f.space?WorkingSpace::LinearRec2020D65:WorkingSpace::LinearProPhotoD50;
        auto mask=std::make_shared<CoverageRasterNode>(CoverageImage({bounds,0},values(f.input)));
        EditSource source;source.id=id(1);source.kind=EditSourceKind::CoverageRasterF32;source.content_sha256=*mask->source_fingerprint();
        manifest.sources.push_back(source);bindings.push_back({source,nullptr,bounds,mask});
        if (guided) {
            auto guide=std::make_shared<RasterSourceNode>(RasterImage({bounds.width,bounds.height,0,manifest.working_space},values(f.guide)));
            source.id=id(2);source.kind=EditSourceKind::SceneLinearRasterF32;source.working_space=manifest.working_space;source.content_sha256=*guide->source_fingerprint();
            manifest.sources.push_back(source);bindings.push_back({source,guide,bounds});
        }
        manifest.operations={operation(f)};manifest.output_id=id(3);
    }
};
bool exact_graph(std::span<const float> actual,std::span<const std::uint32_t> expected) {
    if (actual.size()!=expected.size()) return false;
    for (std::size_t i=0;i<actual.size();++i)
        if (std::bit_cast<std::uint32_t>(actual[i])!=(expected[i]==0x80000000u?0:expected[i])) return false;
    return true;
}
void saved_graphs() {
    unsigned frame_index=0;
    for (const auto& f:mask_refinement_reference::frames) {
        GraphFixture fixture(f);auto cache=std::make_shared<TileCache>(1024*1024);
        ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(fixture.manifest)),fixture.bindings,nullptr,cache);
        for (RenderLevel l:std::array<RenderLevel,4>{{{},{0,RenderQuality::Preview},{1,RenderQuality::Preview},{2,RenderQuality::Preview}}}) {
            const auto bounds=graph.coverage_output().output_bounds(l);const auto truth=!l.mip?f.native:l.mip==1?f.mip1:f.mip2;
            for (unsigned size:{1u,2u,8u}) {
                auto output=CoverageRenderer{}.render_image(graph.coverage_output(),RenderRequest{bounds,size,l});
                require(exact_graph(output.coverage,truth),"saved refinement graph differs from frozen output");
            }
            for (const auto& cell:mask_refinement_reference::cells) if (cell.frame==frame_index && cell.mip==l.mip) {
                const Rect roi{bounds.x+cell.x,bounds.y+cell.y,1,1};
                const Rect expected{cell.input_region[0]-f.x+fixture.bounds.x,cell.input_region[1]-f.y+fixture.bounds.y,
                                    cell.input_region[2],cell.input_region[3]};
                const auto regions=graph.required_source_regions(roi,l);
                require(regions.size()==(f.kind==2?2:1) && same(regions.at(id(1)),expected),"saved refinement scalar halo differs");
                if (f.kind==2) require(same(regions.at(id(2)),expected),"saved refinement RGB halo differs");
            }
        }
        auto disabled=fixture.manifest;disabled.operations[0].enabled=false;
        ExecutableEditGraph bypass(disabled,fixture.bindings,nullptr,cache);
        require(bypass.required_source_regions(fixture.bounds).size()==1,"disabled refinement samples guide");
        require(exact_graph(bypass.coverage_output().render(fixture.bounds).coverage,f.input),"disabled refinement changes mask");
        graph.coverage_output().render(fixture.bounds);const auto warm=cache->stats();graph.coverage_output().render(fixture.bounds);
        require(cache->stats().hits>warm.hits && cache->stats().misses==warm.misses,"refinement warm cache misses");
        auto changed=fixture.manifest;
        if (!f.kind) changed.operations[0].parameters["density"]=EditValue{number(f.control)==0?1.:0.};
        else if (f.kind==1) changed.operations[0].parameters["radius"]=EditValue{std::int64_t(f.radius?0:1)};
        else changed.operations[0].parameters["epsilon"]=EditValue{number(f.control)==65536?0x1p-24:65536.};
        EditHistory history(fixture.manifest,fixture.bindings,{8,1024*1024},nullptr,cache);
        const auto revision=history.commit(changed);require(history.undo()==1 && history.redo()==revision,"refinement history navigation");
        auto restored=EditHistory::restore(history.serialize(),fixture.bindings,nullptr,cache);
        require(restored->serialize()==history.serialize(),"refinement history exact restore");
        const auto misses=cache->stats().misses;restored->revision(revision)->graph->coverage_output().render(fixture.bounds);
        require(cache->stats().misses>misses,"changed refinement parameter identity aliased");
        TileScheduler scheduler(2,8);auto snapshot=history.revision(1);
        require(exact_graph(scheduler.submit(snapshot->graph->coverage_output_handle(),RenderRequest{fixture.bounds,2,{}}).get().coverage,f.native),"pinned refinement job differs");
        ++frame_index;
    }
    for (unsigned index:{0u,5u,11u}) {
        const auto& f=mask_refinement_reference::frames[index];GraphFixture fixture(f);
        for (bool enabled:{false,true}) {
            auto invalid=fixture.manifest;invalid.operations[0].enabled=enabled;
            invalid.operations[0].parameters.begin()->second=EditValue{true};rejects([&]{ExecutableEditGraph g(invalid,fixture.bindings);});
            invalid=fixture.manifest;invalid.operations[0].enabled=enabled;invalid.operations[0].parameters.clear();rejects([&]{ExecutableEditGraph g(invalid,fixture.bindings);});
            invalid=fixture.manifest;invalid.operations[0].enabled=enabled;invalid.operations[0].inputs.emplace("extra",id(1));rejects([&]{ExecutableEditGraph g(invalid,fixture.bindings);});
            invalid=fixture.manifest;invalid.operations[0].enabled=enabled;invalid.operations[0].opacity=.5;rejects([&]{ExecutableEditGraph g(invalid,fixture.bindings);});
        }
        const char* canonical=f.kind==0?mask_refinement_reference::operation0_json:f.kind==1?mask_refinement_reference::operation1_json:mask_refinement_reference::operation2_json;
        auto wrapped=serialize_edit_manifest(fixture.manifest);const auto start=wrapped.find("\"operations\":["),end=wrapped.find("],\"output\"",start);
        require(start!=std::string::npos && end!=std::string::npos,"canonical wrapper fields");wrapped.replace(start+14,end-start-14,canonical);
        require(serialize_edit_manifest(parse_edit_manifest(wrapped)).find(canonical)!=std::string::npos,"canonical refinement bytes changed");
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
class GatedScalar final : public CoverageNode {
public:
    GatedScalar(std::shared_ptr<const CoverageNode> input,std::shared_ptr<Gate> gate,bool fail):input_(std::move(input)),gate_(std::move(gate)),fail_(fail) {}
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
    auto& edge=f.bindings[f.bindings.size()==2?1:0];
    if (edge.coverage_node) edge.coverage_node=std::make_shared<GatedScalar>(edge.coverage_node,gate,fail);
    else edge.node=std::make_shared<GatedRgb>(edge.node,gate,fail);
}
void entered(std::future<void>& future) {
    require(future.wait_for(std::chrono::seconds(10))==std::future_status::ready,"refinement guide gate timed out");future.get();
}
void generation_recovery_and_budget() {
    for (unsigned index:{69u,76u,82u}) {
        const auto& frame=mask_refinement_reference::frames[index];
        {
            GraphFixture f(frame);auto gate=std::make_shared<Gate>();auto started=gate->entered.get_future();gated_guide(f,gate);
            auto cache=std::make_shared<TileCache>(1024*1024);ExecutableEditGraph graph(f.manifest,f.bindings,nullptr,cache);
            auto pending=std::async(std::launch::async,[&]{return graph.coverage_output().render(f.bounds);});ReleaseOnExit release{gate};
            entered(started);cache->clear();gate->open();
            require(exact_graph(pending.get().coverage,frame.native),"refinement in-flight clear changed pixels");
            require(cache->stats().entries==0,"old refinement/guide cache generation repopulated cleared cache");
            require(exact_graph(graph.coverage_output().render(f.bounds).coverage,frame.native),"refinement new generation failed");
            require(cache->stats().entries>0,"new refinement/guide generation not cached");
        }
        for (bool fail:{false,true}) {
            GraphFixture f(frame);auto gate=std::make_shared<Gate>();auto started=gate->entered.get_future();gated_guide(f,gate,fail);
            auto graph=std::make_shared<ExecutableEditGraph>(f.manifest,f.bindings);TileScheduler scheduler(1,2);ReleaseOnExit release{gate};
            const RenderRequest request{f.bounds,2,{}};auto output=graph->coverage_output_handle();
            auto first=fail?scheduler.submit(output,request):scheduler.submit_latest("refinement",output,request);
            entered(started);auto next=fail?scheduler.submit(output,request):scheduler.submit_latest("refinement",output,request);gate->open();
            bool expected=false;
            try {first.get();} catch (const std::bad_alloc&) {expected=fail;} catch (const RenderCancelled&) {expected=!fail;}
            require(expected,"refinement guide source fault/supersession has wrong future result");
            require(exact_graph(next.get().coverage,frame.native),"queued refinement recovery pixels changed");
            require(exact_graph(scheduler.submit(output,request).get().coverage,frame.native),"later refinement recovery pixels changed");
        }
        GraphFixture f(frame);auto tiny=std::make_shared<TileCache>(1);ExecutableEditGraph graph(f.manifest,f.bindings,nullptr,tiny);
        require(exact_graph(graph.coverage_output().render(f.bounds).coverage,frame.native),"refinement tiny budget changed pixels");
        require(tiny->stats().entries==0,"oversized refinement/guide admitted to tiny budget");
        TileScheduler scheduler(1,2);auto token=std::make_shared<CancellationToken>();token->cancel();
        auto cancelled=scheduler.submit(graph.coverage_output_handle(),RenderRequest{f.bounds,2,{}},RenderPriority::Normal,token);
        bool observed=false;try {cancelled.get();} catch (const RenderCancelled&) {observed=true;}
        require(observed,"refinement cancelled job rendered");
        require(exact_graph(scheduler.submit(graph.coverage_output_handle(),RenderRequest{f.bounds,2,{}}).get().coverage,frame.native),"refinement cancellation recovery");
    }
}
}
int main() {
    try {replay();guards();rounding_and_threads();saved_graphs();generation_recovery_and_budget();std::cout<<"238 density cases / 138 frames / 2001 footprints, exhaustive ROI, guards, four-mode ROI/control preservation, threads and saved graphs passed\n";return 0;}
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
