#include "SpatialOps.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected texture rejection");}
bool same(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0;}
class Fixture final:public Node {
public:
    Rect image{17,23,5,3};
    WorkingSpace space=WorkingSpace::LinearProPhotoD50;
    mutable Rect requested{};
    unsigned fault=0;
    bool endpoint=false;
    ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(space);}
    Tile render(Rect r)const override{
        requested=r;Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3),output_descriptor()};
        for(unsigned y=0;y<r.height;++y)for(unsigned x=0;x<r.width;++x){
            const float value=endpoint ? 0.f : (std::uint64_t(r.x)+x<std::uint64_t(image.x)+2 ? .25f : .75f);
            for(unsigned c=0;c<3;++c)t.rgb[(std::size_t(y)*r.width+x)*3+c]=value;
        }
        if(fault==1)t.bounds.x++;
        if(fault==2)t.rgb.pop_back();
        if(fault==3)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
        if(fault>=4)t.rgb[(fault-4)%3]=fault<7 ? std::numeric_limits<float>::quiet_NaN() :
            (fault<10 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
        return t;
    }
};
void native_controls(){
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        auto input=std::make_shared<Fixture>();input->space=space;
        TextureSettings settings{1,1};TextureNode node(input,input->image,settings);settings.amount=0;settings.scale=4;
        require(node.input_node()==input.get() && node.output_descriptor()==input->output_descriptor(),"owned descriptor/settings");
        const auto result=node.render(input->image);
        // Independent weighted means for the neutral step (rows identical):
        // fine=[1/4,3/8,5/8,3/4,3/4], coarse=[7/24,13/32,19/32,23/32,3/4].
        require(result.rgb[0]==.21875f && result.rgb[3]==.2265625f &&
                result.rgb[6]==.7734375f && result.rgb[9]==.7734375f && result.rgb[12]==.75f,
                "per-pass true-border step truth");
        const Rect roi{18,24,1,1};const auto single=node.render(roi);
        require(single.rgb[0]==result.rgb[(std::size_t(1)*5+1)*3],"nonzero-origin staged ROI consistency");
        require(input->requested.x==17 && input->requested.y==23 && input->requested.width==4 && input->requested.height==3,"complete 2*scale halo");
        const auto identity=TextureNode(input,input->image).render(roi);
        require(identity.rgb[0]==.25f && input->requested.x==18 && input->requested.width==1,"identity skips halo/planes");
        std::vector<float> data(21*19*3);
        for(unsigned y=0;y<19;++y)for(unsigned x=0;x<21;++x)for(unsigned c=0;c<3;++c)
            data[(std::size_t(y)*21+x)*3+c]=float((x*7+y*3+c*11)%29)/32;
        auto source=std::make_shared<RasterSourceNode>(RasterImage({21,19,0,space},data));
        for(auto scale:{1u,2u,4u})for(auto amount:{-1.,.5,0x1p-40}){
            TextureNode texture(source,{0,0,21,19},{amount,scale});const auto full=texture.render({0,0,21,19});
            for(unsigned y=0;y<19;y+=3)for(unsigned x=0;x<21;x+=3){
                const auto part=texture.render({x,y,std::min(3u,21-x),std::min(3u,19-y)});
                for(unsigned yy=0;yy<part.bounds.height;++yy)
                    require(std::memcmp(part.rgb.data()+std::size_t(yy)*part.bounds.width*3,
                        full.rgb.data()+(std::size_t(y+yy)*21+x)*3,std::size_t(part.bounds.width)*12)==0,"all-scale/sign staged partition bits");
            }
        }
        TextureNode s4(source,{0,0,21,19},{1,4});
        auto first=std::async(std::launch::async,[&]{return s4.render({0,0,21,19});});
        auto second=std::async(std::launch::async,[&]{return s4.render({0,0,21,19});});
        require(same(first.get().rgb,second.get().rgb),"concurrent local planes");
        require(s4.render_level({0,0,11,10},{1,RenderQuality::Preview}).rgb.size()==330,"requested-level dimensions");
        rejects([&]{s4.render_level({0,0,11,10},{1,RenderQuality::Final});});
        rejects([&]{s4.render_level({0,0,1,1},{3,RenderQuality::Preview});});
        rejects([&]{s4.input_region({0,0,1,1},{0,0,20,19});});
        rejects([&]{s4.render({21,0,1,1});});
        require(!node.supports_level({1,RenderQuality::Preview}),"upstream level support retained");
        const float max=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
        for(auto rgb:{std::vector<float>{-0.f,0.f,-0.f},std::vector<float>{max,max,max},
            std::vector<float>{-max,-max,-max},std::vector<float>{tiny,tiny,tiny},
            std::vector<float>{-tiny,-tiny,-tiny},std::vector<float>{-.125f,.5f,1.25f},std::vector<float>{-max,max,0.f}}){
            std::vector<float> repeated;for(unsigned i=0;i<9;++i)repeated.insert(repeated.end(),rgb.begin(),rgb.end());
            auto constant=std::make_shared<RasterSourceNode>(RasterImage({3,3,0,space},repeated));
            for(auto scale:{1u,4u})for(auto amount:{-1.,0.,1.,0x1p-40})
                require(same(TextureNode(constant,{0,0,3,3},{amount,scale}).render({0,0,3,3}).rgb,repeated),"constant/extreme/bypass bits");
        }
        // Texture's fine/coarse planes annihilate interior Nyquist detail.
        std::vector<float> checker(21*3);for(unsigned x=0;x<21;++x)for(unsigned c=0;c<3;++c)checker[x*3+c]=x%2 ? .25f : .75f;
        auto alternating=std::make_shared<RasterSourceNode>(RasterImage({21,1,0,space},checker));
        require(TextureNode(alternating,{0,0,21,1},{1,4}).render({10,0,1,1}).rgb[0]==.75f,"interior Nyquist bypass");
        require(ClarityNode(alternating,{0,0,21,1},{1,1}).render({10,0,1,1}).rgb[0]!=.75f,"texture differs from clarity");
    }
}
void validation(){
    for(auto amount:{-1.01,1.01,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        rejects([&]{validate_texture_settings({amount,2});});
    for(auto scale:{0u,5u,std::numeric_limits<unsigned>::max()})rejects([&]{validate_texture_settings({0,scale});});
    rejects([]{TextureNode node(nullptr,{0,0,1,1});});
    auto source=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,WorkingSpace::LinearProPhotoD50},{.5f,.5f,.5f}));
    rejects([&]{TextureNode node(source,{0,0,0,1});});
    rejects([&]{TextureNode node(source,{std::numeric_limits<unsigned>::max(),0,2,1});});
    rejects([]{TextureNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})),{0,0,1,1});});
    auto bad=std::make_shared<Fixture>();
    for(bool endpoint:{false,true}){bad->endpoint=endpoint;
        for(unsigned fault=1;fault<13;++fault){bad->fault=fault;
            for(auto amount:{0.,1.})rejects([&]{TextureNode(bad,bad->image,{amount,2}).render({18,24,1,1});});
        }
    }
    const Rect end{std::numeric_limits<unsigned>::max(),std::numeric_limits<unsigned>::max(),1,1};
    bad->fault=0;bad->endpoint=false;bad->image=end;
    TextureNode edge(bad,end,{1,4});const auto halo=edge.input_region(end,end);
    require(halo.x==end.x && halo.y==end.y && halo.width==1 && halo.height==1,"uint32 boundary halo");
    require(edge.render(end).rgb[0]==.25f,"uint32 boundary staged render");
}
}
int main(){try{native_controls();validation();std::cout<<"Texture native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
