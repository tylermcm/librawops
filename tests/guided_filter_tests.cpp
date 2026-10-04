#include "SpatialOps.hpp"
#include "EditGraph.hpp"
#include <cmath>
#include <cfenv>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#define GUIDED_TEST_MXCSR 1
#endif
#ifdef _MSC_VER
#pragma fenv_access(on)
#endif
using namespace rawengine;
namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F f) {try {f();}catch(const std::exception&) {return;}throw std::runtime_error("expected guided-filter rejection");}
bool same(const std::vector<float>& a,const std::vector<float>& b) {return a.size()==b.size()&&!std::memcmp(a.data(),b.data(),a.size()*sizeof(float));}
bool rect_same(Rect a,Rect b) {return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;}
class Fixture final:public Node {
public:
    Rect image{17,29,7,5};mutable Rect requested{};unsigned fault=0;
    WorkingSpace space=WorkingSpace::LinearProPhotoD50;
    ImageDescriptor output_descriptor() const noexcept override {return ImageDescriptor::scene_linear(space);}
    Tile render(Rect r) const override {
        requested=r;Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3),output_descriptor()};
        for(unsigned y=0;y<r.height;++y)for(unsigned x=0;x<r.width;++x)for(unsigned c=0;c<3;++c)
            t.rgb[(std::size_t(y)*r.width+x)*3+c]=float((std::uint64_t(r.x)-image.x+x+2*(std::uint64_t(r.y)-image.y+y)+c)%7)/8;
        if(fault==1)t.bounds.x++;
        if(fault==2)t.rgb.pop_back();
        if(fault==3)t.rgb.push_back(0);
        if(fault==4)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
        if(fault==5)t.descriptor.format=static_cast<PixelFormat>(99);
        if(fault>=6)t.rgb.front()=fault==6 ? std::numeric_limits<float>::quiet_NaN() : fault==7 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
        return t;
    }
};
void math_and_node() {
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        std::vector<float> impulse(15,0);for(unsigned c=0;c<3;++c)impulse[6+c]=1;
        auto input=std::make_shared<RasterSourceNode>(RasterImage({5,1,0,space},impulse));
        WorkingYGuidedFilterSettings settings{1,1};WorkingYGuidedFilterNode node(input,{0,0,5,1},settings);settings.radius=0;settings.epsilon=0;
        const auto result=node.render({0,0,5,1});
        // Exact expanded neutral impulse kernel for epsilon1, actual border counts:
        // a=2/11 in each three-sample nonzero window; q=[3/22,2/11,5/11,2/11,3/22].
        const float expected[]{float(3./22),float(2./11),float(5./11),float(2./11),float(3./22)};
        for(unsigned x=0;x<5;++x)for(unsigned c=0;c<3;++c)require(result.rgb[x*3+c]==expected[x],"independent rational clipped impulse/copy truth");
        require(result.rgb[0]>0 && rect_same(node.input_region({0,0,1,1},{0,0,5,1}),{0,0,3,1}),"two-radius support");
        for(unsigned x=0;x<5;++x)require(!std::memcmp(node.render({x,0,1,1}).rgb.data(),result.rgb.data()+3*x,12),"native ROI bits");
        require(node.input_node()==input.get() && node.output_descriptor()==input->output_descriptor(),"immutable ownership/descriptor");
        auto a=std::async(std::launch::async,[&]{return node.render({0,0,5,1});});
        auto b=std::async(std::launch::async,[&]{return node.render({0,0,5,1});});require(same(a.get().rgb,b.get().rgb),"concurrent immutable node");
        for(unsigned mip:{1u,2u}) {
            RenderLevel level{mip,RenderQuality::Preview};const unsigned width=(5+(1u<<mip)-1)/(1u<<mip);Rect r{0,0,width,1};
            auto reduced=input->render_level(r,level);require(same(node.render_level(r,level).rgb,working_y_guided_filter_rgb(reduced,r,r,{1,1}).rgb),"upstream requested-level filtering");
        }
        rejects([&]{node.render_level({0,0,3,1},{1,RenderQuality::Final});});
        rejects([&]{node.render_level({0,0,1,1},{3,RenderQuality::Preview});});
        rejects([&]{node.input_region({0,0,1,1},{0,0,4,1});});
        rejects([&]{node.render({5,0,1,1});});
        const float maximum=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
        for(auto values:{std::vector<float>{-0.f,0.f,-0.f},std::vector<float>{maximum,maximum,maximum},std::vector<float>{-maximum,maximum,-maximum},std::vector<float>{tiny,tiny,tiny},std::vector<float>{-.5f,.25f,2.f}}) {
            auto constant=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,space},values));
            for(unsigned radius:{0u,1u,8u})for(double epsilon:{0x1p-24,65536.})require(same(WorkingYGuidedFilterNode(constant,{0,0,1,1},{radius,epsilon}).render({0,0,1,1}).rgb,values),"constant/extreme/signed-zero exact bits");
        }
    }
}
void validation() {
    auto source=std::make_shared<Fixture>();const Rect roi{20,31,1,1};
    for(unsigned radius:{0u,1u,8u}) {
        WorkingYGuidedFilterNode node(source,source->image,{radius,0x1p-12});node.render(roi);
        require(rect_same(source->requested,working_y_guided_filter_region(roi,source->image,radius)),"complete finite source request");
        for(unsigned fault=1;fault<=8;++fault) {source->fault=fault;rejects([&]{node.render(roi);});}source->fault=0;
    }
    WorkingYGuidedFilterNode pinned(source,source->image);source->space=WorkingSpace::LinearRec2020D65;
    require(pinned.output_descriptor()==ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50),"descriptor pinned at construction");rejects([&]{pinned.render(roi);});source->space=WorkingSpace::LinearProPhotoD50;
    for(double epsilon:{0.,-0.,-1.,std::nextafter(0x1p-24,0.),std::nextafter(65536.,std::numeric_limits<double>::infinity()),std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})rejects([&]{validate_working_y_guided_filter_settings({0,epsilon});});
    for(unsigned radius:{9u,UINT32_MAX})rejects([&]{validate_working_y_guided_filter_settings({radius,1});});
    rejects([]{WorkingYGuidedFilterNode(nullptr,{0,0,1,1});});
    rejects([&]{WorkingYGuidedFilterNode(source,{0,0,0,1});});
    rejects([&]{WorkingYGuidedFilterNode(source,{UINT32_MAX,0,2,1});});
    rejects([]{WorkingYGuidedFilterNode(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})),{0,0,1,1});});
    rejects([&]{working_y_guided_filter_region(roi,source->image,9);});
    rejects([&]{working_y_guided_filter_region({16,31,1,1},source->image,1);});
    Tile huge{{0,0,UINT32_MAX,UINT32_MAX},{},source->output_descriptor()};rejects([&]{working_y_guided_filter_rgb(huge,huge.bounds,huge.bounds,{0,1});});
    const Rect end{UINT32_MAX,UINT32_MAX,1,1};source->image=end;
    for(unsigned radius:{0u,8u})require(rect_same(WorkingYGuidedFilterNode(source,end,{radius,1}).render(end).bounds,end),"last addressable exclusive endpoint");
}
void saved_parameters_and_identity() {
    auto input=std::make_shared<RasterSourceNode>(RasterImage({5,1,0,WorkingSpace::LinearProPhotoD50},{0,0,0,0,0,0,1,1,1,0,0,0,0,0,0}));
    EditSource source;source.id="98100000-0000-0000-0000-000000000001";source.working_space=WorkingSpace::LinearProPhotoD50;source.content_sha256=*input->source_fingerprint();
    EditOperation op;op.id="98100000-0000-0000-0000-000000000090";op.type_id="rawengine.guided_filter_working_y";op.processing_version=2;op.input_domain=op.output_domain=EditDomain::SceneLinearProPhotoD50;op.inputs={{"image",source.id}};op.parameters={{"radius",EditValue{std::int64_t(0)}},{"epsilon",EditValue{1.}}};
    EditManifest manifest;manifest.sources={source};manifest.operations={op};manifest.output_id=op.id;
    auto cache=std::make_shared<TileCache>(1<<20);auto render=[&](const EditManifest& m) {ExecutableEditGraph graph(parse_edit_manifest(serialize_edit_manifest(m)),{{source,input,{0,0,5,1}}},nullptr,cache);return graph.output().render({0,0,5,1});};
    const auto identity=render(manifest);const auto before=cache->stats();require(same(render(manifest).rgb,identity.rgb)&&cache->stats().misses==before.misses,"warm saved identity");
    auto changed=manifest;changed.operations[0].parameters["epsilon"]=EditValue{2.};require(same(render(changed).rgb,identity.rgb)&&cache->stats().misses>before.misses,"epsilon identity retained at radius zero");
    changed=manifest;changed.operations[0].parameters["radius"]=EditValue{1.};require(!same(render(changed).rgb,identity.rgb),"numeric integral saved radius and active filtering");
    for(bool enabled:{false,true}) {
        auto bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].parameters.erase("epsilon");rejects([&]{render(bad);});
        bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].parameters.erase("radius");rejects([&]{render(bad);});
        for(EditValue radius:{EditValue{true},EditValue{-1.},EditValue{1.25},EditValue{9.},EditValue{std::string("1")}}) {bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].parameters["radius"]=radius;rejects([&]{render(bad);});}
        for(EditValue epsilon:{EditValue{false},EditValue{0.},EditValue{65537.},EditValue{std::string("1")}}) {bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].parameters["epsilon"]=epsilon;rejects([&]{render(bad);});}
        bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].processing_version=1;rejects([&]{render(bad);});
        bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].output_domain=EditDomain::SceneLinearRec2020D65;rejects([&]{render(bad);});
        bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].input_domain=bad.operations[0].output_domain=EditDomain::DisplayLinearSrgb;rejects([&]{render(bad);});
        bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].opacity=.5;rejects([&]{render(bad);});
        bad=manifest;bad.operations[0].enabled=enabled;bad.operations[0].parameters.emplace("extra",EditValue{1.});rejects([&]{render(bad);});
    }
    changed=manifest;changed.operations[0].parameters["radius"]=EditValue{std::int64_t(8)};changed.operations[0].enabled=false;require(same(render(changed).rgb,identity.rgb),"validated disabled upstream bypass");
}
void floating_controls() {
    struct Restore {
        int rounding=std::fegetround();
#ifdef GUIDED_TEST_MXCSR
        unsigned csr=_mm_getcsr();
#endif
        ~Restore() {
            std::fesetround(rounding);
#ifdef GUIDED_TEST_MXCSR
            _mm_setcsr(csr);
#endif
        }
    } restore;
    const float tiny=std::numeric_limits<float>::denorm_min();
    const Tile input{{0,0,3,1},{-0.f,tiny,-tiny,.25f,.5f,1.25f,.75f,-.25f,2.f},ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50)};
    for(int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        require(std::fesetround(mode)==0,"set caller rounding for guided control gate");
        for(unsigned flush:{0u,1u}) {
#ifdef GUIDED_TEST_MXCSR
            _mm_setcsr((_mm_getcsr()&~0x8040u)|(flush?0x8040u:0u));
            const auto controls=_mm_getcsr()&~0x3fu;
#else
            (void)flush;
#endif
            auto check=[&] {
                require(std::fegetround()==mode,"guided helper changed caller rounding");
#ifdef GUIDED_TEST_MXCSR
                require((_mm_getcsr()&~0x3fu)==controls,"guided helper changed caller MXCSR controls");
#endif
            };
            for(unsigned radius:{0u,3u,8u})for(double epsilon:{0x1p-24,65536.}) {
                const auto result=working_y_guided_filter_rgb(input,input.bounds,input.bounds,{radius,epsilon});
                if(radius==0)require(same(result.rgb,input.rgb),"radius-zero bits under caller FP controls");
                check();
                auto bad=input;bad.rgb.front()=std::numeric_limits<float>::quiet_NaN();
                rejects([&]{working_y_guided_filter_rgb(bad,bad.bounds,bad.bounds,{radius,epsilon});});check();
            }
        }
    }
}
}
int main() {try {math_and_node();validation();saved_parameters_and_identity();floating_controls();std::cout<<"Guided-filter native tests passed\n";}catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
