#include "SpatialOps.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>

using namespace rawengine;
namespace {
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
template<class E=std::invalid_argument,class F> void rejects(F action) {
    try { action(); } catch (const E&) { return; }
    throw std::runtime_error("expected spatial exception");
}
class Source final:public Node {
public:
    bool malformed=false,nonfinite=false;
    mutable Rect last;
    ImageDescriptor output_descriptor() const noexcept override { return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50); }
    Tile render(Rect r) const override {
        last=r;
        Tile tile{r,{},output_descriptor()};
        for (std::uint32_t y=0;y<r.height;++y) for (std::uint32_t x=0;x<r.width;++x) {
            tile.rgb.push_back(float(std::int64_t(r.x)+x-12));
            tile.rgb.push_back(float(std::int64_t(r.y)+y-10));
            tile.rgb.push_back(nonfinite ? std::numeric_limits<float>::quiet_NaN() : 2.0f);
        }
        if (malformed) tile.rgb.pop_back();
        return tile;
    }
};
LocalStatisticsTile collected(const Node& node,Rect image,std::uint32_t size,unsigned radius) {
    LocalStatisticsTile result{image,node.output_descriptor(),radius,
        std::vector<double>(std::size_t(image.width)*image.height*3),
        std::vector<double>(std::size_t(image.width)*image.height*3),
        std::vector<std::uint32_t>(std::size_t(image.width)*image.height*3)};
    analyze_local_rgb(node,image,RenderRequest{image,size},[&](const auto& tile) {
        for (unsigned y=0;y<tile.bounds.height;++y) for (unsigned x=0;x<tile.bounds.width;++x)
            for (unsigned c=0;c<3;++c) {
                const auto source=(std::size_t(y)*tile.bounds.width+x)*3+c;
                const auto target=((std::size_t(tile.bounds.y-image.y)+y)*image.width+tile.bounds.x-image.x+x)*3+c;
                result.mean[target]=tile.mean[source]; result.variance[target]=tile.variance[source];
                result.finite_count[target]=tile.finite_count[source];
            }
    },radius);
    return result;
}
void statistics_truth_and_tiles() {
    Source source;
    Rect image{10,7,7,5};
    for (unsigned radius:{0u,1u,3u,8u}) {
        auto full=collected(source,image,256,radius);
        for (unsigned y=0;y<5;++y) for (unsigned x=0;x<7;++x) {
            const auto left=x>radius ? x-radius : 0, right=std::min(6u,x+radius);
            const auto top=y>radius ? y-radius : 0, bottom=std::min(4u,y+radius);
            const auto i=(std::size_t(y)*7+x)*3;
            const auto nx=right-left+1,ny=bottom-top+1;
            require(std::abs(full.mean[i]-(double(left+right)/2-2))<1e-12,"horizontal affine mean differs");
            require(std::abs(full.mean[i+1]-(double(top+bottom)/2-3))<1e-12,"vertical affine mean differs");
            require(std::abs(full.variance[i]-(double(nx)*nx-1)/12)<1e-12,"horizontal population variance differs");
            require(std::abs(full.variance[i+1]-(double(ny)*ny-1)/12)<1e-12,"vertical population variance differs");
            require(full.mean[i+2]==2 && full.variance[i+2]==0 && full.finite_count[i]==nx*ny,"constant/coverage differs");
        }
        for (unsigned size:{1u,2u,4u}) {
            const auto tiled=collected(source,image,size,radius);
            require(tiled.mean==full.mean && tiled.variance==full.variance && tiled.finite_count==full.finite_count,
                    "local statistics tile arithmetic differs");
        }
    }
    // An interior 1x1 ROI still reads nine neighbors, not one ROI-clipped sample.
    analyze_local_rgb(source,image,RenderRequest{{12,9,1,1},1},[&](const auto& tile) {
        require(tile.finite_count[0]==9 && tile.mean[0]==0 && std::abs(tile.variance[0]-2.0/3)<1e-12,
                "analysis clipped the ROI instead of the true image boundary");
    },1);
    require(source.last.width==3 && source.last.height==3,"interior halo missing");
    source.nonfinite=true;
    auto missing=collected(source,image,3,1);
    for (std::size_t i=2;i<missing.mean.size();i+=3)
        require(missing.finite_count[i]==0 && std::isnan(missing.mean[i]) && std::isnan(missing.variance[i]),"empty finite window differs");
    source.nonfinite=false;
    auto a=std::async(std::launch::async,[&] { Source own; return collected(own,image,2,3); });
    Source own;
    const auto b=collected(own,image,4,3);
    require(a.get().variance==b.variance,"independent concurrent analyses differ");
}
void numerical_stability() {
    const float base=1e20f,next=std::nextafter(base,std::numeric_limits<float>::infinity());
    const double delta=double(next)-base;
    Tile input{{0,0,3,1},{base,base,base,next,next,next,base,base,base},ImageDescriptor::camera_linear()};
    const auto output=local_statistics_rgb(input,{1,0,1,1},input.bounds,1);
    require(std::abs(output.variance[0]-delta*delta*2/9)/(delta*delta*2/9)<1e-8,"high-offset variance lost precision");
    const auto inf=std::numeric_limits<float>::infinity();
    input.rgb={-inf,0,1,inf,2,1,std::numeric_limits<float>::quiet_NaN(),4,1};
    const auto finite=local_statistics_rgb(input,{1,0,1,1},input.bounds,1);
    require(finite.finite_count[0]==0 && std::isnan(finite.mean[0]) && finite.finite_count[1]==3 && finite.mean[1]==2,
            "finite-channel exclusion differs");
}
void convolution_semantics() {
    auto source=std::make_shared<Source>();
    const Rect bounds{10,7,7,5};
    ConvolutionNode derivative(source,bounds,{3,1,{1,0,-1}});
    const auto full=Renderer{}.render_image(derivative,bounds,RenderRequest{bounds,256});
    for (unsigned y=0;y<5;++y) for (unsigned x=0;x<7;++x) {
        const auto i=(std::size_t(y)*7+x)*3;
        require(full.rgb[i]==(x==0 || x==6 ? 1.0f : 2.0f),"convolution must flip the kernel and replicate true borders");
        require(full.rgb[i+1]==0 && full.rgb[i+2]==0,"derivative channel differs");
    }
    require(Renderer{}.render_image(derivative,bounds,RenderRequest{bounds,2}).rgb==full.rgb,"convolution seams differ");
    auto first=std::make_shared<ConvolutionNode>(source,bounds,ConvolutionKernel{3,1,{0.25,0.5,0.25}});
    ConvolutionNode second(first,bounds,{1,3,{0.25,0.5,0.25}});
    analyze_local_rgb(second,bounds,RenderRequest{{12,9,1,1},1},[&](const auto& tile) {
        require(tile.finite_count[0]==9,"composed convolution/statistics window differs");
    },1);
    require(source->last.x==10 && source->last.y==7 && source->last.width==5 && source->last.height==5,
            "statistics and convolution support did not compose");
    RasterImage raster({1,1,0,WorkingSpace::LinearProPhotoD50},{-0.0f,-2,3});
    auto one=std::make_shared<RasterSourceNode>(raster);
    ConvolutionNode identity(one,{0,0,1,1},{3,3,{0,0,0,0,1,0,0,0,0}});
    require(std::signbit(identity.render({0,0,1,1}).rgb[0]),"delta kernel must preserve signed zero");
    ConvolutionNode large(one,{0,0,1,1},{17,1,std::vector<double>(17,1)});
    require(large.render({0,0,1,1}).rgb[1]==-34,"singleton border replication differs");
}
void validation_cancellation() {
    Source source; const Rect image{10,7,7,5};
    rejects([&] { local_statistics_region(image,image,9); });
    rejects([&] { analyze_local_rgb(source,image,RenderRequest{image,0},[](auto&){}); });
    rejects([&] { analyze_local_rgb(source,image,RenderRequest{image},{}); });
    rejects<std::out_of_range>([&] { local_statistics_region({9,7,1,1},image,1); });
    rejects([&] { local_statistics_region({0xffffffffu,0,2,1},{0xffffffffu,0,2,1},1); });
    source.malformed=true;
    rejects([&] { collected(source,image,3,1); }); source.malformed=false;
    CancellationToken token; token.cancel();
    rejects<RenderCancelled>([&] { analyze_local_rgb(source,image,RenderRequest{image},[](auto&){},1,&token); });
    CancellationToken last; unsigned callbacks=0;
    rejects<RenderCancelled>([&] { analyze_local_rgb(source,image,RenderRequest{image},[&](auto&) { ++callbacks; last.cancel(); },1,&last); });
    require(callbacks==1,"final callback cancellation not detected");
    rejects<std::runtime_error>([&] { analyze_local_rgb(source,image,RenderRequest{image,1},[](auto&) { throw std::runtime_error("consumer stopped"); }); });
    for (const auto& kernel:{ConvolutionKernel{0,1,{}},{2,1,{1,1}},{19,1,std::vector<double>(19)},
                            {3,1,{1}},{1,1,{65537}},{1,1,{std::numeric_limits<double>::infinity()}}})
        rejects([&] { validate_convolution_kernel(kernel); });
    auto malformed=std::make_shared<Source>(); malformed->malformed=true;
    ConvolutionNode conv(malformed,image,{});
    rejects([&] { conv.render(image); });
    auto huge=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,WorkingSpace::LinearProPhotoD50},
        std::vector<float>(3,std::numeric_limits<float>::max())));
    ConvolutionNode overflow(huge,{0,0,1,1},{1,1,{2}});
    rejects([&] { overflow.render({0,0,1,1}); });
}
} // namespace
int main() {
    try { statistics_truth_and_tiles(); numerical_stability(); convolution_semantics(); validation_cancellation();
        std::cout<<"Spatial operations tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
