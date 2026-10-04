#pragma once

#include "RawEngine.hpp"

namespace rawengine {

struct CurvePoint { double x = 0, y = 0; };
struct PiecewiseLinearCurve {
    std::vector<CurvePoint> knots{{0,0},{1,1}}; // 2..256, strictly increasing x.
};
struct CurvesSettings {
    std::array<PiecewiseLinearCurve,3> channels; // R/G/B in declared working space.
};
struct ChannelLevels {
    double input_black = 0, input_white = 1;
    double output_black = 0, output_white = 1;
};
struct LevelsSettings { std::array<ChannelLevels,3> channels; };
enum class CurveInterpolation { Linear, ShapePreservingCubic };
struct CurveKnots { std::vector<CurvePoint> knots{{0,0},{1,1}}; };
struct ExtendedCurvesSettings {
    CurveKnots master;
    std::array<CurveKnots,3> channels;
    CurveInterpolation interpolation = CurveInterpolation::Linear;
};
struct GammaLevelsSettings {
    std::array<ChannelLevels,3> channels;
    std::array<double,3> gamma{1,1,1};
};
RAWENGINE_API void validate_extended_curves_settings(const ExtendedCurvesSettings& settings);
RAWENGINE_API void validate_gamma_levels_settings(const GammaLevelsSettings& settings);

// Frozen v1: master then channel in binary64, one final float32 cast.
class RAWENGINE_API ExtendedCurvesNode final : public Node {
public:
    ExtendedCurvesNode(std::shared_ptr<const Node> input, ExtendedCurvesSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    double map(unsigned curve,double value) const;
    std::shared_ptr<const Node> input_;
    ExtendedCurvesSettings settings_;
    std::array<std::vector<double>,4> slopes_;
    std::array<std::vector<std::array<double,2>>,4> controls_;
    std::array<bool,4> identity_{};
};

// Signed normalized power; gamma1 preserves the existing affine arithmetic.
class RAWENGINE_API GammaLevelsNode final : public Node {
public:
    GammaLevelsNode(std::shared_ptr<const Node> input, GammaLevelsSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    GammaLevelsSettings settings_;
    std::array<double,3> spans_{},slopes_{};
    std::array<bool,3> identity_{};
};
struct Lut1DSettings {
    double input_min = 0, input_max = 1;
    std::array<std::vector<double>,3> channels{{{0,1},{0,1},{0,1}}}; // Equal 2..256 samples.
};
struct Lut3DSettings {
    std::uint32_t size = 2; // Cubic grid, 2..17 points per axis.
    std::array<double,3> input_min{0,0,0}, input_max{1,1,1};
    // Red-fastest lattice; adjacent output RGB scalars. Default identity cube.
    std::vector<double> values{0,0,0,1,0,0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,1,1,1,1};
};
struct SaturationSettings { double amount = 1; }; // Finite 0..4; 1 is exact identity.
struct VibranceSettings { double amount = 0; }; // Finite -1..1; 0 is exact identity.
struct ColorMixerSettings {
    // Red, orange, yellow, green, aqua, blue, purple, magenta.
    std::array<double,8> hue_shift{}, saturation_delta{}, luminance_delta{};
};
struct ColorBalanceSettings {
    std::array<double,3> shadows{}, midtones{}, highlights{}; // Finite RGB offsets, +/-1.
    bool preserve_luminance = true;
};
struct GradingSettings {
    std::array<double,3> lift{}, gain{1,1,1}, gamma{1,1,1};
};
struct DehazeSettings {
    double amount = 0; // Finite -1..1; zero preserves bits.
    std::array<double,3> atmospheric_light{1,1,1}; // Finite scene-linear RGB 0..4.
};
struct TonalRangeSettings {
    // Original ordered compact tonal warps; normalized amounts, not EV stops.
    double blacks = 0, shadows = 0, highlights = 0, whites = 0;
};
struct ChannelMixerSettings {
    std::array<double,9> matrix{1,0,0,0,1,0,0,0,1}; // Row-major RGB; finite |coefficient|<=64.
};

// Coordinates and segment slopes are finite with magnitude <=65536. Levels
// require increasing input endpoints and nondecreasing output endpoints.
RAWENGINE_API void validate_curves_settings(const CurvesSettings& settings);
RAWENGINE_API void validate_levels_settings(const LevelsSettings& settings);
RAWENGINE_API void validate_lut1d_settings(const Lut1DSettings& settings);
RAWENGINE_API void validate_lut3d_settings(const Lut3DSettings& settings);
RAWENGINE_API void validate_large_lut1d_settings(const Lut1DSettings& settings);
RAWENGINE_API void validate_large_lut3d_settings(const Lut3DSettings& settings);
RAWENGINE_API void validate_grading_settings(const GradingSettings& settings);
RAWENGINE_API void validate_dehaze_settings(const DehazeSettings& settings);
RAWENGINE_API void validate_tonal_range_settings(const TonalRangeSettings& settings);
RAWENGINE_API void validate_saturation_settings(const SaturationSettings& settings);
RAWENGINE_API void validate_vibrance_settings(const VibranceSettings& settings);
RAWENGINE_API void validate_color_mixer_settings(const ColorMixerSettings& settings);
RAWENGINE_API void validate_color_balance_settings(const ColorBalanceSettings& settings);
RAWENGINE_API void validate_channel_mixer_settings(const ChannelMixerSettings& settings);

// Creative point edits on scene-linear ProPhoto/D50 or Rec.2020/D65 RGB.
// First/last segments extrapolate signed values and headroom. No clipping,
// normalization, gamma or display transfer. Identity channels preserve bits.
class RAWENGINE_API CurvesNode final : public Node {
public:
    CurvesNode(std::shared_ptr<const Node> input, CurvesSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    friend class LargeLut1DNode;
    CurvesNode(std::shared_ptr<const Node> input, CurvesSettings settings, std::size_t knot_limit);
    std::shared_ptr<const Node> input_;
    CurvesSettings settings_;
    std::array<std::vector<double>,3> slopes_;
    std::array<bool,3> identity_{};
};

// Affine per-channel black/white mapping, represented by two curve knots.
// This foundation has no midtone gamma or implicit clamp to output endpoints.
class RAWENGINE_API LevelsNode final : public Node {
public:
    LevelsNode(std::shared_ptr<const Node> input, LevelsSettings settings = {});
    Tile render(Rect bounds) const override { return curves_.render(bounds); }
    Tile render_level(Rect bounds, RenderLevel level) const override { return curves_.render_level(bounds,level); }
    bool supports_level(RenderLevel level) const noexcept override { return curves_.supports_level(level); }
    Rect input_region_level(Rect output, Rect bounds, RenderLevel level) const override {
        return curves_.input_region_level(output,bounds,level);
    }
    const Node* input_node() const noexcept override { return curves_.input_node(); }
    ImageDescriptor output_descriptor() const noexcept override { return curves_.output_descriptor(); }
private:
    CurvesNode curves_;
};

// Uniform sampled RGB tables over an explicit shared input range. Delegates
// strict linear interpolation/extrapolation and exact identity to CurvesNode.
class RAWENGINE_API Lut1DNode final : public Node {
public:
    Lut1DNode(std::shared_ptr<const Node> input, Lut1DSettings settings = {});
    Tile render(Rect bounds) const override { return curves_.render(bounds); }
    Tile render_level(Rect bounds, RenderLevel level) const override { return curves_.render_level(bounds,level); }
    bool supports_level(RenderLevel level) const noexcept override { return curves_.supports_level(level); }
    Rect input_region_level(Rect output, Rect bounds, RenderLevel level) const override {
        return curves_.input_region_level(output,bounds,level);
    }
    const Node* input_node() const noexcept override { return curves_.input_node(); }
    ImageDescriptor output_descriptor() const noexcept override { return curves_.output_descriptor(); }
private:
    CurvesNode curves_;
};

// Ordered R/G/B trilinear interpolation with boundary-cell extrapolation.
// Exact identity components copy bits; finite samples/float32 overflow checked.
class RAWENGINE_API Lut3DNode final : public Node {
public:
    Lut3DNode(std::shared_ptr<const Node> input, Lut3DSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    friend class LargeLut3DNode;
    Lut3DNode(std::shared_ptr<const Node> input, Lut3DSettings settings, std::uint32_t size_limit);
    std::shared_ptr<const Node> input_;
    Lut3DSettings settings_;
    std::array<std::vector<double>,3> axes_;
    std::array<bool,3> identity_{true,true,true};
};

// Scale scene-linear chroma around native working-space Y. No clipping,
// normalization, display transfer or adaptive/skin-selective vibrance.
class RAWENGINE_API SaturationNode final : public Node {
public:
    SaturationNode(std::shared_ptr<const Node> input, SaturationSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    SaturationSettings settings_;
    double weight_red_ = 0, weight_blue_ = 0;
};

// Original bounded adaptive chroma around native Y. Weight = abs(Y) /
// (abs(Y) + RGB range); no epsilon, clipping, skin or perceptual hue policy.
class RAWENGINE_API VibranceNode final : public Node {
public:
    VibranceNode(std::shared_ptr<const Node> input, VibranceSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    VibranceSettings settings_;
    double weight_red_ = 0, weight_blue_ = 0;
};

// Bounded linear RGB mixing in the declared working space. No offsets,
// clipping, row normalization or working-space conversion. Unit rows copy bits.
class RAWENGINE_API ChannelMixerNode final : public Node {
public:
    ChannelMixerNode(std::shared_ptr<const Node> input, ChannelMixerSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    ChannelMixerSettings settings_;
    std::array<int,3> selected_channel_{-1,-1,-1};
};

// Original scene-linear hue/native-Y mixer; fixed overlapping hue bands.
// No clipping, display HSL conversion, epsilon or perceptual/skin policy.
class RAWENGINE_API ColorMixerNode final : public Node {
public:
    ColorMixerNode(std::shared_ptr<const Node> input, ColorMixerSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    ColorMixerSettings settings_;
    double weight_red_ = 0, weight_blue_ = 0;
    bool identity_ = true;
};

// Original quadratic native-Y tonal weights and RGB offsets; optional Y
// projection. Can tint neutrals/black; no clipping or display-lightness policy.
class RAWENGINE_API ColorBalanceNode final : public Node {
public:
    ColorBalanceNode(std::shared_ptr<const Node> input, ColorBalanceSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    ColorBalanceSettings settings_;
    double weight_red_ = 0, weight_blue_ = 0;
    bool identity_ = true;
};

// Parameter-free working-Y monochrome in RGB storage. Neutral triples copy bits.
class RAWENGINE_API GrayscaleNode final : public Node {
public:
    explicit GrayscaleNode(std::shared_ptr<const Node> input);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    double weight_red_ = 0, weight_blue_ = 0;
};

class RAWENGINE_API LargeLut1DNode final : public Node {
public:
    LargeLut1DNode(std::shared_ptr<const Node> input, Lut1DSettings settings = {});
    Tile render(Rect r) const override { return curves_.render(r); }
    Tile render_level(Rect r, RenderLevel l) const override { return curves_.render_level(r,l); }
    bool supports_level(RenderLevel l) const noexcept override { return curves_.supports_level(l); }
    Rect input_region_level(Rect r, Rect b, RenderLevel l) const override { return curves_.input_region_level(r,b,l); }
    const Node* input_node() const noexcept override { return curves_.input_node(); }
    ImageDescriptor output_descriptor() const noexcept override { return curves_.output_descriptor(); }
private:
    CurvesNode curves_;
};
class RAWENGINE_API LargeLut3DNode final : public Node {
public:
    LargeLut3DNode(std::shared_ptr<const Node> input, Lut3DSettings settings = {});
    Tile render(Rect r) const override { return node_.render(r); }
    Tile render_level(Rect r, RenderLevel l) const override { return node_.render_level(r,l); }
    bool supports_level(RenderLevel l) const noexcept override { return node_.supports_level(l); }
    Rect input_region_level(Rect r, Rect b, RenderLevel l) const override { return node_.input_region_level(r,b,l); }
    const Node* input_node() const noexcept override { return node_.input_node(); }
    ImageDescriptor output_descriptor() const noexcept override { return node_.output_descriptor(); }
private:
    Lut3DNode node_;
};
class RAWENGINE_API GradingNode final : public Node {
public:
    GradingNode(std::shared_ptr<const Node> input, GradingSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    GradingSettings settings_;
};

// Original explicit uniform-transmission policy. No atmosphere/depth estimation,
// clipping or halo; positive removal can amplify noise up to eightfold.
class RAWENGINE_API DehazeNode final : public Node {
public:
    DehazeNode(std::shared_ptr<const Node> input, DehazeSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    DehazeSettings settings_;
    double coefficient_ = 0;
};

// Monotone working-Y curve with a common RGB gain and odd signed extension.
// Point support; requested-level input precedes mapping. No clipping/recovery.
class RAWENGINE_API TonalRangeNode final : public Node {
public:
    TonalRangeNode(std::shared_ptr<const Node> input, TonalRangeSettings settings = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return input_->output_descriptor(); }
private:
    std::shared_ptr<const Node> input_;
    TonalRangeSettings settings_;
    double weight_red_ = 0, weight_blue_ = 0;
    bool identity_ = true;
};

} // namespace rawengine
