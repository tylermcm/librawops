#include "ToneOps.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("expected grading rejection");}
void controls(){
    const float max=std::numeric_limits<float>::max();
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        const std::vector<float> p{-0.f,0.f,-0.f,max,-max,std::numeric_limits<float>::denorm_min()};
        auto input=std::make_shared<RasterSourceNode>(RasterImage({2,1,0,space},p));
        auto result=GradingNode(input).render({0,0,2,1});
        require(std::memcmp(p.data(),result.rgb.data(),p.size()*sizeof(float))==0,"identity bits");
        GradingSettings s;s.gamma[2]=2;
        result=GradingNode(input,s).render({0,0,2,1});
        require(std::memcmp(p.data(),result.rgb.data(),2*sizeof(float))==0 && result.rgb[2]==0 && std::signbit(result.rgb[2]),"component identity and signed power zero");
        require(result.rgb[5]>p[5],"subnormal signed gamma executes");
        s={};s.gain[0]=4;rejects([&]{GradingNode(input,s).render({0,0,2,1});});
        s={};s.gamma[0]=.25;rejects([&]{GradingNode(input,s).render({0,0,2,1});});
        auto zero=std::make_shared<RasterSourceNode>(RasterImage({1,1,0,space},{0,0,0}));
        s={};s.lift={.25,-.25,.25};s.gamma={2,2,.5};GradingNode node(zero,s);s.lift.fill(0);
        require(node.render({0,0,1,1}).rgb==std::vector<float>{.5f,-.5f,.0625f},"owned controls/signed lift power");
        require(node.input_node()==zero.get() && node.output_descriptor()==zero->output_descriptor(),"ownership/descriptor");
        require(node.input_region_level({0,0,1,1},{0,0,1,1},{}).width==1,"no halo");
        rejects([&]{node.render_level({0,0,1,1},{1,RenderQuality::Final});});
    }
    for(unsigned c=0;c<3;++c){
        GradingSettings s;s.lift[c]=1.01;rejects([&]{validate_grading_settings(s);});
        s={};s.gain[c]=-1;rejects([&]{validate_grading_settings(s);});
        s={};s.gamma[c]=0;rejects([&]{validate_grading_settings(s);});
        s={};s.gamma[c]=std::numeric_limits<double>::quiet_NaN();rejects([&]{validate_grading_settings(s);});
    }
    rejects([]{GradingNode node(nullptr);});
    rejects([]{GradingNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})));});
}
void malformed(){
    class Bad final:public Node{
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}
        Tile render(Rect r)const override{
            Tile t{r,{0,0,0},output_descriptor()};
            if(fault==0)t.bounds.x++;
            if(fault==1)t.rgb.pop_back();
            if(fault==2)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
            if(fault>=3)t.rgb[(fault-3)%3]=fault<6 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
            return t;
        }
    };
    auto bad=std::make_shared<Bad>();
    for(unsigned fault=0;fault<9;++fault){bad->fault=fault;rejects([&]{GradingNode(bad).render({0,0,1,1});});}
}
}
int main(){try{controls();malformed();std::cout<<"Grading native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
