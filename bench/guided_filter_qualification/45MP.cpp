#include "SpatialOps.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
using namespace rawengine;using Clock=std::chrono::steady_clock;
double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
void require(bool v,const char* s){if(!v)throw std::runtime_error(s);}
bool same(Rect a,Rect b){return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;}
class Digest{BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
public:Digest(){require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"SHA provider");require(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)==0,"SHA hash");}
~Digest(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
void update(const void* p,std::size_t n){require(n<=ULONG_MAX&&BCryptHashData(hash,(PUCHAR)p,ULONG(n),0)==0,"SHA update");}
std::string finish(){std::array<unsigned char,32> bytes{};require(BCryptFinishHash(hash,bytes.data(),ULONG(bytes.size()),0)==0,"SHA finish");std::ostringstream text;text<<std::hex<<std::setfill('0');for(auto b:bytes)text<<std::setw(2)<<unsigned(b);return text.str();}};
std::vector<Rect> partitions(Rect image){std::vector<Rect> r;for(unsigned y=0;y<image.height;y+=256)for(unsigned x=0;x<image.width;x+=256)r.push_back({x,y,std::min(256u,image.width-x),std::min(256u,image.height-y)});return r;}
Rect expand(Rect r,Rect image,unsigned halo){auto left=r.x>halo?r.x-halo:0u,top=r.y>halo?r.y-halo:0u;auto right=std::min<std::uint64_t>(image.width,std::uint64_t(r.x)+r.width+halo),bottom=std::min<std::uint64_t>(image.height,std::uint64_t(r.y)+r.height+halo);return {left,top,unsigned(right-left),unsigned(bottom-top)};}
PROCESS_MEMORY_COUNTERS_EX memory(){PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m))!=0,"memory counter");return m;}
int main(int argc,char** argv){try{
 require(argc==5&&!std::filesystem::exists(argv[4]),"space/preset/mode/fresh report required");unsigned space_number=unsigned(std::stoul(argv[1]));require(space_number<=1,"space");auto space=space_number==0?WorkingSpace::LinearProPhotoD50:WorkingSpace::LinearRec2020D65;std::string preset=argv[2],mode=argv[3];require(mode=="stream"||mode=="image","mode");require(preset=="source"||preset=="r0"||preset=="r3"||preset=="r8"||preset=="r8-mip2","preset");
 MEMORYSTATUSEX available{};available.dwLength=sizeof(available);require(GlobalMemoryStatusEx(&available)!=0&&available.ullAvailPhys>=2ull*1024*1024*1024,"45MP study requires2GiB available RAM");
 constexpr unsigned w=7200,h=6250;auto before=memory();auto construction_start=Clock::now();std::vector<float> samples(std::size_t(w)*h*3);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<3;++c)samples[(std::size_t(y)*w+x)*3+c]=float(int((x*19+y*37+c*83)%257)-64)/128;
 auto source=std::make_shared<RasterSourceNode>(RasterImage({w,h,0,space},std::move(samples)));double construction_ms=ms(Clock::now()-construction_start);auto owned=memory();
 unsigned radius=preset=="r3"?3u:(preset=="r8"||preset=="r8-mip2")?8u:0u,mip=preset=="r8-mip2"?2u:0u;WorkingYGuidedFilterSettings settings{radius,radius==3?0x1p-12:0x1p-24};
 std::shared_ptr<const Node> node=preset=="source"?std::shared_ptr<const Node>(source):std::make_shared<WorkingYGuidedFilterNode>(source,Rect{0,0,w,h},settings);
 auto scale=1u<<mip;Rect image{0,0,(w+scale-1)/scale,(h+scale-1)/scale};RenderLevel level{mip,mip?RenderQuality::Preview:RenderQuality::Final};auto rects=partitions(image);
 std::array<std::size_t,4> selections{0,std::size_t((image.width+255)/256-1),rects.size()-std::size_t((image.width+255)/256),rects.size()-1};std::array<Tile,4> reference;for(unsigned k=0;k<4;++k)reference[k]=node->render_level(rects[selections[k]],level);
 std::uint64_t source_bytes=0,max_payload=0,coefficients=0;for(auto r:rects){auto needed=expand(r,image,2*radius);auto centers=expand(r,image,radius);auto n=std::uint64_t(needed.width)*needed.height,o=std::uint64_t(r.width)*r.height,a=radius?std::uint64_t(centers.width)*centers.height:0;source_bytes+=12*n;coefficients+=a;max_payload=std::max(max_payload,12*n+56*a+(preset=="source"?0:12*o));}
 Renderer renderer;Digest digest;std::uint64_t delivered=0,selected_exact=0;std::size_t index=0;double callback_ms=0;
 auto inspect=[&](const Tile& tile,const float* data,std::size_t stride){auto start=Clock::now();require(index<rects.size()&&same(tile.bounds,rects[index])&&tile.descriptor==ImageDescriptor::scene_linear(space),"tile bounds/descriptor");std::array<unsigned,4> bounds{tile.bounds.x,tile.bounds.y,tile.bounds.width,tile.bounds.height};digest.update(bounds.data(),sizeof(bounds));
  for(unsigned y=0;y<tile.bounds.height;++y){auto row=data+std::size_t(y)*stride;digest.update(row,std::size_t(tile.bounds.width)*12);for(unsigned k=0;k<4;++k)if(index==selections[k])require(std::memcmp(row,reference[k].rgb.data()+std::size_t(y)*tile.bounds.width*3,std::size_t(tile.bounds.width)*12)==0,"selected corner differs");}
  for(auto k:selections)if(index==k)++selected_exact;++index;delivered+=std::uint64_t(tile.bounds.width)*tile.bounds.height;callback_ms+=ms(Clock::now()-start);};
 auto render_start=Clock::now();Tile materialized;double total_ms;
 if(mode=="stream")renderer.render_tiles(*node,{0,0,w,h},RenderRequest{image,256,level},[&](const Tile& tile){inspect(tile,tile.rgb.data(),std::size_t(tile.bounds.width)*3);});
 else materialized=renderer.render_image(*node,{0,0,w,h},RenderRequest{image,256,level});
 total_ms=ms(Clock::now()-render_start);auto rendered=memory();
 if(mode=="image")for(auto r:rects){Tile header{r,{},materialized.descriptor};inspect(header,materialized.rgb.data()+(std::size_t(r.y)*image.width+r.x)*3,std::size_t(image.width)*3);}
 require(index==rects.size()&&delivered==std::uint64_t(image.width)*image.height&&selected_exact==4,"coverage/corners");auto output_digest=digest.finish();auto finished=memory();wchar_t dll[32768]{};require(GetModuleFileNameW(GetModuleHandleW(L"RawEngine.dll"),dll,32768)>0,"loaded DLL");
 std::ofstream report(argv[4]);report<<std::setprecision(17)<<"{\"complete\":true,\"space\":"<<space_number<<",\"preset\":\""<<preset<<"\",\"mode\":\""<<mode<<"\",\"radius\":"<<radius<<",\"epsilon\":"<<settings.epsilon<<",\"mip\":"<<mip<<",\"source_shape\":[7200,6250],\"output_pixels\":"<<delivered<<",\"tiles\":"<<rects.size()<<",\"selected_corner_tiles_exact\":"<<selected_exact<<",\"tile_order_output_sha256\":\""<<output_digest<<"\",\"source_owned_payload_bytes\":540000000,\"derived_source_returned_bytes\":"<<source_bytes<<",\"derived_coefficient_centers\":"<<coefficients<<",\"max_node_logical_bytes\":"<<max_payload<<",\"renderer_retained_bytes\":"<<(mode=="image"?delivered*12:0)<<",\"source_construction_ms\":"<<construction_ms<<",\"render_total_ms\":"<<total_ms<<",\"checksum_validation_ms\":"<<callback_ms<<",\"stream_render_excluding_callback_estimate_ms\":"<<(mode=="stream"?total_ms-callback_ms:total_ms)<<",\"before_private_bytes\":"<<before.PrivateUsage<<",\"source_owned_private_bytes\":"<<owned.PrivateUsage<<",\"returned_private_bytes\":"<<rendered.PrivateUsage<<",\"peak_working_set_bytes\":"<<finished.PeakWorkingSetSize<<",\"peak_commit_bytes\":"<<finished.PeakPagefileUsage<<",\"total_physical_memory_bytes\":"<<available.ullTotalPhys<<",\"available_physical_memory_at_start_bytes\":"<<available.ullAvailPhys<<",\"loaded_dll\":\""<<std::filesystem::path(dll).generic_string()<<"\"}\n";
 std::cout<<space_number<<' '<<preset<<' '<<mode<<' '<<total_ms<<"ms peak"<<finished.PeakWorkingSetSize<<" bytes\n"<<std::flush;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
