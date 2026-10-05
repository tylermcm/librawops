#include "RgbaGraph.hpp"
#include "reference/rgba_native_v1.hpp"
#include "reference/alpha_numeric_v1.hpp"

#include <bit>
#include <cfenv>
#include <future>
#include <iostream>
#include <limits>

using namespace rawengine;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool same(Rect a,Rect b) { return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height; }
bool native_final(RenderLevel l) { return l.mip==0 && l.quality==RenderQuality::Final; }
float value(std::uint32_t bits) { return std::bit_cast<float>(bits); }
std::vector<float> values(const std::vector<std::uint32_t>& words) {
    std::vector<float> out;for (auto bits:words) out.push_back(value(bits));return out;
}
template<class Exception=std::invalid_argument,class Function> void rejects(Function f) {
    bool caught=false;try { f(); } catch (const Exception&) { caught=true; }
    require(caught,"expected rejection");
}
struct RgbSpy final : Node {
    Rect bounds;std::vector<float> data;WorkingSpace space;mutable int calls=0;
    mutable Rect last{};mutable RenderLevel last_level{};int bad=0;
    RgbSpy(Rect b,std::vector<float> v,WorkingSpace s):bounds(b),data(std::move(v)),space(s) {}
    Tile render(Rect roi) const override { return render_level(roi,{}); }
    ImageDescriptor output_descriptor() const noexcept override {return ImageDescriptor::scene_linear(space);}
    Tile render_level(Rect roi,RenderLevel level) const override {
        ++calls;last=roi;last_level=level;
        Tile t{roi,std::vector<float>(static_cast<std::size_t>(roi.width)*roi.height*3),output_descriptor()};
        for (std::uint32_t y=0;y<roi.height;++y)
            for (std::uint32_t x=0;x<roi.width;++x)
                for (std::size_t c=0;c<3;++c)
                    t.rgb[(static_cast<std::size_t>(y)*roi.width+x)*3+c]=
                        data[(static_cast<std::size_t>(roi.y-bounds.y+y)*bounds.width+roi.x-bounds.x+x)*3+c];
        if (bad==1) ++t.bounds.x;
        if (bad==2) t.rgb.pop_back();
        if (bad==3) t.descriptor=ImageDescriptor::camera_linear();
        if (bad==4) t.rgb[0]=std::numeric_limits<float>::infinity();
        if (bad==5) t.rgb.push_back(0);
        return t;
    }
};
struct MaskSpy final : CoverageNode {
    Rect bounds;std::vector<float> data;mutable int calls=0;mutable Rect last{};int bad=0;
    MaskSpy(Rect b,std::vector<float> v):bounds(b),data(std::move(v)) {}
    Rect native_bounds() const noexcept override {return bounds;}
    bool supports_level(RenderLevel l) const noexcept override {return native_final(l);}
    CoverageTile render_level(Rect roi,RenderLevel level) const override {
        require(native_final(level),"mask must be requested native Final");++calls;last=roi;
        CoverageTile t{roi,std::vector<float>(static_cast<std::size_t>(roi.width)*roi.height)};
        for (std::uint32_t y=0;y<roi.height;++y)
            for (std::uint32_t x=0;x<roi.width;++x)
                t.coverage[static_cast<std::size_t>(y)*roi.width+x]=
                    data[static_cast<std::size_t>(roi.y-bounds.y+y)*bounds.width+roi.x-bounds.x+x];
        if (bad==1) ++t.bounds.x;
        if (bad==2) t.coverage.pop_back();
        if (bad==3) t.coverage[0]=value(0x7fc00000);
        if (bad==4) t.coverage[0]=-1;
        if (bad==5) t.coverage.push_back(0);
        return t;
    }
};
struct RgbaSpy final : RgbaNode {
    Rect bounds;WorkingSpace space;std::vector<float> data;int bad=0;mutable int calls=0;
    mutable Rect last{};mutable RenderLevel last_level{};
    RgbaSpy(Rect b,WorkingSpace s,std::vector<float> v):bounds(b),space(s),data(std::move(v)) {}
    Rect native_bounds() const noexcept override {return bounds;}
    WorkingSpace working_space() const noexcept override {return space;}
    bool supports_level(RenderLevel l) const noexcept override {return native_final(l);}
    PremultipliedRgbaTile render_level(Rect roi,RenderLevel level) const override {
        ++calls;last=roi;last_level=level;
        PremultipliedRgbaTile t{roi,std::vector<float>(static_cast<std::size_t>(roi.width)*roi.height*4),space};
        for (std::uint32_t y=0;y<roi.height;++y)
            for (std::uint32_t x=0;x<roi.width;++x)
                for (std::size_t c=0;c<4;++c)
                    t.rgba[(static_cast<std::size_t>(y)*roi.width+x)*4+c]=
                        data[(static_cast<std::size_t>(roi.y-bounds.y+y)*bounds.width+roi.x-bounds.x+x)*4+c];
        if (bad==1) ++t.bounds.x;
        if (bad==2) t.rgba.pop_back();
        if (bad==3) t.working_space=static_cast<WorkingSpace>(-1);
        if (bad==4) t.rgba[0]=value(0x7fc00000);
        if (bad==5) t.rgba[3]=2;
        if (bad==6) {t.rgba[3]=0;t.rgba[0]=1;}
        if (bad==7) t.rgba.push_back(0);
        if (bad==8) t.rgba[3]=-1;
        return t;
    }
};
void exact(const std::vector<float>& data,const std::vector<std::uint32_t>& expected,
           Rect full,Rect roi,std::size_t channels) {
    require(data.size()==static_cast<std::size_t>(roi.width)*roi.height*channels,"exact storage");
    for (std::uint32_t y=0;y<roi.height;++y)
        for (std::uint32_t x=0;x<roi.width;++x)
            for (std::size_t c=0;c<channels;++c) {
                const auto i=(static_cast<std::size_t>(y)*roi.width+x)*channels+c;
                const auto e=(static_cast<std::size_t>(roi.y-full.y+y)*full.width+roi.x-full.x+x)*channels+c;
                require(std::bit_cast<std::uint32_t>(data[i])==expected[e],"independent fixture mismatch");
            }
}

