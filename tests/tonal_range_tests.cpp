#include "ToneOps.hpp"
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("expected tonal-range rejection");}
bool bits(const std::vector<float>& a,const std::vector<float>& b){return a.size()==b.size()&&!std::memcmp(a.data(),b.data(),a.size()*sizeof(float));}
void controls(){
    const float maximum=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        auto edge=std::make_shared<RasterSourceNode>(RasterImage({3,1,0,space},{-0.f,0.f,-0.f,maximum,-maximum,tiny,tiny,tiny,tiny}));
        require(bits(TonalRangeNode(edge).render({0,0,3,1}).rgb,edge->render({0,0,3,1}).rgb),"identity extreme bits");
        require(bits(TonalRangeNode(edge,{1,1,1,1}).render({0,0,1,1}).rgb,{-0.f,0.f,-0.f}),"active zero-Y signed-zero bypass");
        const std::array<float,4> midpoints{1.f/16,1.f/4,17.f/16,17.f/4},displacements{1.f/32,1.f/8,15.f/32,15.f/8};
        for(unsigned k=0;k<4;++k)for(double amount:{-1.,1.}){
            TonalRangeSettings s;std::array<double*,4> fields{&s.blacks,&s.shadows,&s.highlights,&s.whites};*fields[k]=amount;
            const float x=midpoints[k],expected=x+float(amount)*displacements[k];
            auto source=std::make_shared<RasterSourceNode>(RasterImage({2,1,0,space},{x,x,x,-x,-x,-x}));
            const auto output=TonalRangeNode(source,s).render({0,0,2,1}).rgb;
            require(output==std::vector<float>{expected,expected,expected,-expected,-expected,-expected},"independent rational midpoint and odd extension");
        }
        auto source=std::make_shared<RasterSourceNode>(RasterImage({4,2,0,space},{.9f,.1f,.025f,-.1f,.2f,1.4f,0,.5f,1,2,4,8,.9f,.1f,.025f,-.1f,.2f,1.4f,0,.5f,1,2,4,8}));
        TonalRangeSettings s{.5,-.75,.25,-.5};TonalRangeNode node(source,s);s={};
        const auto result=node.render({0,0,4,2}).rgb;
        require(result!=source->render({0,0,4,2}).rgb,"owned active settings");
        require(bits(node.render({0,0,4,1}).rgb,std::vector<float>(result.begin(),result.begin()+12)),"exact tile partition");
        auto future=std::async(std::launch::async,[&]{return node.render({0,0,4,2}).rgb;});require(bits(future.get(),result),"immutable concurrent map");
        require(std::abs(double(result[0])*.1-double(result[1])*.9)<1e-8,"common RGB ratio");
        require(result[3]<0&&result[11]>1,"unclipped signed and overrange results");
        require(node.input_node()==source.get()&&node.output_descriptor()==source->output_descriptor(),"descriptor and input ownership");
        const auto region=node.input_region_level({7,11,3,2},{0,0,99,99},{});require(region.x==7&&region.y==11&&region.width==3&&region.height==2,"point support");
        rejects([&]{node.render_level({0,0,1,1},{1,RenderQuality::Final});});
        rejects([&]{node.render_level({0,0,1,1},{3,RenderQuality::Preview});});
        auto low=source->render_level({0,0,2,1},{1,RenderQuality::Preview});
        auto reduced=std::make_shared<RasterSourceNode>(RasterImage({2,1,0,space},low.rgb));
        require(bits(node.render_level({0,0,2,1},{1,RenderQuality::Preview}).rgb,TonalRangeNode(reduced,{.5,-.75,.25,-.5}).render({0,0,2,1}).rgb),"reduce before map");
    }
    for(unsigned k=0;k<4;++k)for(double amount:{-1.01,1.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        TonalRangeSettings s;std::array<double*,4> fields{&s.blacks,&s.shadows,&s.highlights,&s.whites};*fields[k]=amount;rejects([&]{validate_tonal_range_settings(s);});
    }
    rejects([]{TonalRangeNode node(nullptr);});
    rejects([]{TonalRangeNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})));});
}
void malformed_and_origins(){
    class Stub final:public Node{
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}
        Tile render(Rect r)const override{
            Tile t{r,std::vector<float>(std::size_t(r.width)*r.height*3,.25f),output_descriptor()};
            if(fault==1)t.bounds.x++;if(fault==2)t.rgb.pop_back();if(fault==3)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
            if(fault>=4)t.rgb.back()=fault==4?std::numeric_limits<float>::quiet_NaN():fault==5?std::numeric_limits<float>::infinity():-std::numeric_limits<float>::infinity();
            return t;
        }
    };
    auto input=std::make_shared<Stub>();
    for(unsigned fault=1;fault<=6;++fault){input->fault=fault;for(auto s:{TonalRangeSettings{},TonalRangeSettings{1,1,1,1}})rejects([&]{TonalRangeNode(input,s).render({0,0,2,1});});}
    input->fault=0;TonalRangeNode node(input,{0,1,0,0});
    for(auto r:{Rect{71,93,2,1},Rect{std::numeric_limits<std::uint32_t>::max(),std::numeric_limits<std::uint32_t>::max(),1,1}})
        require(node.render(r).rgb==std::vector<float>(std::size_t(r.width)*3,.375f),"translated midpoint");
    rejects([&]{node.render({0,0,0,1});});rejects([&]{node.render({0,0,1,0});});
    rejects([&]{node.render({std::numeric_limits<std::uint32_t>::max(),0,2,1});});
}
}
int main(){try{controls();malformed_and_origins();std::cout<<"Tonal range native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
