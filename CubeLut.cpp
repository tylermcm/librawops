#include "CubeLut.hpp"
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace rawengine {
namespace {
constexpr std::size_t byte_limit=4*1024*1024;
EditDomain domain(WorkingSpace space) {
    if (space==WorkingSpace::LinearProPhotoD50) return EditDomain::SceneLinearProPhotoD50;
    if (space==WorkingSpace::LinearRec2020D65) return EditDomain::SceneLinearRec2020D65;
    throw std::invalid_argument("Cube requires explicit supported scene-linear working space");
}
bool digit(char c) {return c>='0' && c<='9';}
double numeric(std::string_view text) {
    std::size_t i=0;
    if (i<text.size() && (text[i]=='+' || text[i]=='-')) ++i;
    std::size_t count=0;
    while (i<text.size() && digit(text[i])) {++i;++count;}
    if (i<text.size() && text[i]=='.') {
        ++i;while (i<text.size() && digit(text[i])) {++i;++count;}
    }
    if (!count) throw std::invalid_argument("Cube needs decimal numbers");
    if (i<text.size() && (text[i]=='e' || text[i]=='E')) {
        ++i;if (i<text.size() && (text[i]=='+' || text[i]=='-')) ++i;
        const auto start=i;while (i<text.size() && digit(text[i])) ++i;
        if (i==start) throw std::invalid_argument("Cube exponent needs digits");
    }
    if (i!=text.size()) throw std::invalid_argument("invalid Cube numeric token");
    if (text[0]=='+') text.remove_prefix(1);
    double result=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || !std::isfinite(result) || std::abs(result)>65536)
        throw std::invalid_argument("Cube numeric value exceeds engine bounds");
    return result;
}
EditValue array_value(const auto& values) {
    EditValue::Array result;result.reserve(values.size());
    for (double v:values) result.push_back(EditValue{v});
    return EditValue{std::move(result)};
}
}
CubeLut parse_cube_lut(std::string_view text, WorkingSpace space) {
    (void)domain(space);
    if (text.size()>byte_limit) throw std::length_error("Cube text exceeds 4 MiB");
    for (unsigned char c:text)
        if (c!='\r' && c!='\n' && c!='\t' && (c<32 || c>126))
            throw std::invalid_argument("Cube requires ASCII text");
    CubeLut result{space,{},{Lut1DSettings{}}};
    std::array<double,3> lo{0,0,0},hi{1,1,1};
    std::set<std::string> keywords;std::vector<double> rows;
    unsigned size=0;bool three=false,data=false;std::size_t expected=0;
    for (std::size_t begin=0;begin<text.size();) {
        const auto end=text.find('\n',begin);
        auto line=text.substr(begin,end==text.npos ? text.size()-begin : end-begin);
        begin=end==text.npos ? text.size() : end+1;
        if (!line.empty() && line.back()=='\r' && end!=text.npos) line.remove_suffix(1);
        if (line.size()>250 || line.find('\r')!=line.npos) throw std::invalid_argument("invalid Cube line length/separator");
        const auto first=line.find_first_not_of(" \t");
        if (first==line.npos || line[first]=='#') continue;
        line.remove_prefix(first);
        const auto last=line.find_last_not_of(" \t");line=line.substr(0,last+1);
        std::istringstream stream{std::string(line)};std::string token;stream>>token;
        if (!token.empty() && ((token[0]>='A' && token[0]<='Z') || (token[0]>='a' && token[0]<='z'))) {
            if (data || !keywords.insert(token).second) throw std::invalid_argument("Cube header after data or duplicate keyword");
            if (token=="TITLE") {
                auto title=line.substr(token.size());const auto quote=title.find_first_not_of(" \t");
                if (quote==title.npos) throw std::invalid_argument("Cube title needs quotes");
                title.remove_prefix(quote);
                if (title.size()<2 || title.front()!='"' || title.back()!='"' || title.substr(1,title.size()-2).find('"')!=title.npos)
                    throw std::invalid_argument("Cube title needs one quoted string");
                result.title=std::string(title.substr(1,title.size()-2));continue;
            }
            std::vector<std::string> args;while (stream>>token) args.push_back(token);
            const auto& key=std::string(line.substr(0,line.find_first_of(" \t")));
            if (key=="DOMAIN_MIN" || key=="DOMAIN_MAX") {
                if (args.size()!=3) throw std::invalid_argument("Cube domain needs three values");
                auto& out=key=="DOMAIN_MIN" ? lo : hi;
                for (unsigned c=0;c<3;++c) out[c]=numeric(args[c]);
            } else if (key=="LUT_1D_SIZE" || key=="LUT_3D_SIZE") {
                if (size || args.size()!=1 || args[0].empty()) throw std::invalid_argument("Cube requires exactly one table size");
                for (char c:args[0]) if (!digit(c)) throw std::invalid_argument("Cube size must be an integer");
                const auto parsed=std::from_chars(args[0].data(),args[0].data()+args[0].size(),size);
                three=key=="LUT_3D_SIZE";
                if (parsed.ec!=std::errc{} || size<2 || size>(three ? 33u : 4096u)) throw std::invalid_argument("Cube size exceeds engine bounds");
                expected=three ? std::size_t(size)*size*size : size;
                rows.reserve(3*expected);
            } else throw std::invalid_argument("unsupported Cube keyword");
        } else {
            if (!size || rows.size()/3>=expected) throw std::invalid_argument("Cube missing size or excess rows");
            data=true;std::array<double,3> row;row[0]=numeric(token);
            for (unsigned c=1;c<3;++c) {if (!(stream>>token)) throw std::invalid_argument("Cube row needs three values");row[c]=numeric(token);}
            if (stream>>token) throw std::invalid_argument("Cube row has extra values");
            rows.insert(rows.end(),row.begin(),row.end());
        }
    }
    if (!size || rows.size()!=3*expected) throw std::invalid_argument("Cube row count differs from size");
    if (three) {
        Lut3DSettings s;s.size=size;s.input_min=lo;s.input_max=hi;s.values=std::move(rows);
        validate_large_lut3d_settings(s);result.table=std::move(s);
    } else {
        if (lo[0]!=lo[1] || lo[1]!=lo[2] || hi[0]!=hi[1] || hi[1]!=hi[2])
            throw std::invalid_argument("engine 1D Cube requires shared channel domains");
        Lut1DSettings s;s.input_min=lo[0];s.input_max=hi[0];
        for (unsigned c=0;c<3;++c) {s.channels[c].clear();s.channels[c].reserve(size);for (unsigned i=0;i<size;++i)s.channels[c].push_back(rows[3*i+c]);}
        validate_large_lut1d_settings(s);result.table=std::move(s);
    }
    return result;
}
CubeLut load_cube_lut(const std::filesystem::path& path, WorkingSpace space) {
    (void)domain(space);
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("cannot open Cube file");
    std::string text;std::array<char,8192> buffer;
    while (file) {
        file.read(buffer.data(),buffer.size());const auto count=std::size_t(file.gcount());
        if (count>byte_limit-text.size()) throw std::length_error("Cube file exceeds 4 MiB");
        text.append(buffer.data(),count);
    }
    if (!file.eof()) throw std::runtime_error("cannot read Cube file");
    return parse_cube_lut(text,space);
}
EditOperation cube_lut_operation(const CubeLut& lut,std::string id,std::string input_id) {
    EditOperation op;op.id=std::move(id);op.inputs={{"image",std::move(input_id)}};
    op.processing_version=2;op.input_domain=op.output_domain=domain(lut.working_space);
    if (const auto* s=std::get_if<Lut1DSettings>(&lut.table)) {
        validate_large_lut1d_settings(*s);
        op.type_id=s->channels[0].size()<=256 ? "rawengine.lut1d" : "rawengine.lut1d_large";
        EditValue::Array channels;for (const auto& c:s->channels)channels.push_back(array_value(c));
        op.parameters={{"input_min",EditValue{s->input_min}},{"input_max",EditValue{s->input_max}},{"channels",EditValue{std::move(channels)}}};
    } else {
        const auto& cube=std::get<Lut3DSettings>(lut.table);validate_large_lut3d_settings(cube);
        op.type_id=cube.size<=17 ? "rawengine.lut3d" : "rawengine.lut3d_large";
        op.parameters={{"size",EditValue{std::int64_t(cube.size)}},{"input_min",array_value(cube.input_min)},
            {"input_max",array_value(cube.input_max)},{"values",array_value(cube.values)}};
    }
    return op;
}
std::shared_ptr<const Node> make_cube_lut_node(std::shared_ptr<const Node> input,const CubeLut& lut) {
    (void)domain(lut.working_space);
    if (input && input->output_descriptor()!=ImageDescriptor::scene_linear(lut.working_space))
        throw std::invalid_argument("Cube working space differs from input descriptor");
    if (const auto* s=std::get_if<Lut1DSettings>(&lut.table)) {
        if (s->channels[0].size()<=256) return std::make_shared<Lut1DNode>(std::move(input),*s);
        return std::make_shared<LargeLut1DNode>(std::move(input),*s);
    }
    const auto& s=std::get<Lut3DSettings>(lut.table);
    if (s.size<=17) return std::make_shared<Lut3DNode>(std::move(input),s);
    return std::make_shared<LargeLut3DNode>(std::move(input),s);
}
} // namespace rawengine