std::size_t frames() {
    std::size_t count=0;
    for (const auto& f:rgba_native_reference::frames) {
        const Rect native{f.bounds[0],f.bounds[1],f.bounds[2],f.bounds[3]};
        const auto space=f.space==1 ? WorkingSpace::LinearProPhotoD50 : WorkingSpace::LinearRec2020D65;
        auto source_values=values(f.source);
        std::shared_ptr<const RgbaNode> source;
        auto mask=std::make_shared<MaskSpy>(native,values(f.mask));
        std::shared_ptr<RgbSpy> rgb;
        if (f.kind==1) {
            std::vector<float> color,alpha;
            for (std::size_t i=0;i<source_values.size();i+=4) {
                color.insert(color.end(),source_values.begin()+i,source_values.begin()+i+3);
                alpha.push_back(source_values[i+3]);
            }
            rgb=std::make_shared<RgbSpy>(native,color,space);
            auto a=std::make_shared<MaskSpy>(native,alpha);
            source=std::make_shared<RgbPremultiplyNode>(rgb,a,native);
            require(source->rgb_input()==rgb.get() && source->coverage_input()==a.get(),"premultiply typed edges");
        } else {
            source=std::make_shared<RgbaRasterNode>(RgbaImage({native,0,space},source_values));
            if (f.kind==2) source=std::make_shared<RgbaApplyCoverageNode>(source,mask);
            if (f.kind==3) {
                auto backdrop=std::make_shared<RgbaRasterNode>(RgbaImage({native,0,space},values(f.backdrop)));
                source=std::make_shared<RgbaSourceOverNode>(source,backdrop);
                require(source->rgba_input(0) && source->rgba_input(1) && !source->rgba_input(2),"over typed edges");
            }
        }
        if (f.kind<=3) {
            auto native_result=source->render(native);
            RgbaImage frozen({native,0,space},native_result.rgba);
            const auto digest=frozen.fingerprint();
            for (std::size_t i=0;i<32;++i) require(digest[i]==f.fingerprint[i],"independent source SHA256");
        }
        RgbaAlphaNode alpha(source);RgbaStraightRgbNode straight(source);
        for (const auto level: {RenderLevel{},RenderLevel{0,RenderQuality::Preview},
                               RenderLevel{1,RenderQuality::Preview},RenderLevel{2,RenderQuality::Preview}}) {
            const Rect full=source->output_bounds(level);
            for (std::uint32_t y=0;y<full.height;++y)
                for (std::uint32_t x=0;x<full.width;++x)
                    for (std::uint32_t h=1;h<=full.height-y;++h)
                        for (std::uint32_t w=1;w<=full.width-x;++w) {
                            const Rect roi{full.x+x,full.y+y,w,h};
                            const auto input=source->input_region_level(roi,native,level);
                            require(native_final(source->input_level(level)),"native Final input");
                            if (f.kind==4) {
                                const auto tile=alpha.render_level(roi,level);
                                require(same(tile.bounds,roi),"alpha ROI");
                                exact(tile.coverage,f.levels[level.mip],full,roi,1);
                                require(same(alpha.input_region_level(roi,native,level),input),"alpha mapping");
                            } else if (f.kind==5) {
                                const auto tile=straight.render_level(roi,level);
                                require(tile.descriptor==ImageDescriptor::scene_linear(space),"straight descriptor");
                                exact(tile.rgb,f.levels[level.mip],full,roi,3);
                                require(same(straight.input_region_level(roi,native,level),input),"straight mapping");
                            } else {
                                const auto tile=source->render_level(roi,level);
                                require(same(tile.bounds,roi) && tile.working_space==space,"RGBA output identity");
                                exact(tile.rgba,f.levels[level.mip],full,roi,4);
                                validate_premultiplied_rgba_tile(tile);
                            }
                            if (rgb) require(same(rgb->last,input) && native_final(rgb->last_level),"actual RGB native ROI");
                            if (f.kind==2) require(same(mask->last,input),"actual coverage native ROI");
                            ++count;
                        }
        }
    }
    return count;
}

