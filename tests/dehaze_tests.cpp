#include "ToneOps.hpp"
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("expected dehaze rejection");}
bool bits(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0;}
void controls(){
    const float maximum=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        const std::vector<float> extreme{-0.f,0.f,-0.f,maximum,-maximum,tiny};
        auto input=std::make_shared<RasterSourceNode>(RasterImage({2,1,0,space},extreme));
        require(bits(DehazeNode(input).render({0,0,2,1}).rgb,extreme),"identity extreme bits");
        rejects([&]{DehazeNode(input,{1,{1,1,1}}).render({0,0,2,1});});
        auto source=std::make_shared<RasterSourceNode>(RasterImage({3,1,0,space},{.5f,.5f,.5f,1,1,1,-.5f,2,0}));
        DehazeSettings s{.5,{1,1,1}};DehazeNode node(source,s);s.amount=0;s.atmospheric_light.fill(4);
        auto tile=node.render({0,0,3,1});
        for(unsigned c=0;c<3;++c)require(std::abs(tile.rgb[c]-1.f/9)<1e-7,"independent formation inverse 1/9");
        require(tile.rgb[3]==1 && tile.rgb[4]==1 && tile.rgb[5]==1,"fixed airlight");
        require(tile.rgb[6]<0 && tile.rgb[7]>2,"unclipped signed headroom");
        require(node.input_node()==source.get() && node.output_descriptor()==source->output_descriptor(),"owned descriptor/input");
        require(bits(node.render({0,0,1,1}).rgb,std::vector<float>(tile.rgb.begin(),tile.rgb.begin()+3)),"exact partition");
        auto work=std::async(std::launch::async,[&]{return node.render({0,0,3,1}).rgb;});
        require(bits(work.get(),tile.rgb),"concurrent immutable map");
        Rect r{11,13,2,3};auto footprint=node.input_region_level(r,{0,0,99,99},{});
        require(footprint.x==r.x && footprint.y==r.y && footprint.width==r.width && footprint.height==r.height,"no halo");
        rejects([&]{node.render_level({0,0,1,1},{1,RenderQuality::Final});});
        rejects([&]{node.render_level({0,0,1,1},{3,RenderQuality::Preview});});
        auto zeros=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,space},{-0.f,0.f,-0.f}));
        for(double a:{-1.,-.5,.5,1.}) require(bits(DehazeNode(zeros,{a,{0,0,0}}).render({0,0,1,1}).rgb,{-0.f,0.f,-0.f}),"fixed zero bits");
        auto small=DehazeNode(zeros,{std::ldexp(1.,-40),{1,1,1}}).render({0,0,1,1});
        require(small.rgb[0]<0,"tiny amount no epsilon");
        auto sub=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,space},{tiny,tiny,tiny}));
        require(DehazeNode(sub,{1,{0,0,0}}).render({0,0,1,1}).rgb[0]==8*tiny,"subnormal gain");
        require(DehazeNode(sub,{-1,{0,0,0}}).render({0,0,1,1}).rgb[0]==0,"genuine cast underflow");
    }
    for(double a:{-1.01,1.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) rejects([&]{validate_dehaze_settings({a,{1,1,1}});});
    for(unsigned c=0;c<3;++c)for(double a:{-.01,4.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        DehazeSettings s;s.atmospheric_light[c]=a;rejects([&]{validate_dehaze_settings(s);});
    }
    rejects([]{DehazeNode node(nullptr);});
    rejects([]{DehazeNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})));});
}
void malformed_and_origins(){
    class Stub final:public Node{
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}
        Tile render(Rect r)const override{
            Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3,1),output_descriptor()};
            if(fault==1)t.bounds.x++;
            if(fault==2)t.rgb.pop_back();
            if(fault==3)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
            if(fault>=4)t.rgb[t.rgb.size()-3+(fault-4)%3]=fault<7 ? std::numeric_limits<float>::quiet_NaN() : fault<10 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
            return t;
        }
    };
    auto input=std::make_shared<Stub>();
    for(unsigned fault=1;fault<13;++fault){input->fault=fault;for(double a:{0.,1.,-1.}) rejects([&]{DehazeNode(input,{a,{1,1,1}}).render({0,0,2,1});});}
    input->fault=0;DehazeNode node(input,{1,{1,1,1}});
    for(auto r:{Rect{71,93,2,1},Rect{std::numeric_limits<std::uint32_t>::max(),std::numeric_limits<std::uint32_t>::max(),1,1}})
        require(node.render(r).rgb==std::vector<float>(std::size_t(r.width)*3,1),"translated fixed field");
    rejects([&]{node.render({0,0,0,1});});
    rejects([&]{node.render({std::numeric_limits<std::uint32_t>::max(),0,2,1});});
}
}
int main(){try{controls();malformed_and_origins();std::cout<<"Dehaze native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
