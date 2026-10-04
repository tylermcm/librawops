#include "ToneOps.hpp"
#include <algorithm>
#include <cfenv>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("expected curves/levels rejection");}
bool bits(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size()&&!std::memcmp(a.data(),b.data(),a.size()*sizeof(float));}
auto source(std::vector<float> data,unsigned w,unsigned h=1,WorkingSpace s=WorkingSpace::LinearProPhotoD50){return std::make_shared<RasterSourceNode>(RasterImage({w,h,0,s},std::move(data)));}
void truth(){
    auto input=source({.25f,.25f,.25f,.5029296875f,.5029296875f,.5029296875f},2);
    ExtendedCurvesSettings s;s.interpolation=CurveInterpolation::ShapePreservingCubic;s.master.knots={{0,0},{.5,.25},{1,1}};
    for(auto& c:s.channels)c.knots={{0,0},{.5,.75},{1,1}};
    ExtendedCurvesNode node(input,s);s.master.knots={{0,0},{1,0}};
    auto tile=node.render({0,0,2,1});require(tile.rgb[0]==.2109375f && tile.rgb[3]==.4401211142539978f,"independent composition/cast truth");
    std::vector<float> squares{-4,-4,-4,-.25f,-.25f,-.25f,-0.f,-0.f,-0.f,0,0,0,.25f,.25f,.25f,4,4,4};
    GammaLevelsSettings g;g.gamma={2,2,2};GammaLevelsNode gamma(source(squares,6),g);g.gamma={.25,.25,.25};
    const auto result=gamma.render({0,0,6,1});const std::vector<float> expected{-2,-2,-2,-.5f,-.5f,-.5f,0,0,0,0,0,0,.5f,.5f,.5f,2,2,2};require(bits(result.rgb,expected),"signed gamma rational square truth");
    auto huge=source({std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-0.f},1);
    s={};s.master.knots={{0,0},{1,65536}};for(auto& c:s.channels)c.knots={{0,0},{1,1.0/65536}};
    require(bits(ExtendedCurvesNode(huge,s).render({0,0,1,1}).rgb,{std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),0.f}),"master intermediate float32 rejection/cast");
    s.channels={};rejects([&]{ExtendedCurvesNode(huge,s).render({0,0,1,1});});
    GammaLevelsSettings strong;strong.gamma={.25,.25,.25};rejects([&]{GammaLevelsNode(huge,strong).render({0,0,1,1});});
    ExtendedCurvesSettings flat;flat.interpolation=CurveInterpolation::ShapePreservingCubic;flat.master.knots={{0,0},{1,1},{2,1},{3,2}};
    require(ExtendedCurvesNode(source({1.25f,1.5f,1.75f},1),flat).render({0,0,1,1}).rgb==std::vector<float>({1,1,1}),"flat segment overshoot");
}
void levels_and_ownership(){
    std::vector<float> data;for(unsigned i=0;i<11*9*3;++i)data.push_back(float(int(i*31%97)-29)/32);
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        auto input=source(data,11,9,space);ExtendedCurvesSettings s;s.interpolation=CurveInterpolation::ShapePreservingCubic;s.master.knots={{0,0},{.5,.25},{1,1}};
        ExtendedCurvesNode curves(input,s);GammaLevelsSettings g;g.gamma={2,.7,1.3};GammaLevelsNode gamma(input,g);
        for(const Node* node:{static_cast<const Node*>(&curves),static_cast<const Node*>(&gamma)}){
            require(node->output_descriptor()==input->output_descriptor(),"descriptor changed");
            for(unsigned mip=0;mip<=2;++mip){const RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};const unsigned scale=1u<<mip;Rect r{0,0,(11+scale-1)/scale,(9+scale-1)/scale};
                const auto whole=node->render_level(r,level);RenderRequest request{r,3};request.level=level;
                require(bits(Renderer{}.render_image(*node,{0,0,11,9},request).rgb,whole.rgb),"tile partition mismatch");
                Rect roi{1,0,1,1};require(bits(node->render_level(roi,level).rgb,{whole.rgb[3],whole.rgb[4],whole.rgb[5]}),"ROI mismatch");
                const auto footprint=node->input_region_level(roi,r,level);require(footprint.x==1&&footprint.width==1&&footprint.height==1,"point halo");
                auto reduced=source(input->render_level(r,level).rgb,r.width,r.height,space);
                auto independent=mip? (node==&curves?std::shared_ptr<const Node>(std::make_shared<ExtendedCurvesNode>(reduced,s)):std::shared_ptr<const Node>(std::make_shared<GammaLevelsNode>(reduced,g))) : std::shared_ptr<const Node>{};
                if(mip)require(bits(independent->render({0,0,r.width,r.height}).rgb,whole.rgb),"mapping before requested reduction");
            }
            require(!node->supports_level({1,RenderQuality::Final})&&!node->supports_level({3,RenderQuality::Preview}),"unsupported level admitted");
            rejects([&]{node->render({0,0,0,1});});rejects([&]{node->render({UINT32_MAX,0,2,1});});
            auto a=std::async(std::launch::async,[&]{return node->render({0,0,11,9}).rgb;});auto b=std::async(std::launch::async,[&]{return node->render({0,0,11,9}).rgb;});require(bits(a.get(),b.get()),"concurrent mutable map");
        }
    }
}
void affine_and_identity(){
    const int modes[]={FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    for(int mode:modes){std::fesetround(mode);
        auto input=source({-0.f,0.f,-2.f,.25f,.5f,1.f,std::numeric_limits<float>::denorm_min(),1.5f,2.f},3);
        require(bits(ExtendedCurvesNode(input).render({0,0,3,1}).rgb,input->render({0,0,3,1}).rgb),"curve identity bits");
        require(bits(GammaLevelsNode(input).render({0,0,3,1}).rgb,input->render({0,0,3,1}).rgb),"gamma identity bits");
        for(ChannelLevels p:{ChannelLevels{.25,.75,-1,2},ChannelLevels{0,1,-0.,0.},ChannelLevels{0,1,0.,-0.}}){LevelsSettings old;GammaLevelsSettings g;for(auto& c:old.channels)c=p;g.channels=old.channels;
            require(bits(LevelsNode(input,old).render({0,0,3,1}).rgb,GammaLevelsNode(input,g).render({0,0,3,1}).rgb),"gamma1 changed legacy affine signs/order");}
    }
    std::fesetround(FE_TONEAREST);
}
void rejection(){
    auto input=source({0,1,2},1);ExtendedCurvesSettings s;
    for(auto k:{std::vector<CurvePoint>{{0,0}},{{0,0},{0,1}},{{0,0},{1e-8,1}},std::vector<CurvePoint>(257)}){s={};s.master.knots=k;rejects([&]{ExtendedCurvesNode n(input,s);});}
    s={};s.interpolation=static_cast<CurveInterpolation>(99);rejects([&]{validate_extended_curves_settings(s);});
    for(double g:{0.,.249,4.001,std::numeric_limits<double>::quiet_NaN()}){GammaLevelsSettings bad;bad.gamma[2]=g;rejects([&]{GammaLevelsNode n(input,bad);});}
    GammaLevelsSettings small;small.channels[0]={0,1e-8,2,2};rejects([&]{GammaLevelsNode n(input,small);});
    rejects([&]{ExtendedCurvesNode n(nullptr);});rejects([&]{GammaLevelsNode n(nullptr);});
    class Bad final:public Node{public:unsigned fault=0;ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}Tile render(Rect r)const override{Tile t{r,{0,1,2},output_descriptor()};if(fault==0)t.rgb[2]=std::numeric_limits<float>::quiet_NaN();if(fault==1)t.rgb.pop_back();if(fault==2)++t.bounds.x;if(fault==3)t.descriptor=ImageDescriptor::camera_linear();return t;}};
    auto bad=std::make_shared<Bad>();for(unsigned fault=0;fault<4;++fault){bad->fault=fault;rejects([&]{ExtendedCurvesNode(bad).render({0,0,1,1});});rejects([&]{GammaLevelsNode(bad).render({0,0,1,1});});}
}
}
int main(){try{truth();levels_and_ownership();affine_and_identity();rejection();std::cout<<"Curves/levels independent truth, composition, affine, ROI/mip/tile/concurrency/validation pass\n";}catch(const std::exception& e){std::fesetround(FE_TONEAREST);std::cerr<<e.what()<<'\n';return 1;}}