void pixel_oracles() {
    const Rect b{0,0,1,1};
    for (const auto& c:alpha_reference::cases) {
        std::vector<float> s,bg;for (std::size_t i=0;i<4;++i) {s.push_back(value(c.input[i]));bg.push_back(value(c.input[i+4]));}
        auto base=std::make_shared<RgbaRasterNode>(RgbaImage({b},c.kind==0 ? std::vector<float>{0,0,0,0} : s));
        std::shared_ptr<const RgbaNode> node;
        if (c.kind==0) node=std::make_shared<RgbPremultiplyNode>(
            std::make_shared<RgbSpy>(b,std::vector<float>{s[0],s[1],s[2]},WorkingSpace::LinearProPhotoD50),
            std::make_shared<MaskSpy>(b,std::vector<float>{s[3]}),b);
        if (c.kind==1) node=std::make_shared<RgbaApplyCoverageNode>(base,std::make_shared<MaskSpy>(b,std::vector<float>{bg[0]}));
        if (c.kind==2) node=std::make_shared<RgbaSourceOverNode>(base,std::make_shared<RgbaRasterNode>(RgbaImage({b},bg)));
        auto check=[&] {
            const auto out=c.kind==3 ? RgbaStraightRgbNode(base).render(b).rgb : node->render(b).rgba;
            for (std::size_t i=0;i<out.size();++i) require(std::bit_cast<std::uint32_t>(out[i])==c.output[i],"alpha oracle through graph");
        };
        if (c.overflow) rejects<std::overflow_error>(check);else check();
    }
    for (const auto& c:alpha_reference::straight_cases) {
        PremultipliedRgbaTile tile{b,{value(c.input[0]),value(c.input[1]),value(c.input[2]),value(c.input[3])}};
        const auto out=straight_rgb_float64(tile);
        for (std::size_t i=0;i<3;++i) require(std::bit_cast<std::uint64_t>(out.rgb[i])==c.output[i],"wide exact oracle");
    }
}

