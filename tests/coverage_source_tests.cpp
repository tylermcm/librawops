#include "CoverageSource.hpp"
#include "reference/coverage_source_v1.hpp"

#include <algorithm>
#include <bit>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace rawengine;

namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template<class Callback> void rejects(Callback callback) {
    try { callback(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected invalid_argument did not occur");
}

bool same(Rect a, Rect b) {
    return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
}

std::vector<float> decoded(std::span<const std::uint32_t> bits) {
    std::vector<float> values;
    for (auto word : bits) values.push_back(std::bit_cast<float>(word));
    return values;
}

void exact(const std::vector<float>& actual, std::span<const std::uint32_t> expected) {
    require(actual.size()==expected.size(),"fixture sample count changed");
    for (std::size_t i=0;i<actual.size();++i)
        require(std::bit_cast<std::uint32_t>(actual[i])==expected[i],"coverage fixture bits differ");
}

Rect brute_footprint(Rect native, Rect output, RenderLevel level) {
    if (level.mip==0) return output;
    // Enumerate individual native cells selected by every reduced output pixel.
    // This independent support oracle avoids reusing the node's rectangle mapping.
    std::uint32_t xmin=UINT32_MAX,ymin=UINT32_MAX,xmax=0,ymax=0;
    const std::uint64_t scale=1ull<<level.mip;
    for (std::uint32_t y=0;y<native.height;++y) {
        for (std::uint32_t x=0;x<native.width;++x) {
            const auto u=x/scale,v=y/scale;
            if (u<output.x || v<output.y || u>=static_cast<std::uint64_t>(output.x)+output.width ||
                v>=static_cast<std::uint64_t>(output.y)+output.height) continue;
            xmin=std::min(xmin,x);ymin=std::min(ymin,y);
            xmax=std::max(xmax,x);ymax=std::max(ymax,y);
        }
    }
    require(xmin!=UINT32_MAX,"empty oracle footprint");
    return {native.x+xmin,native.y+ymin,xmax-xmin+1,ymax-ymin+1};
}

std::size_t all_sources() {
    std::size_t rois=0;
    for (const auto& fixture : coverage_source_reference::cases) {
        const Rect native{fixture.x,fixture.y,fixture.width,fixture.height};
        auto caller=decoded(fixture.input);
        CoverageImage image({native,fixture.stride},caller);
        require(image.metadata().row_stride_samples==fixture.width,"owned stride must be packed");
        require(same(image.metadata().bounds,native),"source origin/extent changed");
        exact(image.samples(),fixture.native);
        require(image.fingerprint()==fixture.digest && fingerprint_coverage_source(image)==fixture.digest,
                "independent canonical source digest mismatch");
        auto copy=image;
        require(copy.samples().data()==image.samples().data(),"immutable image copy did not share storage");
        // Retained caller storage and pointers must have no effect on the snapshot.
        std::fill(caller.begin(),caller.end(),-99.0f);
        exact(image.samples(),fixture.native);
        CoverageSourceNode source(copy);
        require(source.source_fingerprint()==fixture.digest,"node source identity changed");
        for (auto level : {RenderLevel{},RenderLevel{0,RenderQuality::Preview},
                           RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            require(source.supports_level(level),"valid source level rejected");
            const auto bounds=source.output_bounds(level);
            const auto expected=level.mip==0?fixture.native:level.mip==1?fixture.mip1:fixture.mip2;
            require(expected.size()==static_cast<std::size_t>(bounds.width)*bounds.height,
                    "level extent differs from fixture");
            require(level.mip==0?same(bounds,native):(bounds.x==0 && bounds.y==0),
                    "level origin convention changed");
            auto full=source.render_level(bounds,level);
            require(same(full.bounds,bounds),"returned bounds changed");
            exact(full.coverage,expected);
            validate_coverage_tile(full);
            // Every rectangular ROI for the bounded fixtures, including singleton edges.
            for (std::uint32_t y=0;y<bounds.height;++y) {
                for (std::uint32_t x=0;x<bounds.width;++x) {
                    for (std::uint32_t h=1;h<=bounds.height-y;++h) {
                        for (std::uint32_t w=1;w<=bounds.width-x;++w) {
                            const Rect roi{bounds.x+x,bounds.y+y,w,h};
                            require(same(source.required_native_region(roi,level),
                                         brute_footprint(native,roi,level)),"native support mismatch");
                            const auto tile=source.render_level(roi,level);
                            require(same(tile.bounds,roi),"ROI bounds changed");
                            for (std::uint32_t v=0;v<h;++v)
                                for (std::uint32_t u=0;u<w;++u)
                                    require(std::bit_cast<std::uint32_t>(tile.coverage[v*w+u])==
                                            expected[(y+v)*bounds.width+x+u],"ROI/tile partition mismatch");
                            ++rois;
                        }
                    }
                }
            }
            full.coverage[0]=-77;
            exact(source.render_level(bounds,level).coverage,expected);
        }
    }
    return rois;
}

void identity_and_lifetime() {
    const Rect bounds{11,17,2,2};
    const std::vector<float> pixels{0,-0.0f,0.25f,1};
    const CoverageImage a({bounds,0},pixels);
    const CoverageImage positive({bounds,2},{0,0,0.25f,1});
    require(a.fingerprint()==positive.fingerprint(),"zero sign affected canonical identity");
    const CoverageImage padded({bounds,3},{0,-0.0f,99,0.25f,1,-99});
    require(a.fingerprint()==padded.fingerprint(),"padding/stride affected identity");
    const CoverageImage shifted({{12,17,2,2},2},pixels);
    require(a.fingerprint()!=shifted.fingerprint(),"origin omitted from source identity");
    const CoverageImage reshaped({{11,17,1,4},1},pixels);
    require(a.fingerprint()!=reshaped.fingerprint(),"dimensions omitted from identity");
    auto changed=pixels;changed[2]=std::bit_cast<float>(0x3e800001u);
    const CoverageImage different({bounds,2},changed);
    require(a.fingerprint()!=different.fingerprint(),"visible bit omitted from identity");
    const auto source=[] {
        std::vector<float> temporary{0,0.5f,1};
        CoverageImage image({{31,47,3,1},3},temporary);
        CoverageSourceNode node(image);
        temporary.clear();
        return node;
    }();
    exact(source.render({31,47,3,1}).coverage,std::array<std::uint32_t,3>{0,0x3f000000,0x3f800000});
}

void admission() {
    for (auto word : {0x80000001u,0xbf800000u,0x3f800001u,0x7f800000u,0xff800000u,0x7fc00000u})
        rejects([&] { CoverageImage image({{0,0,1,1},1},{std::bit_cast<float>(word)}); });
    rejects([] { CoverageImage image({{0,0,0,1},0},{}); });
    rejects([] { CoverageImage image({{UINT32_MAX,0,1,1},1},{0}); });
    rejects([] { CoverageImage image({{0,UINT32_MAX,1,1},1},{0}); });
    rejects([] { CoverageImage image({{0,0,2,1},1},{0}); });
    rejects([] { CoverageImage image({{0,0,2,1},2},{0}); });
    rejects([] { CoverageImage image({{0,0,1,1},1},{0,1}); });
    rejects([] { CoverageImage image({{0,0,1,UINT32_MAX},UINT32_MAX},{}); });
    CoverageSourceNode source(CoverageImage({{11,17,7,5},7},std::vector<float>(35,0.5f)));
    for (auto level : {RenderLevel{1,RenderQuality::Final},RenderLevel{2,RenderQuality::Final},
                       RenderLevel{3,RenderQuality::Preview},RenderLevel{UINT32_MAX,RenderQuality::Preview},
                       RenderLevel{0,static_cast<RenderQuality>(99)}}) {
        require(!source.supports_level(level),"unsupported level admitted");
        rejects([&] { source.output_bounds(level); });
        rejects([&] { source.required_native_region({0,0,1,1},level); });
        rejects([&] { source.render_level({0,0,1,1},level); });
    }
    for (auto level : {RenderLevel{},RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
        const auto bounds=source.output_bounds(level);
        for (auto bad : {Rect{bounds.x,bounds.y,0,1},Rect{bounds.x,bounds.y,1,0},
                         Rect{bounds.x,bounds.y,bounds.width+1,1},
                         Rect{bounds.x+bounds.width,bounds.y,1,1},
                         Rect{UINT32_MAX,UINT32_MAX,2,2}}) {
            rejects([&] { source.required_native_region(bad,level); });
            rejects([&] { source.render_level(bad,level); });
        }
    }
    rejects([&] { source.render({0,0,1,1}); });
}

void concurrent_snapshots() {
    const auto& fixture=coverage_source_reference::cases[8];
    const CoverageImage fresh({{fixture.x,fixture.y,fixture.width,fixture.height},fixture.stride},
                              decoded(fixture.input));
    std::promise<void> start;
    const auto gate=start.get_future().share();
    std::vector<std::future<void>> workers;
    for (int i=0;i<8;++i) {
        workers.push_back(std::async(std::launch::async,[fresh,gate,&fixture] {
            CoverageSourceNode node(fresh);
            gate.wait();
            require(fresh.fingerprint()==fixture.digest,"concurrent first fingerprint mismatch");
            for (auto level : {RenderLevel{},RenderLevel{1,RenderQuality::Preview},
                               RenderLevel{2,RenderQuality::Preview}}) {
                exact(node.render_level(node.output_bounds(level),level).coverage,
                      level.mip==0?fixture.native:level.mip==1?fixture.mip1:fixture.mip2);
                require(node.source_fingerprint()==fixture.digest,"concurrent fingerprint replay changed");
            }
        }));
    }
    start.set_value();
    for (auto& worker : workers) worker.get();
}

} // namespace

int main() {
    try {
        const auto rois=all_sources();
        identity_and_lifetime();admission();concurrent_snapshots();
        std::cout << "20 independent coverage sources; " << rois << " exact ROI/footprint checks; "
                     "canonical identity,admission,ownership,lifetime and8-worker replay passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
