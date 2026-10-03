#include "CubeLut.hpp"
#include <cstring>
#include <iostream>
#include <limits>
using namespace rawengine;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}catch(const std::length_error&){return;}throw std::runtime_error("expected Cube/LUT rejection");}
void controls(){
    const float max=std::numeric_limits<float>::max();
    for(auto space:{WorkingSpace::LinearProPhotoD50,WorkingSpace::LinearRec2020D65}){
        const std::vector<float> p{-0.f,0.f,-0.f,max,-max,std::numeric_limits<float>::denorm_min()};
        auto input=std::make_shared<RasterSourceNode>(RasterImage({2,1,0,space},p));
        Lut1DSettings one;
        for(auto& c:one.channels){c.clear();for(unsigned i=0;i<4096;++i)c.push_back(double(i)/4095);}
        validate_large_lut1d_settings(one);rejects([&]{validate_lut1d_settings(one);});
        LargeLut1DNode one_node(input,one);one.channels[0][0]=.5;
        auto out=one_node.render({0,0,2,1});require(std::memcmp(p.data(),out.rgb.data(),p.size()*sizeof(float))==0,"large1D identity/copied settings");
        Lut3DSettings cube;cube.size=33;cube.values.clear();
        for(unsigned b=0;b<33;++b)for(unsigned g=0;g<33;++g)for(unsigned r=0;r<33;++r){cube.values.push_back(double(r)/32);cube.values.push_back(double(g)/32);cube.values.push_back(double(b)/32);}
        validate_large_lut3d_settings(cube);rejects([&]{validate_lut3d_settings(cube);});
        LargeLut3DNode cube_node(input,cube);cube.values[0]=.5;
        out=cube_node.render({0,0,2,1});require(std::memcmp(p.data(),out.rgb.data(),p.size()*sizeof(float))==0,"large3D identity/copied settings");
        require(cube_node.input_node()==input.get() && cube_node.output_descriptor()==input->output_descriptor(),"ownership/descriptor");
        auto lut=parse_cube_lut("# original fixture\r\nTITLE \"identity # test\"\r\nLUT_1D_SIZE 2\r\n0 0 0\r\n1 1 1\r\n",space);
        require(lut.title=="identity # test","quoted title");
        require(make_cube_lut_node(input,lut)->render({0,0,2,1}).rgb==p,"parse/node identity");
        auto op=cube_lut_operation(lut,"93000000-0000-0000-0000-000000000090","93000000-0000-0000-0000-000000000001");
        require(op.type_id=="rawengine.lut1d" && op.processing_version==2 && op.parameters.size()==3,"saved operation fields");
        rejects([&]{make_cube_lut_node(std::make_shared<RawUnpackNode>(RawImage(1,1,{1})),lut);});
        rejects([&]{make_cube_lut_node(nullptr,lut);});
    }
    for(const auto* text:{"","LUT_1D_SIZE 1\n","LUT_3D_SIZE 34\n","LUT_1D_SIZE 4097\n", "LUT_1D_SIZE 2\n0 0 0\n", "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n2 2 2\n", "LUT_1D_SIZE 2\n0 0 0\nNaN 1 1\n", "LUT_1D_SIZE 2\n0 0 0\nTITLE \"late\"\n1 1 1\n"})
        rejects([&]{parse_cube_lut(text,WorkingSpace::LinearProPhotoD50);});
    rejects([]{parse_cube_lut(std::string(4*1024*1024+1,' '),WorkingSpace::LinearProPhotoD50);});
    rejects([]{parse_cube_lut("LUT_1D_SIZE 2\n0 0 0\n1 1 1",static_cast<WorkingSpace>(999));});
    Lut1DSettings one;one.channels[0].resize(4097);rejects([&]{validate_large_lut1d_settings(one);});
    Lut3DSettings cube;cube.size=34;rejects([&]{validate_large_lut3d_settings(cube);});
}
}
int main(){try{controls();std::cout<<"Extended LUT/Cube native tests passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
