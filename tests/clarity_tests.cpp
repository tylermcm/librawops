#include "SpatialOps.hpp"
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected clarity rejection");}
bool same(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0;}
class Fixture final:public Node {
public:
    Rect image{17,23,5,3};
    WorkingSpace space=WorkingSpace::LinearProPhotoD50;
    mutable Rect requested{};
    unsigned fault=0;
    ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(space);}
    Tile render(Rect r)const override{
        requested=r;Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3),output_descriptor()};
        for(unsigned y=0;y<r.height;++y)for(unsigned x=0;x<r.width;++x){
            float value=r.x+x<image.x+2 ? .25f : .75f;
            for(unsigned c=0;c<3;++c)t.rgb[(std::size_t(y)*r.width+x)*3+c]=value;
        }
        if(fault==1)t.bounds.x++;
        if(fault==2)t.rgb.pop_back();
        if(fault==3)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
        if(fault>=4)t.rgb[(fault-4)%3]=fault<7 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        return t;
    }
};
void native_controls(){
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        auto input=std::make_shared<Fixture>();input->space=space;
        ClaritySettings settings{1,1};ClarityNode node(input,input->image,settings);settings.amount=0;
        require(node.input_node()==input.get() && node.output_descriptor()==input->output_descriptor(),"owned descriptor/settings");
        auto result=node.render(input->image);
        // Independent neutral step mean: x1=.25, neighbors .25/.25/.75.
        require(result.rgb[3]==.125f && result.rgb[6]==.875f,"actual clipped-count step truth");
        require(result.rgb[0]==.25f && result.rgb[12]==.75f,"flat true-border exact");
        const Rect roi{18,24,1,1};auto single=node.render(roi);
        require(single.rgb[0]==result.rgb[(std::size_t(1)*5+1)*3],"nonzero-origin ROI consistency");
        require(input->requested.x==17 && input->requested.y==23 && input->requested.width==3 && input->requested.height==3,"true complete halo");
        auto identity=ClarityNode(input,input->image).render(roi);
        require(identity.rgb[0]==.25f && input->requested.x==18 && input->requested.width==1,"identity skips halo");
        auto data=std::vector<float>(19*3);
        for(unsigned x=0;x<19;++x)for(unsigned c=0;c<3;++c)data[x*3+c]=x<9 ? .25f : .75f;
        auto source=std::make_shared<RasterSourceNode>(RasterImage({19,1,0,space},data));
        ClarityNode r8(source,{0,0,19,1},{1,8});
        auto full=r8.render({0,0,19,1});
        for(unsigned x=0;x<19;++x){auto p=r8.render({x,0,1,1});require(std::memcmp(p.rgb.data(),full.rgb.data()+3*x,12)==0,"radius8 partition bits");}
        auto first=std::async(std::launch::async,[&]{return r8.render({0,0,19,1});});
        auto second=std::async(std::launch::async,[&]{return r8.render({0,0,19,1});});
        require(same(first.get().rgb,second.get().rgb),"independent concurrent requests");
        auto reduced=r8.render_level({0,0,10,1},{1,RenderQuality::Preview});require(reduced.rgb.size()==30,"requested-level dimensions");
        rejects([&]{r8.render_level({0,0,10,1},{1,RenderQuality::Final});});
        rejects([&]{r8.render_level({0,0,1,1},{3,RenderQuality::Preview});});
        rejects([&]{r8.input_region({0,0,1,1},{0,0,18,1});});
        rejects([&]{r8.render({19,0,1,1});});
        const float max=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
        for(auto rgb:{std::vector<float>{-0.f,0.f,-0.f},std::vector<float>{max,max,max},std::vector<float>{-max,-max,-max},std::vector<float>{tiny,tiny,tiny},std::vector<float>{-.125f,.5f,1.25f}}){
            auto constant=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,space},rgb));
            for(auto amount:{-1.,0.,1.,0x1p-40})require(same(ClarityNode(constant,{0,0,1,1},{amount,8}).render({0,0,1,1}).rgb,rgb),"constant/extreme/bypass bits");
        }
    }
}
void validation(){
    for(auto amount:{-1.01,1.01,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        rejects([&]{validate_clarity_settings({amount,3});});
    for(auto radius:{0u,9u,std::numeric_limits<unsigned>::max()})rejects([&]{validate_clarity_settings({0,radius});});
    rejects([]{ClarityNode node(nullptr,{0,0,1,1});});
    auto source=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,WorkingSpace::LinearProPhotoD50},{.5f,.5f,.5f}));
    rejects([&]{ClarityNode node(source,{0,0,0,1});});
    rejects([&]{ClarityNode node(source,{std::numeric_limits<unsigned>::max(),0,2,1});});
    rejects([]{ClarityNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})),{0,0,1,1});});
    auto bad=std::make_shared<Fixture>();
    for(unsigned fault=1;fault<10;++fault){bad->fault=fault;
        for(auto amount:{0.,1.})rejects([&]{ClarityNode(bad,bad->image,{amount,3}).render({18,24,1,1});});
    }
    // Widened support math at the last uint32 coordinate, with a valid 1-pixel extent.
    const Rect end{std::numeric_limits<unsigned>::max(),std::numeric_limits<unsigned>::max(),1,1};
    bad->fault=0;bad->image=end;
    auto halo=ClarityNode(bad,end,{1,8}).input_region(end,end);
    require(halo.x==end.x && halo.y==end.y && halo.width==1 && halo.height==1,"address boundary halo");
}
}
int main(){try{native_controls();validation();std::cout<<"Clarity native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
