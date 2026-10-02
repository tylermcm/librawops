#include "ToneOps.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace rawengine;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected channel mixer rejection");
}
std::shared_ptr<const Node> source(const std::vector<float>& data,WorkingSpace space,unsigned w,unsigned h=1) {
    return std::make_shared<RasterSourceNode>(RasterImage({w,h,0,space},data));
}
void truth_and_bits() {
    const std::vector<float> data{1,0,0,0,1,0,0,0,1,-1,.5f,2,3,-2,1};
    const std::array<double,9> mix{1.25,-.25,.125,.25,.5,.25,-.5,.25,1.25};
    for (auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}) {
        auto input=source(data,space,5);
        ChannelMixerSettings settings{mix}; ChannelMixerNode node(input,settings); settings.matrix.fill(0);
        const auto output=node.render({0,0,5,1});
        require(output.descriptor==input->output_descriptor(),"mixer descriptor differs");
        for (unsigned p=0;p<5;++p) for (unsigned row=0;row<3;++row) {
            // Dyadic coefficients/samples make this weighted sum exact in binary64.
            double expected=0; for (unsigned c=0;c<3;++c) expected+=mix[row*3+c]*data[p*3+c];
            require(output.rgb[p*3+row]==float(expected),"independent dot truth differs");
        }
        require(Renderer{}.render_image(node,{0,0,5,1},RenderRequest{{0,0,5,1},2}).rgb==output.rgb,"partition differs");
        const float max=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
        const std::vector<float> extreme{-0.f,0.f,-0.f,max,-max,tiny,-tiny,tiny,-0.f};
        auto special=source(extreme,space,3);
        auto identity=ChannelMixerNode(special).render({0,0,3,1});
        require(std::memcmp(identity.rgb.data(),extreme.data(),extreme.size()*sizeof(float))==0,"identity bits differ");
        ChannelMixerSettings permutation{{0,1,0,0,0,1,1,0,0}};
        auto rotated=ChannelMixerNode(special,permutation).render({0,0,3,1});
        for (unsigned p=0;p<3;++p) for (unsigned c=0;c<3;++c)
            require(std::memcmp(&rotated.rgb[p*3+c],&extreme[p*3+(c+1)%3],sizeof(float))==0,"unit row bits differ");
        ChannelMixerSettings partial{{.5,.25,.25,-0.,1,0,1,0,0}};
        auto partial_result=ChannelMixerNode(special,partial).render({0,0,3,1});
        for (unsigned p=0;p<3;++p) {
            require(std::memcmp(&partial_result.rgb[p*3+1],&extreme[p*3+1],4)==0,"unchanged row bits differ");
            require(std::memcmp(&partial_result.rgb[p*3+2],&extreme[p*3],4)==0,"selected row bits differ");
        }
        ChannelMixerSettings boundary{{64,-64,0,0,0,0,0,0,-64}};
        const auto edge=ChannelMixerNode(source({.125f,.25f,-.5f},space,1),boundary).render({0,0,1,1});
        require(edge.rgb==std::vector<float>({-8,0,32}),"coefficient boundary or implicit normalization differs");
    }
}
void levels_and_errors() {
    std::vector<float> data;
    for (unsigned i=0;i<15;++i) data.insert(data.end(),{float(i)/8-.5f,float(i%4)/4,float(i%7)/3});
    auto input=source(data,WorkingSpace::LinearRec2020D65,5,3);
    ChannelMixerSettings settings{{1.25,-.25,.125,.25,.5,.25,-.5,.25,1.25}};
    ChannelMixerNode node(input,settings);
    for (unsigned mip:{1u,2u}) {
        RenderLevel level{mip,RenderQuality::Preview}; Rect roi{0,0,mip==1?3u:2u,mip==1?2u:1u};
        auto reduced=input->render_level(roi,level);
        auto expected=ChannelMixerNode(source(reduced.rgb,WorkingSpace::LinearRec2020D65,roi.width,roi.height),settings).render(roi);
        require(node.render_level(roi,level).rgb==expected.rgb,"mixer does not map requested-level samples");
        require(node.input_region_level(roi,roi,level).width==roi.width,"mixer adds halo");
    }
    for (double invalid:{-64.1,64.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        for (unsigned index=0;index<9;++index) {
            auto bad=settings;bad.matrix[index]=invalid;
            rejects([&] { ChannelMixerNode rejected(input,bad); });
        }
    rejects([&] { ChannelMixerNode rejected(nullptr); });
    auto camera=std::make_shared<RawUnpackNode>(RawImage(1,1,{32768}));
    rejects([&] { ChannelMixerNode rejected(camera); });
    rejects([&] { node.render_level({0,0,1,1},{3,RenderQuality::Preview}); });
    rejects([&] { node.render_level({0,0,1,1},{1,RenderQuality::Final}); });
    auto overflow_settings=ChannelMixerSettings{};overflow_settings.matrix[0]=2;
    ChannelMixerNode overflow(source({std::numeric_limits<float>::max(),0,0},WorkingSpace::LinearProPhotoD50,1),overflow_settings);
    rejects([&] { overflow.render({0,0,1,1}); });
    class Bad final:public Node {
    public:
        unsigned fault=0,channel=0;
        ImageDescriptor output_descriptor() const noexcept override { return ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50); }
        Tile render(Rect r) const override {
            Tile tile{r,{1,2,3},output_descriptor()};
            if (fault==0) tile.rgb.pop_back();
            if (fault==1) ++tile.bounds.x;
            if (fault==2) tile.descriptor=ImageDescriptor::camera_linear();
            if (fault==3) tile.rgb[channel]=std::numeric_limits<float>::quiet_NaN();
            if (fault==4) tile.rgb[channel]=std::numeric_limits<float>::infinity();
            if (fault==5) tile.rgb[channel]=-std::numeric_limits<float>::infinity();
            return tile;
        }
    };
    auto bad=std::make_shared<Bad>();ChannelMixerNode checked(bad);
    for (unsigned i=0;i<3;++i) {bad->fault=i;rejects([&] {checked.render({0,0,1,1});});}
    for (auto matrix:{ChannelMixerSettings{},ChannelMixerSettings{{0,0,0,0,0,0,0,0,0}},ChannelMixerSettings{{0,1,0,0,1,0,0,1,0}}})
        for (unsigned c=0;c<3;++c) for (unsigned fault=3;fault<6;++fault) {
            bad->fault=fault;bad->channel=c;ChannelMixerNode invalid(bad,matrix);
            rejects([&] { invalid.render({0,0,1,1}); });
        }
}
}
int main() {
    try { truth_and_bits();levels_and_errors();std::cout<<"Channel mixer controls pass\n";return 0; }
    catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
