#pragma once
#include "ToneOps.hpp"
#include "EditGraph.hpp"
#include <filesystem>
#include <variant>

namespace rawengine {
struct CubeLut {
    WorkingSpace working_space;
    std::string title;
    std::variant<Lut1DSettings,Lut3DSettings> table;
};
// Bounded ASCII Cube subset. Domain is explicit; recipes store copied values.
RAWENGINE_API CubeLut parse_cube_lut(std::string_view text, WorkingSpace working_space);
RAWENGINE_API CubeLut load_cube_lut(const std::filesystem::path& path, WorkingSpace working_space);
RAWENGINE_API EditOperation cube_lut_operation(const CubeLut& lut, std::string id, std::string input_id);
RAWENGINE_API std::shared_ptr<const Node> make_cube_lut_node(std::shared_ptr<const Node> input, const CubeLut& lut);
} // namespace rawengine
