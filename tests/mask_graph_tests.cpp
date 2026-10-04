#include "EditGraph.hpp"
#include "TileScheduler.hpp"
#include "reference/mask_graph_numeric_v1.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>
#include <span>
#include <stdexcept>

using namespace rawengine;

namespace {
void require(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
template<class Exception=std::invalid_argument,class Callback> void rejects(Callback call) {
    try {call();} catch (const Exception&) {return;}
    throw std::runtime_error("expected rejection did not occur");
}
std::string id(int number) {char tail[16];std::snprintf(tail,sizeof(tail),"%012d",number);return std::string("00000000-0000-4000-8000-")+tail;}
std::vector<float> decoded(std::span<const std::uint32_t> words) {
    std::vector<float> values;for (auto word:words) values.push_back(std::bit_cast<float>(word));return values;
}
void exact(const std::vector<float>& actual,std::span<const std::uint32_t> words) {
    require(actual.size()==words.size(),"numeric fixture count differs");
    for (std::size_t i=0;i<actual.size();++i)
        require(std::bit_cast<std::uint32_t>(actual[i])==words[i],"numeric fixture bits differ");
}
bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;}
EditDomain domain(WorkingSpace space) {return space==WorkingSpace::LinearProPhotoD50?EditDomain::SceneLinearProPhotoD50:EditDomain::SceneLinearRec2020D65;}
EditOperation operation(int number,const char* name,EditDomain d) {
    EditOperation op;op.id=id(number);op.type_id=name;op.processing_version=2;op.input_domain=op.output_domain=d;return op;
}

struct Fixture {
    EditManifest manifest;
    std::vector<BoundEditSource> bindings;
    Fixture(WorkingSpace space=WorkingSpace::LinearProPhotoD50) {
        const Rect bounds{0,0,7,5};manifest.format_version=4;manifest.working_space=space;
        for (int i=1;i<=2;++i) {
            auto node=std::make_shared<RasterSourceNode>(RasterImage({7,5,0,space},
                decoded(i==1?std::span(mask_graph_reference::base_rgb):std::span(mask_graph_reference::layer_rgb))));
            EditSource source;source.id=id(i);source.working_space=space;source.content_sha256=*node->source_fingerprint();
            manifest.sources.push_back(source);bindings.push_back({source,node,bounds});
        }
        for (int i=3;i<=4;++i) {
            auto node=std::make_shared<CoverageRasterNode>(CoverageImage({bounds,0},
                decoded(i==3?std::span(mask_graph_reference::mask_a):std::span(mask_graph_reference::mask_b))));
            EditSource source;source.id=id(i);source.kind=EditSourceKind::CoverageRasterF32;source.content_sha256=*node->source_fingerprint();
            manifest.sources.push_back(source);bindings.push_back({source,nullptr,bounds,node});
        }
        auto inverse=operation(5,"rawengine.mask_invert",EditDomain::Coverage);inverse.inputs={{"mask",id(3)}};
        auto combine=operation(6,"rawengine.mask_combine",EditDomain::Coverage);combine.inputs={{"base",id(5)},{"layer",id(4)}};
        combine.parameters={{"mode",EditValue{std::string("intersect")}}};
        auto mix=operation(7,"rawengine.masked_mix",domain(space));mix.inputs={{"base",id(1)},{"layer",id(2)}};
        mix.masks={{"coverage",id(6)}};mix.parameters={{"amount",EditValue{0.5}}};
        manifest.operations={inverse,combine,mix};manifest.output_id=id(7);
    }
};

void numeric() {
    for (const auto& test:mask_graph_reference::algebra) {
        const auto a=std::bit_cast<float>(test.a),b=std::bit_cast<float>(test.b);
        const auto value=test.mode==0?invert_coverage(a):combine_coverage(a,b,static_cast<MaskCombineMode>(test.mode-1));
        require(std::bit_cast<std::uint32_t>(value)==test.output,"mask algebra oracle mismatch");
    }
    for (const auto& test:mask_graph_reference::mixes) {
        const auto descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);
        Tile base{{0,0,1,1},decoded(test.base),descriptor},layer{{0,0,1,1},decoded(test.layer),descriptor};
        CoverageTile mask{{0,0,1,1},{std::bit_cast<float>(test.mask)}};
        auto call=[&] {return masked_mix_rgb(base,layer,mask,std::bit_cast<double>(test.amount));};
        if (test.overflow) rejects<std::overflow_error>(call);
        else exact(call().rgb,test.output);
    }
    for (auto word:{0x80000001u,0x3f800001u,0x7f800000u,0x7fc00000u}) {
        rejects([&] {invert_coverage(std::bit_cast<float>(word));});
        rejects([&] {combine_coverage(0,std::bit_cast<float>(word),MaskCombineMode::Add);});
    }
    rejects([] {combine_coverage(0,0,static_cast<MaskCombineMode>(99));});
    Tile base{{0,0,1,1},{-0.0f,-1,4},ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50)},layer=base;
    layer.rgb={0.0f,2,-4};CoverageTile zero{{0,0,1,1},{0}};
    exact(masked_mix_rgb(base,layer,zero,1).rgb,std::array<std::uint32_t,3>{0x80000000u,0xbf800000u,0x40800000u});
    layer.rgb.back()=std::numeric_limits<float>::quiet_NaN();
    rejects([&] {masked_mix_rgb(base,layer,zero,0);});
}