void ownership_and_guards() {
    const Rect b{9,15,2,1};const auto space=WorkingSpace::LinearProPhotoD50;
    std::vector<float> padded{value(0x80000000),0,0,value(0x80000000),-2,4,value(0x80000000),.5f,value(0x7fc00000)};
    RgbaImage image({b,9,space},padded);const auto hash=image.fingerprint();
    padded[4]=1;
    require(image.samples()[4]==-2 && image.metadata().row_stride_samples==8,"source ownership and stride");
    for (std::size_t i=0;i<4;++i) require(std::bit_cast<std::uint32_t>(image.samples()[i])==0,"transparent canonicalization");
    require(std::bit_cast<std::uint32_t>(image.samples()[6])==0x80000000,"nontransparent negative zero retained");
    require(RgbaImage({b,0,space},image.samples()).fingerprint()==hash,"padding does not affect hash");
    require(RgbaImage({b,0,WorkingSpace::LinearRec2020D65},image.samples()).fingerprint()!=hash,"space affects hash");
    require(RgbaImage({{10,15,2,1},0,space},image.samples()).fingerprint()!=hash,"origin affects hash");
    std::vector<std::future<std::array<std::uint8_t,32>>> futures;
    RgbaImage fresh({b,0,space},image.samples());
    for (int i=0;i<8;++i) futures.push_back(std::async(std::launch::async,[fresh]{return fresh.fingerprint();}));
    for (auto& f:futures) require(f.get()==hash,"concurrent immutable source fingerprint");
    rejects([&]{RgbaImage({b,7,space},image.samples());});
    rejects([&]{RgbaImage({{0,0,0,1}},{});});
    rejects([&]{RgbaImage({{0xffffffff,0,1,1}},{0,0,0,0});});
    rejects([&]{RgbaImage({b,0,static_cast<WorkingSpace>(-1)},image.samples());});
    for (const auto input: {std::vector<float>{1,0,0,0},std::vector<float>{0,0,0,2},
                            std::vector<float>{value(0x7fc00000),0,0,1}})
        rejects([&]{RgbaImage({{0,0,1,1}},input);});
    auto rgba=std::make_shared<RgbaSpy>(b,space,std::vector<float>{0,0,0,0,0,0,0,0});
    auto mask=std::make_shared<MaskSpy>(b,std::vector<float>{0,0});
    auto rgb=std::make_shared<RgbSpy>(b,std::vector<float>(6),space);
    RgbaApplyCoverageNode coverage(rgba,mask);RgbaSourceOverNode over(rgba,rgba);
    RgbaAlphaNode alpha(rgba);RgbaStraightRgbNode straight(rgba);RgbPremultiplyNode premultiply(rgb,mask,b);
    for (int bad=1;bad<=8;++bad) {
        rgba->bad=bad;
        auto checks=[&] {over.render(b);};
        if (bad<=1 || bad==3) {
            rejects<std::domain_error>(checks);rejects<std::domain_error>([&]{coverage.render(b);});
            rejects<std::domain_error>([&]{alpha.render(b);});rejects<std::domain_error>([&]{straight.render(b);});
        } else {
            rejects(checks);rejects([&]{coverage.render(b);});rejects([&]{alpha.render(b);});rejects([&]{straight.render(b);});
        }
    }
    rgba->bad=0;
    for (int bad=1;bad<=5;++bad) {
        mask->bad=bad;
        if (bad==1) {rejects<std::domain_error>([&]{coverage.render(b);});rejects<std::domain_error>([&]{premultiply.render(b);});}
        else {rejects([&]{coverage.render(b);});rejects([&]{premultiply.render(b);});}
    }
    mask->bad=0;
    for (int bad=1;bad<=5;++bad) {
        rgb->bad=bad;
        if (bad==1 || bad==3) rejects<std::domain_error>([&]{premultiply.render(b);});
        else rejects([&]{premultiply.render(b);});
    }
    rgb->bad=0;
    for (const auto level: {RenderLevel{1,RenderQuality::Final},RenderLevel{3,RenderQuality::Preview},
                           RenderLevel{0,static_cast<RenderQuality>(-1)}}) {
        const auto before=rgba->calls;
        rejects([&]{over.render_level(b,level);});rejects([&]{alpha.render_level(b,level);});
        rejects([&]{straight.render_level(b,level);});require(before==rgba->calls,"level guard before upstream");
    }
    const auto before=rgba->calls;
    rejects([&]{over.render({8,15,1,1});});rejects([&]{over.render({9,15,0,1});});
    rejects([&]{over.input_region_level(b,{0,0,2,1},{});});
    require(before==rgba->calls,"ROI and extent guard before upstream");
    rejects([&]{RgbaSourceOverNode(nullptr,rgba);});rejects([&]{RgbaApplyCoverageNode(rgba,nullptr);});
    rejects([&]{RgbaAlphaNode(nullptr);});rejects([&]{RgbaStraightRgbNode(nullptr);});
    rejects([&]{RgbPremultiplyNode(nullptr,mask,b);});
    auto other=std::make_shared<RgbaSpy>(b,WorkingSpace::LinearRec2020D65,std::vector<float>(8));
    rejects([&]{RgbaSourceOverNode(rgba,other);});
    other->space=space;other->bounds.width=1;rejects([&]{RgbaSourceOverNode(rgba,other);});
    auto huge=std::make_shared<RgbaSpy>(Rect{0,0,0xffffffff,0xffffffff},space,std::vector<float>{});
    RgbaSourceOverNode large(huge,huge);
    rejects([&]{large.render(huge->bounds);});
    require(huge->calls==0,"resource guard before upstream");
    PremultipliedRgbaTile tiny{{0,0,1,1},{value(0x7f7fffff),-1,0,value(1)}};
    const auto wide=straight_rgb_float64(tiny);
    require(wide.rgb[0]>std::numeric_limits<float>::max(),"wide access without epsilon floor");
    auto tiny_node=std::make_shared<RgbaRasterNode>(RgbaImage({tiny.bounds},tiny.rgba));
    rejects<std::overflow_error>([&]{RgbaStraightRgbNode(tiny_node).render(tiny.bounds);});
    // Actual typed native footprints at a clipped shifted-origin preview cell.
    const Rect shifted{11,17,7,5};
    auto visible=std::make_shared<RgbaSpy>(shifted,space,std::vector<float>(7*5*4,0));
    auto visible_mask=std::make_shared<MaskSpy>(shifted,std::vector<float>(7*5,1));
    RgbaApplyCoverageNode visible_node(visible,visible_mask);
    (void)visible_node.render_level({1,1,1,1},{2,RenderQuality::Preview});
    require(same(visible->last,{15,21,3,1}) && same(visible_mask->last,visible->last) &&
            native_final(visible->last_level),"actual clipped native point footprints");
    // A reduced alpha that underflows cannot retain hidden premultiplied color.
    auto underflow=std::make_shared<RgbaRasterNode>(RgbaImage({{0,0,2,2}},
        {1,-1,2,value(1),0,0,0,0,0,0,0,0,0,0,0,0}));
    const auto reduced=underflow->render_level({0,0,1,1},{1,RenderQuality::Preview});
    for (float v:reduced.rgba) require(std::bit_cast<std::uint32_t>(v)==0,"reduced transparent canonicalization");
    // An opaque foreground does not permit invalid backdrop bypass.
    auto opaque=std::make_shared<RgbaSpy>(b,space,std::vector<float>{1,2,3,1,1,2,3,1});
    rgba->bad=6;
    rejects([&]{RgbaSourceOverNode(opaque,rgba).render(b);});
    rgba->bad=0;
    const auto old=std::fegetround();
    for (int mode: {FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}) {
        require(std::fesetround(mode)==0,"set rounding mode");
        (void)over.render_level({0,0,1,1},{1,RenderQuality::Preview});
        require(std::fegetround()==mode,"caller rounding mode retained");
    }
    std::fesetround(old);
}
} // namespace
int main() {
    try {
        require(std::fegetround()==FE_TONEAREST,"fixtures require nearest even");
        const auto rois=frames();pixel_oracles();ownership_and_guards();
        std::cout<<"84 independent frames; "<<rois<<" exhaustive level/ROI comparisons; alpha/wide oracles and guards passed\n";
        return 0;
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
