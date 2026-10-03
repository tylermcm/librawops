#include "SpatialOps.hpp"
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected sharpen rejection");}
bool same(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0;}
class Fixture final:public Node {
public:
    Rect image{17,23,5,3};
    WorkingSpace space=WorkingSpace::LinearProPhotoD50;
    mutable Rect requested{};
    unsigned fault=0;
    bool constant_red=false;
    ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(space);}
    Tile render(Rect r)const override{
        requested=r;Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3),output_descriptor()};
        for(unsigned y=0;y<r.height;++y)for(unsigned x=0;x<r.width;++x){
            float value=std::uint64_t(r.x)+x<std::uint64_t(image.x)+2 ? .25f : .75f;
            for(unsigned c=0;c<3;++c)t.rgb[(std::size_t(y)*r.width+x)*3+c]=constant_red && c==0 ? -0.f : value;
        }
        if(fault==1)t.bounds.x++;
        if(fault==2)t.rgb.pop_back();
        if(fault==3)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
        if(fault>=4)t.rgb[(fault-4)%3]=fault<7 ? std::numeric_limits<float>::quiet_NaN() : fault<10 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
        return t;
    }
};
void native_controls(){
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        auto input=std::make_shared<Fixture>();input->space=space;
        SharpenSettings settings{1,1};SharpenNode node(input,input->image,settings);settings.amount=0;
        require(node.input_node()==input.get() && node.output_descriptor()==input->output_descriptor(),"owned descriptor/settings");
        auto result=node.render(input->image);
        // Independent neutral step mean: x1=.25, neighbors .25/.25/.75.
        require(std::abs(result.rgb[3]-1.f/12)<1e-7 && std::abs(result.rgb[6]-11.f/12)<1e-7,"actual clipped-count step truth");
        require(result.rgb[0]==.25f && result.rgb[12]==.75f,"flat true-border exact");
        input->constant_red=true;auto partial=node.render(input->image);
        for(std::size_t i=0;i<partial.rgb.size();i+=3)require(partial.rgb[i]==0 && std::signbit(partial.rgb[i]),"constant channel bits while other channels edit");
        require(partial.rgb[4]<.25f,"independent channel edit");input->constant_red=false;
        const Rect roi{18,24,1,1};auto single=node.render(roi);
        require(single.rgb[0]==result.rgb[(std::size_t(1)*5+1)*3],"nonzero-origin ROI consistency");
        require(input->requested.x==17 && input->requested.y==23 && input->requested.width==3 && input->requested.height==3,"true complete halo");
        auto identity=SharpenNode(input,input->image).render(roi);
        require(identity.rgb[0]==.25f && input->requested.x==18 && input->requested.width==1,"identity skips halo");
        auto data=std::vector<float>(19*3);
        for(unsigned x=0;x<19;++x)for(unsigned c=0;c<3;++c)data[x*3+c]=x<9 ? .25f : .75f;
        auto source=std::make_shared<RasterSourceNode>(RasterImage({19,1,0,space},data));
        SharpenNode r3(source,{0,0,19,1},{1,3});
        auto full=r3.render({0,0,19,1});
        for(unsigned x=0;x<19;++x){auto p=r3.render({x,0,1,1});require(std::memcmp(p.rgb.data(),full.rgb.data()+3*x,12)==0,"radius3 partition bits");}
        auto first=std::async(std::launch::async,[&]{return r3.render({0,0,19,1});});
        auto second=std::async(std::launch::async,[&]{return r3.render({0,0,19,1});});
        require(same(first.get().rgb,second.get().rgb),"independent concurrent requests");
        auto reduced=r3.render_level({0,0,10,1},{1,RenderQuality::Preview});require(reduced.rgb.size()==30,"requested-level dimensions");
        rejects([&]{r3.render_level({0,0,10,1},{1,RenderQuality::Final});});
        rejects([&]{r3.render_level({0,0,1,1},{3,RenderQuality::Preview});});
        rejects([&]{r3.input_region({0,0,1,1},{0,0,18,1});});
        rejects([&]{r3.render({19,0,1,1});});
        const float max=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
        for(auto rgb:{std::vector<float>{-0.f,0.f,-0.f},std::vector<float>{max,max,max},std::vector<float>{-max,-max,-max},std::vector<float>{tiny,tiny,tiny},std::vector<float>{-.125f,.5f,1.25f}}){
            auto constant=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,space},rgb));
            for(auto amount:{0.,1.,2.,0x1p-40})require(same(SharpenNode(constant,{0,0,1,1},{amount,3}).render({0,0,1,1}).rgb,rgb),"constant/extreme/bypass bits");
        }
    }
}
void validation(){
    const float maximum=std::numeric_limits<float>::max();
    auto extreme=std::make_shared<RasterSourceNode>(RasterImage({2,1,0,WorkingSpace::LinearProPhotoD50},{maximum,maximum,maximum,-maximum,-maximum,-maximum}));
    rejects([&]{SharpenNode(extreme,{0,0,2,1},{2,1}).render({0,0,2,1});});
    for(auto amount:{-.01,2.01,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        rejects([&]{validate_sharpen_settings({amount,3});});
    for(auto radius:{0u,4u,std::numeric_limits<unsigned>::max()})rejects([&]{validate_sharpen_settings({0,radius});});
    rejects([]{SharpenNode node(nullptr,{0,0,1,1});});
    auto source=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,WorkingSpace::LinearProPhotoD50},{.5f,.5f,.5f}));
    rejects([&]{SharpenNode node(source,{0,0,0,1});});
    rejects([&]{SharpenNode node(source,{std::numeric_limits<unsigned>::max(),0,2,1});});
    rejects([]{SharpenNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})),{0,0,1,1});});
    auto bad=std::make_shared<Fixture>();
    for(unsigned fault=1;fault<13;++fault){bad->fault=fault;
        for(auto amount:{0.,1.})rejects([&]{SharpenNode(bad,bad->image,{amount,3}).render({18,24,1,1});});
    }
    // Widened support math at the last uint32 coordinate, with a valid 1-pixel extent.
    const Rect end{std::numeric_limits<unsigned>::max(),std::numeric_limits<unsigned>::max(),1,1};
    bad->fault=0;bad->image=end;
    auto halo=SharpenNode(bad,end,{1,3}).input_region(end,end);
    require(SharpenNode(bad,end,{1,3}).render(end).rgb.size()==3,"final coordinate render");
    require(halo.x==end.x && halo.y==end.y && halo.width==1 && halo.height==1,"address boundary halo");
}
}
int main(){try{native_controls();validation();std::cout<<"Sharpen native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