void frames_and_planning() {
    for (auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        Fixture f(space);const auto json=serialize_edit_manifest(f.manifest);
        require(parse_edit_manifest(json)==f.manifest,"format4 round trip changed state");
        auto cache=std::make_shared<TileCache>(1<<20);
        ExecutableEditGraph graph(parse_edit_manifest(json),f.bindings,nullptr,cache);
        require(!graph.output_is_coverage(),"masked output must remain RGB");
        rejects([&] {graph.coverage_output();});
        Renderer renderer;
        for (auto level:{RenderLevel{},RenderLevel{0,RenderQuality::Preview},RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            const Rect bounds{0,0,level.mip==0?7u:level.mip==1?4u:2u,level.mip==0?5u:level.mip==1?3u:2u};
            const auto expected=level.mip==0?std::span<const std::uint32_t>(mask_graph_reference::mixed_mip0):
                level.mip==1?std::span<const std::uint32_t>(mask_graph_reference::mixed_mip1):std::span<const std::uint32_t>(mask_graph_reference::mixed_mip2);
            for (unsigned size:{1u,2u,8u}) exact(renderer.render_image(graph,RenderRequest{bounds,size,level}).rgb,expected);
            for (unsigned y=0;y<bounds.height;++y) for (unsigned x=0;x<bounds.width;++x) {
                const Rect roi{x,y,1,1};const auto output=renderer.render_image(graph,RenderRequest{roi,1,level});
                exact(output.rgb,expected.subspan((y*bounds.width+x)*3,3));
                const auto footprints=graph.required_source_regions(roi,level);require(footprints.size()==4,"mask dependencies missing from planner");
                const auto scale=1u<<level.mip;
                const Rect native{x*scale,y*scale,std::min(scale,7u-x*scale),std::min(scale,5u-y*scale)};
                for (int source=1;source<=4;++source) require(same(footprints.at(id(source)),native),"mixed native footprint mismatch");
            }
        }
        rejects([&] {graph.source_bounds();});rejects([&] {graph.required_source_region({0,0,1,1});});
        f.manifest.output_id=id(6);ExecutableEditGraph scalar(f.manifest,f.bindings,nullptr,cache);
        require(scalar.output_is_coverage(),"coverage output kind missing");
        rejects([&] {scalar.output();});rejects([&] {scalar.output_handle();});
        rejects([&] {renderer.render_image(scalar,RenderRequest{{0,0,7,5},8,{}});});
        for (unsigned mip=0;mip<=2;++mip) {
            const RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
            const auto expected=mip==0?std::span<const std::uint32_t>(mask_graph_reference::coverage_mip0):mip==1?
                std::span<const std::uint32_t>(mask_graph_reference::coverage_mip1):std::span<const std::uint32_t>(mask_graph_reference::coverage_mip2);
            exact(scalar.coverage_output().render_level(scalar.coverage_output().output_bounds(level),level).coverage,expected);
            require(scalar.required_source_regions(scalar.coverage_output().output_bounds(level),level).size()==2,
                    "scalar source footprints missing");
        }
        f.manifest.operations[2].enabled=false;f.manifest.output_id=id(7);
        ExecutableEditGraph disabled(f.manifest,f.bindings);
        exact(renderer.render_image(disabled,{0,0,7,5},8).rgb,mask_graph_reference::base_rgb);
        require(disabled.required_source_regions({0,0,7,5}).size()==1,"disabled masked node retains inactive footprint");
        require(same(disabled.source_bounds(),{0,0,7,5}),"disabled singular source bounds changed");
    }
}

void guards() {
    Fixture f;
    auto invalid=[&](auto change) {auto manifest=f.manifest;change(manifest);rejects([&] {ExecutableEditGraph graph(manifest,f.bindings);});};
    invalid([](auto& m) {m.format_version=3;});
    invalid([](auto& m) {m.processing_version=1;});
    invalid([](auto& m) {m.sources[2].working_space=WorkingSpace::LinearProPhotoD50;});
    invalid([](auto& m) {m.sources[2].demosaic=RawDemosaicIdentity{};});
    invalid([](auto& m) {m.operations[0].inputs["mask"]=id(6);});
    invalid([](auto& m) {m.operations[2].masks["coverage"]=id(7);});
    invalid([](auto& m) {m.operations[0].inputs["mask"]=id(1);});
    invalid([](auto& m) {m.operations[1].inputs["layer"]=id(2);});
    invalid([](auto& m) {m.operations[2].inputs["base"]=id(3);});
    invalid([](auto& m) {m.operations[2].masks["coverage"]=id(1);});
    invalid([](auto& m) {m.operations[2].masks.clear();});
    invalid([](auto& m) {m.operations[2].masks["unknown"]=id(3);});
    invalid([](auto& m) {m.operations[0].parameters["unknown"]=EditValue{1.0};});
    invalid([](auto& m) {m.operations[1].parameters["mode"]=EditValue{std::string("max")};});
    invalid([](auto& m) {m.operations[2].parameters["amount"]=EditValue{1.1};});
    for (unsigned index=0;index<3;++index) for (bool enabled:{false,true}) {
        invalid([&](auto& m) {m.operations[index].enabled=enabled;m.operations[index].schema_version=2;});
        invalid([&](auto& m) {m.operations[index].enabled=enabled;m.operations[index].processing_version=1;});
        invalid([&](auto& m) {m.operations[index].enabled=enabled;m.operations[index].opacity=0.5;});
        invalid([&](auto& m) {m.operations[index].enabled=enabled;m.operations[index].blend_mode="multiply";});
        invalid([&](auto& m) {m.operations[index].enabled=enabled;m.operations[index].extra_fields["future"]=EditValue{true};});
    }
    auto bindings=f.bindings;bindings[2].node=bindings[0].node;
    rejects([&] {ExecutableEditGraph g(f.manifest,bindings);});
    bindings=f.bindings;bindings[0].coverage_node=bindings[2].coverage_node;
    rejects([&] {ExecutableEditGraph g(f.manifest,bindings);});
    bindings=f.bindings;bindings[2].coverage_node=bindings[3].coverage_node;
    rejects([&] {ExecutableEditGraph g(f.manifest,bindings);});
    bindings=f.bindings;++bindings[2].bounds.x;
    rejects([&] {ExecutableEditGraph g(f.manifest,bindings);});
    auto foreign=f.manifest;foreign.operations[0].type_id="future.mask";
    foreign.operations[0].extra_fields["opaque"]=EditValue{true};
    require(parse_edit_manifest(serialize_edit_manifest(foreign))==foreign,"unknown mask fields not preserved");
    rejects([&] {ExecutableEditGraph g(foreign,f.bindings);});
    auto legacy=f.manifest;legacy.format_version=2;legacy.sources.resize(1);legacy.operations.clear();legacy.output_id=id(1);
    ExecutableEditGraph old(parse_edit_manifest(serialize_edit_manifest(legacy)),{f.bindings[0]});
    exact(old.output().render({0,0,7,5}).rgb,mask_graph_reference::base_rgb);
}

void cache_and_history() {
    Fixture f;auto cache=std::make_shared<TileCache>(1<<20);Renderer renderer;
    ExecutableEditGraph first(f.manifest,f.bindings,nullptr,cache);
    const auto initial=renderer.render_image(first,{0,0,7,5},8);const auto warm=cache->stats();
    renderer.render_image(first,{0,0,7,5},8);require(cache->stats().misses==warm.misses,"warm mask graph missed");
    auto next=f.manifest;next.operations[2].parameters["amount"]=EditValue{0.75};
    ExecutableEditGraph changed(next,f.bindings,nullptr,cache);renderer.render_image(changed,{0,0,7,5},8);
    require(cache->stats().misses==warm.misses+1,"consumer change did not reuse RGB and mask upstream");
    next=f.manifest;next.operations[1].parameters["mode"]=EditValue{std::string("add")};
    const auto before=cache->stats();ExecutableEditGraph mask_changed(next,f.bindings,nullptr,cache);
    const auto output=renderer.render_image(mask_changed,{0,0,7,5},8);
    require(output.rgb!=initial.rgb && cache->stats().misses>=before.misses+2,"mask signature failed to invalidate consumer");
    const auto source_rgb=f.bindings[0].node;const auto source_mask=f.bindings[2].coverage_node;
    TileCache separated(4096);std::array<std::uint8_t,32> signature{};signature[0]=99;
    separated.render(*source_rgb,signature,{0,0,1,1});
    auto coverage=separated.render(*source_mask,signature,{0,0,1,1});coverage.coverage[0]=99;
    require(separated.stats().entries==2 && separated.stats().used_bytes==528,"typed cache keys/payload accounting failed");
    require(separated.render(*source_mask,signature,{0,0,1,1}).coverage[0]==0,"returned cache storage aliases cached coverage");
    TileCache shared_budget(520);shared_budget.render(*source_rgb,signature,{0,0,1,1});
    shared_budget.render(*source_mask,signature,{0,0,1,1});
    require(shared_budget.stats().entries==1 && shared_budget.stats().used_bytes==260,"coverage budget is independent/additive");
    EditHistory history(f.manifest,f.bindings,{8,1<<20},nullptr,cache);
    const auto pinned=history.current();const auto revision=history.commit(next);
    const auto saved=history.serialize();auto restored=EditHistory::restore(saved,f.bindings,nullptr,cache);
    require(restored->current()->id==revision,"format4 history revision changed");
    exact(renderer.render_image(*pinned->graph,{0,0,7,5},8).rgb,mask_graph_reference::mixed_mip0);
    require(history.undo()==pinned->id && history.redo()==revision,"mask history navigation changed");
    f.manifest.output_id=id(6);const auto coverage_revision=history.commit(f.manifest);
    auto scalar_restore=EditHistory::restore(history.serialize(),f.bindings,nullptr,cache);
    require(scalar_restore->current()->id==coverage_revision && scalar_restore->current()->graph->output_is_coverage(),
            "scalar history output kind lost");
    auto bad_source=f.manifest;bad_source.sources[2].content_sha256[0]^=1;
    rejects([&] {history.commit(bad_source);});
}

class FaultCoverage final:public CoverageNode {
public:
    explicit FaultCoverage(std::shared_ptr<const CoverageNode> input):input_(std::move(input)) {}
    mutable std::atomic<bool> fail{true};
    CoverageTile render_level(Rect bounds,RenderLevel level) const override {
        if (fail.exchange(false)) throw std::bad_alloc();
        return input_->render_level(bounds,level);
    }
    bool supports_level(RenderLevel level) const noexcept override {return input_->supports_level(level);}
    Rect native_bounds() const noexcept override {return input_->native_bounds();}
    std::optional<std::array<std::uint8_t,32>> source_fingerprint() const override {return input_->source_fingerprint();}
    Rect required_native_region(Rect output,RenderLevel level) const override {return input_->required_native_region(output,level);}
private:std::shared_ptr<const CoverageNode> input_;
};

void jobs() {
    Fixture f;ExecutableEditGraph graph(f.manifest,f.bindings);TileScheduler scheduler(1,8);
    for (unsigned mip=0;mip<=2;++mip) {
        const RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
        const Rect bounds{0,0,mip==0?7u:mip==1?4u:2u,mip==0?5u:mip==1?3u:2u};
        auto job=scheduler.submit(graph.output_handle(),graph.output_bounds(),RenderRequest{bounds,2,level});
        exact(job.get().rgb,mip==0?std::span<const std::uint32_t>(mask_graph_reference::mixed_mip0):mip==1?
              std::span<const std::uint32_t>(mask_graph_reference::mixed_mip1):std::span<const std::uint32_t>(mask_graph_reference::mixed_mip2));
    }
    auto fault=std::make_shared<FaultCoverage>(f.bindings[2].coverage_node);f.bindings[2].coverage_node=fault;
    ExecutableEditGraph failed(f.manifest,f.bindings);
    auto bad=scheduler.submit(failed.output_handle(),failed.output_bounds(),RenderRequest{{0,0,7,5},8,{}});
    auto recovered=scheduler.submit(failed.output_handle(),failed.output_bounds(),RenderRequest{{0,0,7,5},8,{}});
    rejects<std::bad_alloc>([&] {bad.get();});exact(recovered.get().rgb,mask_graph_reference::mixed_mip0);
}

class DamagedCoverage final:public CoverageNode {
public:
    DamagedCoverage(std::shared_ptr<const CoverageNode> input,int damage):input_(std::move(input)),damage_(damage) {}
    CoverageTile render_level(Rect bounds,RenderLevel level) const override {
        auto tile=input_->render_level(bounds,level);
        if (damage_==0) ++tile.bounds.x;
        if (damage_==1) tile.coverage.pop_back();
        if (damage_==2) tile.coverage.back()=std::numeric_limits<float>::quiet_NaN();
        if (damage_==3) tile.coverage.back()=1.1f;
        return tile;
    }
    bool supports_level(RenderLevel level) const noexcept override {return input_->supports_level(level);}
    Rect native_bounds() const noexcept override {return input_->native_bounds();}
private:std::shared_ptr<const CoverageNode> input_;int damage_;
};

void actual_tile_guards() {
    Fixture f;const auto source=f.bindings[2].coverage_node;
    const Rect full{0,0,7,5};std::array<std::uint8_t,32> signature{};
    for (int damage=0;damage<4;++damage) {
        auto bad=std::make_shared<DamagedCoverage>(source,damage);
        rejects<std::exception>([&] {CoverageInvertNode(bad).render(full);});
        rejects<std::exception>([&] {CoverageCombineNode(source,bad,MaskCombineMode::Add).render(full);});
        rejects<std::exception>([&] {MaskedMixNode(f.bindings[0].node,f.bindings[1].node,bad,full,0).render(full);});
        TileCache cache(4096);rejects<std::exception>([&] {cache.render(*bad,signature,full);});
        require(cache.stats().entries==0,"invalid coverage was cached");
    }
    rejects([&] {CoverageInvertNode(nullptr);});
    rejects([&] {CoverageCombineNode(source,nullptr,MaskCombineMode::Add);});
    auto wrong=std::make_shared<CoverageRasterNode>(CoverageImage({{1,0,7,5},0},decoded(mask_graph_reference::mask_a)));
    rejects([&] {CoverageCombineNode(source,wrong,MaskCombineMode::Add);});
    rejects([&] {MaskedMixNode(f.bindings[0].node,f.bindings[1].node,wrong,full,1);});
    for (RenderLevel level: {RenderLevel{1,RenderQuality::Final},RenderLevel{3,RenderQuality::Preview},
                            RenderLevel{0,static_cast<RenderQuality>(99)}}) {
        CoverageInvertNode inverse(source);require(!inverse.supports_level(level),"unsupported mask level admitted");
        rejects([&] {inverse.render_level({0,0,1,1},level);});
    }
    auto base=f.bindings[0].node->render(full),layer=f.bindings[1].node->render(full);
    auto mask=source->render(full);
    for (int damage=0;damage<3;++damage) {
        auto bad=layer;
        if (damage==0) ++bad.bounds.x;
        if (damage==1) bad.rgb.pop_back();
        if (damage==2) bad.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
        rejects<std::exception>([&] {masked_mix_rgb(base,bad,mask,0);});
    }
    for (double amount:{-0.1,1.1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        rejects([&] {masked_mix_rgb(base,layer,mask,amount);});
}

class BlockingCoverage final:public CoverageNode {
public:
    explicit BlockingCoverage(std::shared_ptr<const CoverageNode> input):input_(std::move(input)),release_(release.get_future().share()) {}
    mutable std::promise<void> entered;
    std::promise<void> release;
    CoverageTile render_level(Rect bounds,RenderLevel level) const override {
        entered.set_value();release_.wait();return input_->render_level(bounds,level);
    }
    bool supports_level(RenderLevel level) const noexcept override {return input_->supports_level(level);}
    Rect native_bounds() const noexcept override {return input_->native_bounds();}
private:std::shared_ptr<const CoverageNode> input_;std::shared_future<void> release_;
};

void generation_and_lifetime() {
    Fixture f;TileCache cache(4096);std::array<std::uint8_t,32> signature{};
    BlockingCoverage blocked(f.bindings[2].coverage_node);auto entered=blocked.entered.get_future();
    auto pending=std::async(std::launch::async,[&] {return cache.render(blocked,signature,{0,0,1,1});});
    const bool started=entered.wait_for(std::chrono::seconds(10))==std::future_status::ready;
    if (started) cache.clear();blocked.release.set_value();
    require(started,"coverage render did not enter test gate");
    require(pending.get().coverage[0]==0 && cache.stats().entries==0,"old generation repopulated coverage cache");
    cache.render(*f.bindings[2].coverage_node,signature,{0,0,1,1});
    require(cache.stats().entries==1,"fresh generation failed coverage admission");
    std::shared_ptr<const EditRevision> pinned;
    {
        Fixture owned;owned.manifest.output_id=id(6);
        EditHistory history(owned.manifest,owned.bindings,{8,4096});pinned=history.current();
    }
    exact(pinned->graph->coverage_output().render({0,0,7,5}).coverage,mask_graph_reference::coverage_mip0);
}

void dependency_variants() {
    Fixture f;Renderer renderer;auto cache=std::make_shared<TileCache>(1<<20);
    ExecutableEditGraph original(f.manifest,f.bindings,nullptr,cache);
    const auto initial=renderer.render_image(original,{0,0,7,5},8);
    auto changed=f;auto samples=decoded(mask_graph_reference::mask_a);samples[0]=1;
    auto replacement=std::make_shared<CoverageRasterNode>(CoverageImage({{0,0,7,5},0},samples));
    changed.manifest.sources[2].content_sha256=*replacement->source_fingerprint();
    changed.bindings[2]={changed.manifest.sources[2],nullptr,{0,0,7,5},replacement};
    ExecutableEditGraph new_source(changed.manifest,changed.bindings,nullptr,cache);
    require(renderer.render_image(new_source,{0,0,7,5},8).rgb!=initial.rgb,"coverage content reused stale consumer");
    auto aliases=f;aliases.manifest.sources[3].content_sha256=aliases.manifest.sources[2].content_sha256;
    aliases.bindings[3]={aliases.manifest.sources[3],nullptr,{0,0,7,5},aliases.bindings[2].coverage_node};
    ExecutableEditGraph same_node(aliases.manifest,aliases.bindings,nullptr,cache);
    const auto regions=same_node.required_source_regions({2,1,1,1});
    require(regions.size()==4 && same(regions.at(id(3)),regions.at(id(4))),"runtime alias lost distinct coverage source IDs");
    f.manifest.output_id=id(6);f.manifest.operations[1].enabled=false;
    ExecutableEditGraph bypass_combine(f.manifest,f.bindings);
    require(bypass_combine.required_source_regions({0,0,7,5}).size()==1,"disabled combine retains layer footprint");
    auto inverse=CoverageInvertNode(f.bindings[2].coverage_node).render({0,0,7,5});
    require(bypass_combine.coverage_output().render({0,0,7,5}).coverage==inverse.coverage,"disabled combine did not alias base");
    f.manifest.operations[0].enabled=false;
    ExecutableEditGraph bypass_both(f.manifest,f.bindings);
    exact(bypass_both.coverage_output().render({0,0,7,5}).coverage,mask_graph_reference::mask_a);
    require(bypass_both.required_source_regions({0,0,7,5}).size()==1,"disabled invert retains operation footprint");
}

void crop_halo_and_raw_formats() {
    Fixture f;const auto d=domain(f.manifest.working_space);
    auto blur=operation(8,"rawengine.box_blur",d);blur.inputs={{"image",id(7)}};
    blur.parameters={{"radius",EditValue{std::int64_t{1}}}};
    auto crop=operation(9,"rawengine.crop",d);crop.inputs={{"image",id(8)}};
    crop.parameters={{"x",EditValue{std::int64_t{1}}},{"y",EditValue{std::int64_t{1}}},
                     {"width",EditValue{std::int64_t{5}}},{"height",EditValue{std::int64_t{3}}}};
    f.manifest.operations.push_back(blur);f.manifest.operations.push_back(crop);f.manifest.output_id=id(9);
    ExecutableEditGraph composed(f.manifest,f.bindings);
    auto original=f.manifest;original.operations.resize(3);original.output_id=id(7);
    ExecutableEditGraph input(original,f.bindings);
    auto direct_blur=std::make_shared<BoxBlurNode>(input.output_handle(),Rect{0,0,7,5},1);
    CropNode direct(direct_blur,{0,0,7,5},{1,1,5,3});Renderer renderer;
    for (unsigned mip=0;mip<=2;++mip) {
        const RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
        const Rect bounds{0,0,mip==0?5u:mip==1?3u:2u,mip==0?3u:mip==1?2u:1u};
        const auto expected=direct.render_level(bounds,level);
        for (unsigned size:{1u,2u,8u})
            require(renderer.render_image(composed,RenderRequest{bounds,size,level}).rgb==expected.rgb,"mask crop/halo partition differs");
    }
    for (unsigned mip=0;mip<=1;++mip) {
        const RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};
        const auto regions=composed.required_source_regions({0,0,1,1},level);
        const Rect expected=mip?Rect{0,0,4,4}:Rect{0,0,3,3};
        require(regions.size()==4,"crop/halo lost mask dependencies");
        for (int source=1;source<=4;++source) require(same(regions.at(id(source)),expected),"crop/halo did not compose mask native footprint");
    }
    RawMetadata metadata;metadata.width=7;metadata.height=5;metadata.white_levels.fill(65535);
    RawImage image(metadata,std::vector<std::uint16_t>(35,12345));auto raw=std::make_shared<RawUnpackNode>(image);
    EditSource source;source.id=id(20);source.kind=EditSourceKind::DecodedBayerU16;
    source.content_sha256=image.fingerprint();source.demosaic=RawDemosaicIdentity{};
    EditManifest manifest;manifest.format_version=3;manifest.sources={source};manifest.output_id=source.id;
    ExecutableEditGraph version3(manifest,{{source,raw,{0,0,7,5}}});manifest.format_version=4;
    ExecutableEditGraph version4(parse_edit_manifest(serialize_edit_manifest(manifest)),{{source,raw,{0,0,7,5}}});
    require(version3.output().render({0,0,7,5}).rgb==version4.output().render({0,0,7,5}).rgb,"format4 RAW policy changed output");
    manifest.sources[0].demosaic.reset();rejects([&] {validate_edit_manifest(manifest);});
}
} // namespace

int main() {
    try {
        numeric();frames_and_planning();guards();cache_and_history();jobs();
        actual_tile_guards();generation_and_lifetime();dependency_variants();crop_halo_and_raw_formats();
        std::cout << "504 scalar fixtures,3 frozen chain levels,two-space graph/ROI/cache/guards/history/jobs passed\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
