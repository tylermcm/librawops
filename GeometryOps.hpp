#pragma once
#include "RawEngine.hpp"
#include <array>

namespace rawengine {
struct RAWENGINE_API RotateSettings {
    double angle_degrees = 0;
};
RAWENGINE_API void validate_rotate_settings(const RotateSettings& settings);

// Fixed-canvas clockwise rotation around the current image's pixel-center pivot.
// True source edges replicate; mip previews average rotated native pixels.
// See docs/ROTATE_CONTRACT_V1.md for strict sampling and bounded block rules.
class RAWENGINE_API RotateNode final : public Node {
public:
    RotateNode(std::shared_ptr<const Node> input, Rect input_bounds,
               RotateSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region(Rect output, Rect input_bounds) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
    Rect output_bounds() const noexcept { return {0, 0, input_bounds_.width, input_bounds_.height}; }
private:
    struct Taps {
        std::uint32_t x0, y0, x1, y1;
        double fx, fy;
    };
    Taps taps(std::uint32_t x, std::uint32_t y) const;
    Rect native_output_region(Rect output, RenderLevel level) const;
    Rect support(Rect native_output) const;
    Tile native_block(Rect output) const;
    std::shared_ptr<const Node> input_;
    Rect input_bounds_;
    RotateSettings settings_;
    ImageDescriptor descriptor_;
    double cx_, cy_, cosine_, sine_;
};
struct RAWENGINE_API ProjectiveSettings {
    std::uint32_t width = 0, height = 0;
    std::array<double,9> source_from_output{1,0,0,0,1,0,0,0,1};
};
RAWENGINE_API void validate_projective_settings(const ProjectiveSettings& settings);

// Explicit canvas and normalized inverse homography; see the frozen v1 contract.
class RAWENGINE_API ProjectiveNode final : public Node {
public:
    ProjectiveNode(std::shared_ptr<const Node> input, Rect input_bounds,
                   ProjectiveSettings settings);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region(Rect output, Rect input_bounds) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
    Rect output_bounds() const noexcept { return {0,0,settings_.width,settings_.height}; }
private:
    struct Taps { std::uint32_t x0,y0,x1,y1; double fx,fy; };
    Taps taps(std::uint32_t x,std::uint32_t y) const;
    Rect native_output_region(Rect output,RenderLevel level) const;
    Rect support(Rect native_output) const;
    Tile native_block(Rect output) const;
    void sample_into(Rect output,Rect mapped,Tile& native) const;
    void fallback_row(Rect row,Tile& native) const;
    void render_block(Rect block,RenderLevel level,Tile& result) const;
    std::shared_ptr<const Node> input_;
    Rect input_bounds_;
    ProjectiveSettings settings_;
    ImageDescriptor descriptor_;
    bool identity_;
};
struct RAWENGINE_API CubicResizeSettings {
    std::uint32_t width = 0, height = 0;
};
RAWENGINE_API void validate_cubic_resize_settings(const CubicResizeSettings& settings,
                                                 Rect native_input_bounds);

// Fixed scale-aware Catmull-Rom with at-most-fourfold native shrink per axis.
// See docs/CUBIC_RESIZE_CONTRACT_V1.md for arithmetic, admission and work bounds.
class RAWENGINE_API CubicResizeNode final : public Node {
public:
    CubicResizeNode(std::shared_ptr<const Node> input, Rect input_bounds,
                    CubicResizeSettings settings);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region(Rect output, Rect input_bounds) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
    Rect output_bounds() const noexcept { return {0,0,settings_.width,settings_.height}; }
private:
    struct Tap { std::uint32_t index; double weight; };
    struct Axis { std::uint32_t center, count; std::array<Tap,18> taps; };
    static Axis axis(std::uint32_t input, std::uint32_t output, std::uint32_t index);
    Rect native_output_region(Rect output, RenderLevel level) const;
    Rect support(Rect native_output) const;
    Tile native_block(Rect output) const;
    std::shared_ptr<const Node> input_;
    Rect input_bounds_;
    CubicResizeSettings settings_;
    ImageDescriptor descriptor_;
};
} // namespace rawengine
