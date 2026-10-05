#include "EditGraph.hpp"
#include "RgbaDelivery.hpp"
#include "TileScheduler.hpp"
#include "reference/rgba_native_v1.hpp"

#include <atomic>
#include <bit>
#include <future>
#include <iostream>

using namespace rawengine;
namespace {
void check(bool yes,const char* message) {if (!yes) throw std::runtime_error(message);}
template<class E=std::invalid_argument,class F> void rejects(F f) {
    bool caught=false;try {f();} catch (const E&) {caught=true;}check(caught,"expected rejection");
}
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}
std::string id(int n) {const auto digits=std::to_string(n);return "81000000-0000-4000-8000-"+std::string(12-digits.size(),'0')+digits;}
std::vector<float> floats(const std::vector<std::uint32_t>& words) {
    std::vector<float> output;for (auto word:words) output.push_back(std::bit_cast<float>(word));return output;
}
EditDomain rgb_domain(WorkingSpace s) {return s==WorkingSpace::LinearProPhotoD50?EditDomain::SceneLinearProPhotoD50:EditDomain::SceneLinearRec2020D65;}
EditDomain rgba_domain(WorkingSpace s) {return s==WorkingSpace::LinearProPhotoD50?EditDomain::PremultipliedSceneLinearProPhotoD50:EditDomain::PremultipliedSceneLinearRec2020D65;}
BoundEditSource rgba_source(int n,Rect b,WorkingSpace s,std::vector<float> values) {
    auto node=std::make_shared<RgbaRasterNode>(RgbaImage({b,0,s},values));
    EditSource record;record.id=id(n);record.kind=EditSourceKind::PremultipliedRgbaRasterF32;
    record.working_space=s;record.content_sha256=*node->source_fingerprint();
    return {record,nullptr,b,nullptr,node};
}
BoundEditSource mask_source(int n,Rect b,std::vector<float> values) {
    auto node=std::make_shared<CoverageRasterNode>(CoverageImage({b},values));
    EditSource record;record.id=id(n);record.kind=EditSourceKind::CoverageRasterF32;record.content_sha256=*node->source_fingerprint();
    return {record,nullptr,b,node};
}
BoundEditSource rgb_source(int n,Rect b,WorkingSpace s,std::vector<float> values) {
    RasterImage image({b.width,b.height,0,s},std::move(values));
    auto node=std::make_shared<RasterSourceNode>(image);
    EditSource record;record.id=id(n);record.working_space=s;record.content_sha256=*node->source_fingerprint();
    return {record,node,b};
}
EditOperation operation(std::string type,std::map<std::string,std::string> inputs,EditDomain in,EditDomain out) {
    EditOperation op;op.id=id(4);op.type_id=std::move(type);op.processing_version=2;
    op.inputs=std::move(inputs);op.input_domain=in;op.output_domain=out;return op;
}
struct Fixture {
    EditManifest manifest;std::vector<BoundEditSource> bindings;
    Fixture(const rgba_native_reference::Frame& f) {
        const auto space=f.space==1?WorkingSpace::LinearProPhotoD50:WorkingSpace::LinearRec2020D65;
        Rect b{f.bounds[0],f.bounds[1],f.bounds[2],f.bounds[3]};
        if (f.kind==1) b.x=b.y=0; // RGB built-in sources are zero-origin.
        manifest.format_version=5;manifest.working_space=space;manifest.output_id=id(1);
        if (f.kind==1) {
            std::vector<float> rgb,alpha;const auto input=floats(f.source);
            for (std::size_t i=0;i<input.size();i+=4) {rgb.insert(rgb.end(),input.begin()+i,input.begin()+i+3);alpha.push_back(input[i+3]);}
            bindings.push_back(rgb_source(1,b,space,rgb));bindings.push_back(mask_source(2,b,alpha));
            manifest.operations.push_back(operation("rawengine.rgba_premultiply",{{"image",id(1)},{"alpha",id(2)}},rgb_domain(space),rgba_domain(space)));
        } else {
            bindings.push_back(rgba_source(1,b,space,floats(f.source)));
            if (f.kind==2) {
                bindings.push_back(mask_source(2,b,floats(f.mask)));
                manifest.operations.push_back(operation("rawengine.rgba_apply_coverage",{{"image",id(1)},{"coverage",id(2)}},rgba_domain(space),rgba_domain(space)));
            }
            if (f.kind==3) {
                bindings.push_back(rgba_source(2,b,space,floats(f.backdrop)));
                manifest.operations.push_back(operation("rawengine.rgba_source_over",{{"source",id(1)},{"backdrop",id(2)}},rgba_domain(space),rgba_domain(space)));
            }
            if (f.kind==4 || f.kind==5) manifest.operations.push_back(operation(
                f.kind==4?"rawengine.rgba_alpha":"rawengine.rgba_straight_rgb",{{"image",id(1)}},rgba_domain(space),f.kind==4?EditDomain::Coverage:rgb_domain(space)));
        }
        for (const auto& binding:bindings) manifest.sources.push_back(binding.identity);
        if (!manifest.operations.empty()) manifest.output_id=id(4);
    }
};
void exact(const std::vector<float>& actual,const std::vector<std::uint32_t>& expected) {
    check(actual.size()==expected.size(),"frozen storage count");
    for (std::size_t i=0;i<actual.size();++i) check(std::bit_cast<std::uint32_t>(actual[i])==expected[i],"independent saved replay bits");
}
void frozen_saved() {
    for (const auto& frame:rgba_native_reference::frames) {
        Fixture f(frame);const auto text=serialize_edit_manifest(f.manifest);
        check(serialize_edit_manifest(parse_edit_manifest(text))==text,"canonical manifest round trip");
        auto cache=std::make_shared<TileCache>(1024*1024);
        ExecutableEditGraph graph(parse_edit_manifest(text),f.bindings,nullptr,cache);
        check(graph.output_is_rgba()==(frame.kind<=3) && graph.output_is_coverage()==(frame.kind==4),"typed saved output");
        if (frame.kind<=3) {rejects([&]{graph.output();});rejects([&]{graph.coverage_output();});}
        else rejects([&]{graph.rgba_output();});
        for (auto level: {RenderLevel{},RenderLevel{0,RenderQuality::Preview},RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            Rect extent=graph.output_bounds();if (level.mip) {
                const auto scale=1u<<level.mip;extent={0,0,extent.width/scale+(extent.width%scale!=0),extent.height/scale+(extent.height%scale!=0)};
            }
            for (auto size:{1u,2u,8u}) {
                RenderRequest request{extent,size,level};
                if (frame.kind<=3) exact(RgbaRenderer{}.render_image(graph.rgba_output(),request).rgba,frame.levels[level.mip]);
                else if (frame.kind==4) exact(CoverageRenderer{}.render_image(graph.coverage_output(),request).coverage,frame.levels[level.mip]);
                else exact(Renderer{}.render_image(graph,request).rgb,frame.levels[level.mip]);
            }
            for (std::uint32_t y=0;y<extent.height;++y)
                for (std::uint32_t x=0;x<extent.width;++x) {
                    const Rect roi{extent.x+x,extent.y+y,1,1};
                    const auto regions=graph.required_source_regions(roi,level);
                    check(regions.size()==f.bindings.size(),"every named source footprint");
                    for (const auto& binding:f.bindings) {
                        const auto native=binding.bounds;const auto scale=1u<<level.mip;
                        const Rect expected=level.mip?Rect{native.x+x*scale,native.y+y*scale,
                            std::min(scale,native.width-x*scale),std::min(scale,native.height-y*scale)}:roi;
                        check(same(regions.at(binding.identity.id),expected),"native source-ID footprint");
                    }
                }
        }
        if (frame.kind<=3) {
            const auto native=graph.output_bounds();graph.rgba_output().render(native);const auto before=cache->stats();
            exact(graph.rgba_output().render(native).rgba,frame.levels[0]);check(cache->stats().misses==before.misses,"warm RGBA cache");
            auto pinned=graph.rgba_output_handle();TileScheduler scheduler(2,8);
            exact(scheduler.submit(pinned,RenderRequest{native,2,{}}).get().rgba,frame.levels[0]);
            EditHistory history(f.manifest,f.bindings,{});auto snapshot=history.current();
            auto restored=EditHistory::restore(history.serialize(),f.bindings,nullptr,cache);
            check(restored->serialize()==history.serialize(),"exact history restore");
            auto next=f.manifest;next.output_id=f.bindings.back().identity.id;
            history.commit(next);check(history.undo()==snapshot->id && history.redo()!=snapshot->id,"history navigation");
            exact(snapshot->graph->rgba_output().render(native).rgba,frame.levels[0]);
        }
    }
}
void strict_guards() {
    const auto& frame=rgba_native_reference::frames[3*7+3];Fixture fixture(frame);
    auto manifest=fixture.manifest;auto bindings=fixture.bindings;
    auto invalid=[&](EditManifest bad){rejects([&]{ExecutableEditGraph graph(bad,bindings);});};
    for (bool enabled:{true,false}) {
        auto bad=manifest;bad.operations[0].enabled=enabled;
        auto control=bad;control.operations[0].parameters["extra"]=EditValue{1.0};invalid(control);
        control=bad;control.operations[0].extra_fields["extra"]=EditValue{true};invalid(control);
        control=bad;control.operations[0].opacity=.5;invalid(control);
        control=bad;control.operations[0].blend_mode="multiply";invalid(control);
        control=bad;control.operations[0].inputs.erase("source");invalid(control);
        control=bad;control.operations[0].inputs["mask"]=id(1);invalid(control);
        control=bad;control.operations[0].input_domain=EditDomain::Coverage;invalid(control);
        control=bad;control.operations[0].output_domain=EditDomain::SceneLinearProPhotoD50;invalid(control);
        control=bad;control.operations[0].schema_version=2;invalid(control);
        control=bad;control.operations[0].processing_version=1;invalid(control);
        control=bad;control.operations[0].masks["coverage"]=id(1);invalid(control);
    }
    auto disabled=manifest;disabled.operations[0].enabled=false;
    ExecutableEditGraph bypass(disabled,bindings);
    check(bypass.required_source_regions(bypass.output_bounds()).size()==1 &&
          bypass.required_source_regions(bypass.output_bounds()).contains(id(2)),"disabled over excludes source dependency");
    auto output=bypass.rgba_output().render(bypass.output_bounds());
    check(output.rgba==bindings[1].rgba_node->render(bindings[1].bounds).rgba,"disabled over aliases backdrop");
    for (int format:{2,3,4}) {auto bad=manifest;bad.format_version=format;invalid(bad);}
    auto bad=manifest;bad.sources[0].working_space.reset();invalid(bad);
    bad=manifest;bad.sources[0].content_sha256[0]^=1;invalid(bad);
    for (int mode=0;mode<4;++mode) {
        auto changed=bindings;
        if (mode==0) changed[0].coverage_node=std::make_shared<CoverageRasterNode>(CoverageImage({changed[0].bounds},std::vector<float>(9)));
        if (mode==1) changed[0].rgba_node.reset();
        if (mode==2) ++changed[0].bounds.x;
        if (mode==3) changed[0].rgba_node=rgba_source(1,changed[0].bounds,WorkingSpace::LinearRec2020D65,output.rgba).rgba_node;
        rejects([&]{ExecutableEditGraph graph(manifest,changed);});
    }
    // Every adapter validates its fixed port/type/domain contract when disabled.
    for (int kind:{1,4,5}) {
        Fixture f(rgba_native_reference::frames[kind*7+3]);f.manifest.operations[0].enabled=false;
        rejects([&]{ExecutableEditGraph graph(f.manifest,f.bindings);});
    }
    auto wrong=manifest;wrong.operations[0]=operation("rawengine.exposure",{{"image",id(1)}},
        EditDomain::SceneLinearProPhotoD50,EditDomain::SceneLinearProPhotoD50);
    wrong.operations[0].parameters["stops"]=EditValue{0.0};invalid(wrong);
    auto duplicate=bindings;duplicate[1].rgba_node=duplicate[0].rgba_node;duplicate[1].identity.content_sha256=duplicate[0].identity.content_sha256;
    auto repeated=manifest;repeated.sources[1]=duplicate[1].identity;ExecutableEditGraph sharing(repeated,duplicate);
    check(sharing.required_source_regions(sharing.output_bounds()).size()==2,"shared runtime retains both source IDs");
    rejects([&]{sharing.source_bounds();});
    auto cache=std::make_shared<TileCache>(2048);std::array<std::uint8_t,32> signature{};signature.fill(42);
    const Rect one{0,0,1,1};auto rgba=rgba_source(1,one,WorkingSpace::LinearProPhotoD50,{1,2,3,1});
    auto rgb=rgb_source(2,one,WorkingSpace::LinearProPhotoD50,{1,2,3});auto mask=mask_source(3,one,{1});
    cache->render(*rgba.rgba_node,signature,one);cache->render(*rgb.node,signature,one);cache->render(*mask.coverage_node,signature,one);
    check(cache->stats().entries==3 && cache->stats().used_bytes==3*256+16+12+4,"typed cache separation and actual channel accounting");
    TileCache tiny(1);tiny.render(*rgba.rgba_node,signature,one);check(tiny.stats().entries==0,"oversized RGBA bypass");
    RgbaRenderer renderer;auto token=std::make_shared<CancellationToken>();int callbacks=0;
    rejects<RenderCancelled>([&]{renderer.render_tiles(*bindings[0].rgba_node,RenderRequest{bindings[0].bounds,1,{}},
        [&](const auto&){++callbacks;token->cancel();},token.get());});
    check(callbacks==1,"cooperative callback cancellation");
    const auto empty=renderer.render_image(*rgba.rgba_node,RenderRequest{{1,1,0,0},1,{}});check(empty.rgba.empty(),"empty RGBA delivery");
}
struct Gate {
    std::promise<void> entered,release;std::shared_future<void> released=release.get_future().share();
    std::atomic<bool> first{true};bool fail=false;
};
class GatedRgba final:public RgbaNode {
public:
    GatedRgba(std::shared_ptr<const RgbaNode> node,std::shared_ptr<Gate> gate):node_(std::move(node)),gate_(std::move(gate)) {}
    Rect native_bounds() const noexcept override {return node_->native_bounds();}
    WorkingSpace working_space() const noexcept override {return node_->working_space();}
    bool supports_level(RenderLevel l) const noexcept override {return node_->supports_level(l);}
    std::optional<std::array<std::uint8_t,32>> source_fingerprint() const override {return node_->source_fingerprint();}
    PremultipliedRgbaTile render_level(Rect b,RenderLevel l) const override {
        if (gate_->first.exchange(false)) {gate_->entered.set_value();gate_->released.wait();if (gate_->fail) throw std::bad_alloc();}
        return node_->render_level(b,l);
    }
private:std::shared_ptr<const RgbaNode> node_;std::shared_ptr<Gate> gate_;
};
void gated_recovery() {
    const Rect b{0,0,2,2};const auto original=rgba_source(1,b,WorkingSpace::LinearProPhotoD50,std::vector<float>{1,2,3,1,1,2,3,1,1,2,3,1,1,2,3,1});
    for (bool fail:{false,true}) {
        auto gate=std::make_shared<Gate>();gate->fail=fail;auto entered=gate->entered.get_future();
        auto source=std::make_shared<GatedRgba>(original.rgba_node,gate);TileCache cache(4096);
        auto pending=std::async(std::launch::async,[&]{return cache.render(*source,original.identity.content_sha256,b);});
        const bool ready=entered.wait_for(std::chrono::seconds(5))==std::future_status::ready;
        cache.clear();gate->release.set_value();check(ready,"cache gate entry");
        if (fail) rejects<std::bad_alloc>([&]{pending.get();});else check(pending.get().rgba==original.rgba_node->render(b).rgba,"in-flight output retained");
        check(cache.stats().entries==0,"clear/fault does not publish old generation");
        check(cache.render(*source,original.identity.content_sha256,b).rgba==original.rgba_node->render(b).rgba,"later RGBA cache recovery");
    }
    auto gate=std::make_shared<Gate>();auto entered=gate->entered.get_future();TileScheduler scheduler(1,2);
    auto active=scheduler.submit_latest("mixed",std::make_shared<GatedRgba>(original.rgba_node,gate),RenderRequest{b,1,{}});
    const bool ready=entered.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    auto rgb=rgb_source(2,b,WorkingSpace::LinearProPhotoD50,std::vector<float>(12,1));auto mask=mask_source(3,b,std::vector<float>(4,1));
    auto queued=scheduler.submit_latest("mixed",rgb.node,b,RenderRequest{b,1,{}});
    auto replacement=scheduler.submit_latest("mixed",mask.coverage_node,RenderRequest{b,1,{}});
    gate->release.set_value();check(ready,"scheduler gate entry");
    rejects<RenderCancelled>([&]{active.get();});rejects<RenderCancelled>([&]{queued.get();});
    check(replacement.get().coverage==std::vector<float>(4,1),"cross-payload replacement succeeds");
    check(scheduler.submit(original.rgba_node,RenderRequest{b,2,{}}).get().rgba==original.rgba_node->render(b).rgba,"later RGBA scheduler recovery");
    auto fault=std::make_shared<Gate>();fault->fail=true;auto fault_entered=fault->entered.get_future();
    auto first=scheduler.submit(std::make_shared<GatedRgba>(original.rgba_node,fault),RenderRequest{b,1,{}});
    const bool fault_ready=fault_entered.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    auto healthy=scheduler.submit(original.rgba_node,RenderRequest{b,1,{}});
    fault->release.set_value();check(fault_ready,"worker fault gate entry");
    rejects<std::bad_alloc>([&]{first.get();});
    check(healthy.get().rgba==original.rgba_node->render(b).rgba,"queued RGBA job recovers after source fault");
}
void mixed_composition() {
    const Rect b{0,0,7,5};
    for (auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        auto rgb=rgb_source(1,b,space,std::vector<float>(105,2));
        auto mask=mask_source(2,b,std::vector<float>(35,.5f));
        std::vector<float> background;
        for (int i=0;i<35;++i) background.insert(background.end(),{6,6,6,1});
        auto rgba=rgba_source(3,b,space,background);
        std::vector<BoundEditSource> bindings{rgb,mask,rgba};EditManifest manifest;manifest.format_version=5;manifest.working_space=space;
        for (auto source:bindings) manifest.sources.push_back(source.identity);
        auto append=[&](int n,std::string type,std::map<std::string,std::string> inputs,EditDomain in,EditDomain out) {
            auto op=operation(type,inputs,in,out);op.id=id(n);manifest.operations.push_back(op);
        };
        append(4,"rawengine.rgba_premultiply",{{"image",id(1)},{"alpha",id(2)}},rgb_domain(space),rgba_domain(space));
        append(5,"rawengine.rgba_source_over",{{"source",id(4)},{"backdrop",id(3)}},rgba_domain(space),rgba_domain(space));
        // Shared input mask scales alpha1 by .5, preserving native straight color4.
        append(6,"rawengine.rgba_apply_coverage",{{"image",id(5)},{"coverage",id(2)}},rgba_domain(space),rgba_domain(space));
        append(7,"rawengine.rgba_straight_rgb",{{"image",id(6)}},rgba_domain(space),rgb_domain(space));
        append(8,"rawengine.rgba_alpha",{{"image",id(6)}},rgba_domain(space),EditDomain::Coverage);
        append(9,"rawengine.masked_mix",{{"base",id(1)},{"layer",id(7)}},rgb_domain(space),rgb_domain(space));
        manifest.operations.back().masks={{"coverage",id(8)}};manifest.operations.back().parameters={{"amount",EditValue{1.0}}};
        append(10,"rawengine.box_blur",{{"image",id(9)}},rgb_domain(space),rgb_domain(space));
        manifest.operations.back().parameters={{"radius",EditValue{std::int64_t{1}}}};
        append(11,"rawengine.crop",{{"image",id(10)}},rgb_domain(space),rgb_domain(space));
        manifest.operations.back().parameters={{"x",EditValue{std::int64_t{1}}},{"y",EditValue{std::int64_t{1}}},
            {"width",EditValue{std::int64_t{5}}},{"height",EditValue{std::int64_t{3}}}};manifest.output_id=id(11);
        ExecutableEditGraph graph(manifest,bindings,nullptr,std::make_shared<TileCache>(1<<20));
        for (auto level:{RenderLevel{},RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            const auto scale=1u<<level.mip;const Rect full{0,0,(5+scale-1)/scale,(3+scale-1)/scale};
            const auto regions=graph.required_source_regions(full,level);
            check(regions.size()==3,"shared three-payload composition retains every source ID");
            for (const auto& [name,region]:regions) check(same(region,b),"crop/blur/adapter source halo merge");
            for (auto size:{1u,2u,8u}) {
                const auto tile=Renderer{}.render_image(graph,RenderRequest{full,size,level});
                for (float value:tile.rgb) check(value==3,"independent flat normal-over/alpha/masked-mix truth");
            }
        }
    }
}
} // namespace
int main() {
    try {frozen_saved();strict_guards();gated_recovery();mixed_composition();std::cout<<"84 saved frames, typed cache/history/footprints/delivery, mixed halo composition and gated cross-payload recovery passed\n";return 0;}
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
