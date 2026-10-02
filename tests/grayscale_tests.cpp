#include "ToneOps.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f) {
    try {f();}catch(const std::invalid_argument&){return;}
    throw std::runtime_error("expected grayscale rejection");
}
void controls() {
    const float maximum=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        const std::vector<float> p{-0.f,0.f,-0.f,maximum,maximum,maximum,-maximum,-maximum,-maximum,tiny,tiny,tiny};
        auto source=std::make_shared<RasterSourceNode>(RasterImage({4,1,0,space},p));
        auto gray=std::make_shared<GrayscaleNode>(source);auto result=gray->render({0,0,4,1});
        require(std::memcmp(result.rgb.data(),p.data(),p.size()*sizeof(float))==0,"neutral/sign/extreme bits");
        require(gray->input_node()==source.get() && result.descriptor==source->output_descriptor(),"input ownership/descriptor");
        const double wr=space==WorkingSpace::LinearProPhotoD50 ? .28807112822929337 : .26270021201126703;
        const double wb=space==WorkingSpace::LinearProPhotoD50 ? .00008565396060525903 : .059301716469861945;
        auto primaries=std::make_shared<RasterSourceNode>(RasterImage({3,1,0,space},{1,0,0,0,0,1,-maximum,maximum,tiny}));
        auto mapped=std::make_shared<GrayscaleNode>(primaries);result=mapped->render({0,0,3,1});
        require(result.rgb[0]==float(wr) && result.rgb[3]==float(wb),"primary independent luminance");
        for(std::size_t i=0;i<result.rgb.size();i+=3)
            require(std::isfinite(result.rgb[i]) && result.rgb[i]==result.rgb[i+1] && result.rgb[i]==result.rgb[i+2],"equal finite channels");
        require(GrayscaleNode(mapped).render({0,0,3,1}).rgb==result.rgb,"idempotence");
        require(Renderer{}.render_image(*mapped,{0,0,3,1},RenderRequest{{0,0,3,1},1}).rgb==result.rgb,"tile parity");
        auto roi=mapped->input_region_level({0,0,1,1},{0,0,3,1},{1,RenderQuality::Preview});
        require(roi.x==0 && roi.y==0 && roi.width==1 && roi.height==1,"no halo");
        require(mapped->render_level({0,0,1,1},{1,RenderQuality::Preview}).rgb.size()==3,"requested-level mapping");
        rejects([&]{mapped->render_level({0,0,1,1},{1,RenderQuality::Final});});
        rejects([&]{mapped->render_level({0,0,1,1},{3,RenderQuality::Preview});});
    }
    rejects([]{GrayscaleNode node(nullptr);});
    rejects([]{GrayscaleNode node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})));});
}
void malformed() {
    class Bad final:public Node {
    public:
        unsigned fault=0;
        ImageDescriptor output_descriptor()const noexcept override{return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50);}
        Tile render(Rect r)const override {
            Tile t{r,{0,0,0},output_descriptor()};
            if(fault==0)t.bounds.y++;
            if(fault==1)t.rgb.pop_back();
            if(fault==2)t.rgb.push_back(0);
            if(fault==3)t.descriptor=ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65);
            if(fault>=4)t.rgb[(fault-4)%3]=std::numeric_limits<float>::quiet_NaN();
            if(fault>=7)t.rgb[(fault-4)%3]=std::numeric_limits<float>::infinity();
            return t;
        }
    };
    auto source=std::make_shared<Bad>();
    for(unsigned fault=0;fault<10;++fault) {source->fault=fault;rejects([&]{GrayscaleNode(source).render({0,0,1,1});});}
}
}
int main() {
    try {controls();malformed();std::cout<<"Grayscale native tests passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
