#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32) && defined(RAWENGINE_BUILDING)
#  define RAWENGINE_API __declspec(dllexport)
#elif defined(_WIN32)
#  define RAWENGINE_API __declspec(dllimport)
#else
#  define RAWENGINE_API
#endif

namespace rawengine {

enum class BayerPattern { RGGB, BGGR, GRBG, GBRG };
enum class WorkingSpace { LinearProPhotoD50, LinearRec2020D65 };

struct Rect {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
};

enum class RenderQuality : std::uint8_t { Preview = 0, Final = 1 };

// Coordinates in a request are expressed at its mip level. Mip 1 and 2
// previews are zero-based within the native output extent, including a RAW
// active area with a nonzero sensor origin. Native requests retain that origin.
struct RenderLevel {
    std::uint32_t mip = 0;
    RenderQuality quality = RenderQuality::Final;
};

struct RenderRequest {
    Rect viewport;
    std::uint32_t tile_size = 256;
    RenderLevel level;
    RenderRequest() = default;
    explicit constexpr RenderRequest(Rect requested_viewport,
                                     std::uint32_t requested_tile_size = 256,
                                     RenderLevel requested_level = {}) noexcept
        : viewport(requested_viewport), tile_size(requested_tile_size),
          level(requested_level) {}
};

// Site arrays are in pattern-relative row-major 2x2 order. The phase shifts
// that pattern over sensor coordinates; (0,0) indexes phase_x,phase_y.
struct RawMetadata {
    std::uint32_t width = 0, height = 0;
    std::uint32_t row_stride_samples = 0; // 0 means tightly packed width.
    BayerPattern pattern = BayerPattern::RGGB;
    std::uint8_t cfa_phase_x = 0, cfa_phase_y = 0;
    Rect active_area; // all zero means the full sensor; coordinates remain sensor-relative.
    std::array<std::uint16_t, 4> black_levels{0, 0, 0, 0};
    std::array<std::uint16_t, 4> white_levels{65535, 65535, 65535, 65535};
};

class RawImage;
RAWENGINE_API std::array<std::uint8_t, 32> fingerprint_raw_source(const RawImage& image);

// Owns decoded, uncompressed uint16 Bayer samples. Construction validates and
// normalizes metadata; copies of RawImage share immutable sample storage.
class RAWENGINE_API RawImage {
public:
    RawImage(RawMetadata metadata, std::vector<std::uint16_t> samples);
    // Compatibility constructor for tightly packed, uniform-level Bayer data.
    RawImage(std::uint32_t width, std::uint32_t height,
             std::vector<std::uint16_t> samples,
             BayerPattern pattern = BayerPattern::RGGB,
             std::uint16_t black_level = 0,
             std::uint16_t white_level = 65535);
    const RawMetadata& metadata() const noexcept { return metadata_; }
    const std::vector<std::uint16_t>& samples() const noexcept { return *bayer_; }
    std::uint32_t width() const noexcept { return metadata_.width; }
    std::uint32_t height() const noexcept { return metadata_.height; }
    std::array<std::uint8_t, 32> fingerprint() const;
private:
    RawMetadata metadata_;
    std::shared_ptr<const std::vector<std::uint16_t>> bayer_;
    std::shared_ptr<std::once_flag> fingerprint_once_ = std::make_shared<std::once_flag>();
    std::shared_ptr<std::array<std::uint8_t, 32>> fingerprint_ =
        std::make_shared<std::array<std::uint8_t, 32>>();
};

// Already decoded, scene-linear, interleaved float32 RGB. Encoded raster files
// must be decoded and color-converted by the host before using this source.
struct RasterMetadata {
    std::uint32_t width = 0, height = 0;
    std::uint32_t row_stride_pixels = 0; // 0 means tightly packed width.
    WorkingSpace working_space = WorkingSpace::LinearProPhotoD50; // Serialize explicitly in future edit files.
};

class RasterImage;
RAWENGINE_API std::array<std::uint8_t, 32> fingerprint_raster_source(const RasterImage& image);

class RAWENGINE_API RasterImage {
public:
    RasterImage(RasterMetadata metadata, std::vector<float> pixels);
    const RasterMetadata& metadata() const noexcept { return metadata_; }
    const std::vector<float>& pixels() const noexcept { return *pixels_; }
    std::uint32_t width() const noexcept { return metadata_.width; }
    std::uint32_t height() const noexcept { return metadata_.height; }
    std::array<std::uint8_t, 32> fingerprint() const;
private:
    RasterMetadata metadata_;
    std::shared_ptr<const std::vector<float>> pixels_;
    std::shared_ptr<std::once_flag> fingerprint_once_ = std::make_shared<std::once_flag>();
    std::shared_ptr<std::array<std::uint8_t, 32>> fingerprint_ =
        std::make_shared<std::array<std::uint8_t, 32>>();
};

// Version 1 canonical source digests cover rendering-relevant metadata and
// visible samples in row-major order. Padding/row stride do not affect them.

// Row-major transform from white-balanced camera-linear RGB to XYZ D50 (Y=1
// for diffuse white). This is a fully calibrated transform, not an unmodified
// DNG ForwardMatrix. The host is responsible for camera/profile calibration.
struct CameraColorTransform {
    std::array<double, 9> camera_to_xyz_d50{};
    WorkingSpace target = WorkingSpace::LinearProPhotoD50; // Serialize explicitly in future edit files.
};

enum class OutputMode { LegacyBounded, SrgbPreview, IccDisplay };

// Immutable editing recipe. A new graph can be made cheaply from the same RAW.
struct GraphRecipe {
    float red_gain = 1.0f, green_gain = 1.0f, blue_gain = 1.0f;
    float exposure_stops = 0.0f;
    float tone_shoulder = 0.25f;
    float tone_gamma = 1.0f;
    std::optional<CameraColorTransform> camera_color;
    OutputMode output_mode = OutputMode::LegacyBounded;
};

// CameraNative is uncalibrated; it is not an ICC profile.
enum class PixelFormat { RGBFloat32 };
enum class PixelDomain { CameraLinearRGB, SceneLinearRGB, ToneMappedUnmanagedRGB,
                         BoundedUnmanagedRGB, DisplayLinearRGB, DisplayEncodedRGB };
enum class ColorPrimaries { CameraNative, ProPhoto, Rec2020, SRGB, ICCProfile };
enum class WhitePoint { Unspecified, D50, D65 };
enum class TransferFunction { Linear, CustomTone, SRGB, ICCProfile };
enum class ReferenceState { CameraReferred, SceneReferred, DisplayReferred, Unspecified };
enum class AlphaMode { None };

struct ImageDescriptor {
    PixelFormat format = PixelFormat::RGBFloat32;
    PixelDomain domain = PixelDomain::CameraLinearRGB;
    ColorPrimaries primaries = ColorPrimaries::CameraNative;
    WhitePoint white_point = WhitePoint::Unspecified;
    TransferFunction transfer = TransferFunction::Linear;
    ReferenceState reference = ReferenceState::CameraReferred;
    AlphaMode alpha = AlphaMode::None;
    std::array<std::uint8_t, 32> profile_sha256{}; // Zero for non-ICC domains.

    static constexpr ImageDescriptor camera_linear() noexcept { return {}; }
    static constexpr ImageDescriptor scene_linear(WorkingSpace space) noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::SceneLinearRGB,
                space == WorkingSpace::LinearProPhotoD50 ? ColorPrimaries::ProPhoto : ColorPrimaries::Rec2020,
                space == WorkingSpace::LinearProPhotoD50 ? WhitePoint::D50 : WhitePoint::D65,
                TransferFunction::Linear, ReferenceState::SceneReferred, AlphaMode::None};
    }
    static constexpr ImageDescriptor linear_srgb() noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::SceneLinearRGB,
                ColorPrimaries::SRGB, WhitePoint::D65, TransferFunction::Linear,
                ReferenceState::SceneReferred, AlphaMode::None};
    }
    static constexpr ImageDescriptor tone_mapped() noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::ToneMappedUnmanagedRGB,
                ColorPrimaries::CameraNative, WhitePoint::Unspecified,
                TransferFunction::CustomTone, ReferenceState::Unspecified, AlphaMode::None};
    }
    static constexpr ImageDescriptor bounded_output() noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::BoundedUnmanagedRGB,
                ColorPrimaries::CameraNative, WhitePoint::Unspecified,
                TransferFunction::CustomTone, ReferenceState::Unspecified, AlphaMode::None};
    }
    static constexpr ImageDescriptor srgb_output() noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::DisplayEncodedRGB,
                ColorPrimaries::SRGB, WhitePoint::D65, TransferFunction::SRGB,
                ReferenceState::DisplayReferred, AlphaMode::None};
    }
    static constexpr ImageDescriptor display_linear_srgb() noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::DisplayLinearRGB,
                ColorPrimaries::SRGB, WhitePoint::D65, TransferFunction::Linear,
                ReferenceState::DisplayReferred, AlphaMode::None};
    }
    static constexpr ImageDescriptor icc_display(
        std::array<std::uint8_t, 32> digest) noexcept {
        return {PixelFormat::RGBFloat32, PixelDomain::DisplayEncodedRGB,
                ColorPrimaries::ICCProfile, WhitePoint::Unspecified,
                TransferFunction::ICCProfile, ReferenceState::DisplayReferred,
                AlphaMode::None, digest};
    }
    bool operator==(const ImageDescriptor&) const = default;
};

struct Tile {
    Rect bounds;
    std::vector<float> rgb; // interleaved float32 RGB, row-major; 3 floats/pixel
    ImageDescriptor descriptor = ImageDescriptor::camera_linear();
};

struct IccProfileIdentity {
    std::array<std::uint8_t, 32> profile_sha256{};
    std::string intent = "relative_colorimetric";
    bool black_point_compensation = false;
    std::string engine = "lcms2-core";
    std::string engine_version = "2.19.1";
    bool operator==(const IccProfileIdentity&) const = default;
};

// Stable reconstruction identity, independent of the decoded sample fingerprint.
// Legacy manifests permanently mean this exact algorithm/version.
struct RawDemosaicIdentity {
    std::string algorithm = "rawengine.bilinear";
    std::uint32_t processing_version = 1;
    bool operator==(const RawDemosaicIdentity&) const = default;
};
RAWENGINE_API void validate_raw_demosaic(const RawDemosaicIdentity& identity);

class RAWENGINE_API Node {
public:
    virtual ~Node() = default;
    virtual Tile render(Rect bounds) const = 0;
    virtual Tile render_level(Rect bounds, RenderLevel level) const {
        if (level.mip == 0 && level.quality == RenderQuality::Final)
            return render(bounds);
        throw std::invalid_argument("node does not support this render level");
    }
    virtual bool supports_level(RenderLevel level) const noexcept {
        return level.mip == 0 && level.quality == RenderQuality::Final;
    }
    virtual ImageDescriptor output_descriptor() const noexcept = 0;
    virtual const Node* input_node() const noexcept { return nullptr; }
    // Most nodes preserve the requested level upstream. A reduction anchor
    // can request native inputs and report the corresponding native ROI.
    virtual RenderLevel input_level(RenderLevel output_level) const { return output_level; }
    // Rectangle of upstream pixels needed to produce an output rectangle.
    // Point operations use the same rectangle; spatial nodes expand it.
    virtual Rect input_region(Rect output, Rect source_bounds) const {
        (void)source_bounds;
        return output;
    }
    virtual Rect input_region_level(Rect output, Rect source_bounds,
                                    RenderLevel level) const {
        if (level.mip == 0 && level.quality == RenderQuality::Final)
            return input_region(output, source_bounds);
        throw std::invalid_argument("node has no mapping for this render level");
    }
    // Encoded ICC raster sources report the exact profile and conversion
    // policy used by their runtime node. Other nodes return no identity.
    virtual std::optional<IccProfileIdentity> input_icc_identity() const { return std::nullopt; }
    virtual std::optional<std::array<std::uint8_t, 32>> source_fingerprint() const {
        return std::nullopt;
    }
    virtual std::optional<Rect> source_bounds() const { return std::nullopt; }
    virtual std::optional<RawDemosaicIdentity> raw_demosaic_identity() const { return std::nullopt; }
};

class RAWENGINE_API RawUnpackNode final : public Node {
public:
    explicit RawUnpackNode(RawImage image);
    RawUnpackNode(RawImage image, RawDemosaicIdentity demosaic);
    Tile render(Rect bounds) const override;
    Rect input_region(Rect output, Rect source_bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
    std::optional<std::array<std::uint8_t, 32>> source_fingerprint() const override {
        return fingerprint_raw_source(image_);
    }
    std::optional<Rect> source_bounds() const override { return image_.metadata().active_area; }
    std::optional<RawDemosaicIdentity> raw_demosaic_identity() const override { return demosaic_; }
private:
    RawImage image_;
    RawDemosaicIdentity demosaic_;
};

class RAWENGINE_API RasterSourceNode final : public Node {
public:
    explicit RasterSourceNode(RasterImage image);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override {
        return (level.mip == 0 &&
                (level.quality == RenderQuality::Final ||
                 level.quality == RenderQuality::Preview)) ||
               (level.mip >= 1 && level.mip <= 2 &&
                level.quality == RenderQuality::Preview);
    }
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override {
        if (!supports_level(level))
            throw std::invalid_argument("raster source has no mapping for this render level");
        return output;
    }
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::scene_linear(image_.metadata().working_space);
    }
    std::optional<std::array<std::uint8_t, 32>> source_fingerprint() const override {
        return fingerprint_raster_source(image_);
    }
    std::optional<Rect> source_bounds() const override {
        return Rect{0, 0, image_.width(), image_.height()};
    }
private:
    RasterImage image_;
};

class RAWENGINE_API WhiteBalanceNode final : public Node {
public:
    WhiteBalanceNode(std::shared_ptr<const Node> input, float red, float green, float blue);
    Tile render(Rect bounds) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
private:
    std::shared_ptr<const Node> input_;
    float gains_[3];
};

// Reference two-input operation: (1 - amount) * base + amount * layer.
// Matching scene-linear working spaces only; no alpha, masks or clipping.
class RAWENGINE_API LinearMixNode final : public Node {
public:
    LinearMixNode(std::shared_ptr<const Node> base, std::shared_ptr<const Node> layer,
                  float amount);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    ImageDescriptor output_descriptor() const noexcept override { return base_->output_descriptor(); }
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override {
        if (!supports_level(level)) throw std::invalid_argument("linear mix has no mapping for this level");
        return output;
    }
private:
    std::shared_ptr<const Node> base_, layer_;
    float amount_;
};

class RAWENGINE_API ExposureNode final : public Node {
public:
    ExposureNode(std::shared_ptr<const Node> input, float stops);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override {
        if (!supports_level(level))
            throw std::invalid_argument("exposure has no mapping for this render level");
        return output;
    }
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override {
        return descriptor_;
    }
private:
    std::shared_ptr<const Node> input_;
    float multiplier_;
    ImageDescriptor descriptor_;
};

// Converts calibrated camera-linear RGB to a declared scene-linear working
// space. No transfer function, clipping, or display profile is applied.
// Supplying native_input_bounds enables mip-1/2 preview: average the native
// calibrated float32 pixels in direct 2x2/4x4 footprints anchored at that origin.
// White balance/demosaic and any preceding camera-linear edits run natively.
class RAWENGINE_API CameraToWorkingNode final : public Node {
public:
    CameraToWorkingNode(std::shared_ptr<const Node> input, CameraColorTransform transform,
                        Rect native_input_bounds = {});
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
private:
    std::shared_ptr<const Node> input_;
    std::array<float, 9> matrix_{};
    ImageDescriptor descriptor_;
    Rect native_input_bounds_;
};

// Explicit conversion between the two supported scene-linear working spaces.
// It preserves signed/over-range float values; output clipping is separate.
class RAWENGINE_API WorkingSpaceConvertNode final : public Node {
public:
    WorkingSpaceConvertNode(std::shared_ptr<const Node> input, WorkingSpace target);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override {
        if (!supports_level(level))
            throw std::invalid_argument("working-space conversion has no mapping for this render level");
        return output;
    }
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
private:
    std::shared_ptr<const Node> input_;
    std::array<float, 9> matrix_{};
    ImageDescriptor descriptor_;
    bool identity_ = false;
};

// Matrix-only conversion from a declared linear working space to linear sRGB.
// Negative and over-range values survive until the later output boundary.
// Mip 1/2 preview propagates upstream reduction before nonlinear output stages.
class RAWENGINE_API WorkingToSrgbNode final : public Node {
public:
    explicit WorkingToSrgbNode(std::shared_ptr<const Node> input);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override {
        if (!supports_level(level))
            throw std::invalid_argument("sRGB preview stage has no mapping for this render level");
        return output;
    }
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::linear_srgb();
    }
private:
    std::shared_ptr<const Node> input_;
    std::array<float, 9> matrix_{};
};

// Reduced preview is supported only on the explicit linear-sRGB output path.
class RAWENGINE_API ToneCurveNode final : public Node {
public:
    ToneCurveNode(std::shared_ptr<const Node> input, float shoulder, float gamma);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override {
        if (!supports_level(level))
            throw std::invalid_argument("sRGB preview stage has no mapping for this render level");
        return output;
    }
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override {
        return descriptor_;
    }
private:
    std::shared_ptr<const Node> input_;
    float shoulder_, inverse_gamma_;
    ImageDescriptor descriptor_;
};

// Native-resolution integer crop of scene-linear RGB. Output coordinates start
// at zero; pixels map by translation into the upstream crop rectangle without
// resampling. Mip 1/2 preview averages clipped native input footprints anchored
// at the crop origin, before downstream reduced operations and output encoding.
class RAWENGINE_API CropNode final : public Node {
public:
    CropNode(std::shared_ptr<const Node> input, Rect input_bounds, Rect crop);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    RenderLevel input_level(RenderLevel output_level) const override;
    Rect input_region(Rect output, Rect input_bounds) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
    Rect output_bounds() const noexcept { return {0, 0, crop_.width, crop_.height}; }
private:
    std::shared_ptr<const Node> input_;
    Rect input_bounds_, crop_;
    ImageDescriptor descriptor_;
};

enum class ResizeFilter { Nearest, Bilinear, Area };

// Exact clockwise quarter turns, followed by flips in rotated coordinates.
// Native pixels are permuted without interpolation; reduced previews average
// transformed native pixels at the output origin.
class RAWENGINE_API OrientationNode final : public Node {
public:
    OrientationNode(std::shared_ptr<const Node> input, Rect input_bounds,
                    std::uint32_t quarter_turns = 0, bool flip_horizontal = false,
                    bool flip_vertical = false);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region(Rect output, Rect input_bounds) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
    Rect output_bounds() const noexcept { return output_bounds_; }
private:
    std::pair<std::uint32_t, std::uint32_t> input_pixel(std::uint32_t x, std::uint32_t y) const;
    Rect native_output_region(Rect output, RenderLevel level) const;
    std::shared_ptr<const Node> input_;
    Rect input_bounds_, output_bounds_;
    std::uint32_t quarter_turns_;
    bool flip_horizontal_, flip_vertical_;
    ImageDescriptor descriptor_;
};

// Scene-linear reference resampler. Pixel centers map by the input/output
// extent ratio; coordinates clamp to input edge pixels. Area integrates source
// pixel cells over each output footprint. Mip 1/2 averages the native resized
// scene-linear output before downstream reduced edits.
class RAWENGINE_API ResizeNode final : public Node {
public:
    ResizeNode(std::shared_ptr<const Node> input, Rect input_bounds,
               std::uint32_t width, std::uint32_t height,
               ResizeFilter filter = ResizeFilter::Bilinear);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    RenderLevel input_level(RenderLevel level) const override;
    Rect input_region(Rect output, Rect input_bounds) const override;
    Rect input_region_level(Rect output, Rect input_bounds, RenderLevel level) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
    Rect output_bounds() const noexcept { return output_bounds_; }
private:
    std::shared_ptr<const Node> input_;
    Rect input_bounds_, output_bounds_;
    ResizeFilter filter_;
    ImageDescriptor descriptor_;
};

// Reference neighborhood operation for the Phase 2 ROI/halo contract. Uses
// a clipped 1..8 pixel box kernel in scene-linear RGB, preserving signed data.
class RAWENGINE_API BoxBlurNode final : public Node {
public:
    BoxBlurNode(std::shared_ptr<const Node> input, Rect source_bounds,
                std::uint32_t radius);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
    Rect input_region(Rect output, Rect source_bounds) const override;
    Rect input_region_level(Rect output, Rect source_bounds,
                            RenderLevel level) const override;
private:
    std::shared_ptr<const Node> input_;
    Rect source_bounds_;
    std::uint32_t radius_;
    ImageDescriptor descriptor_;
};

// The only stage that clips signed/over-range values for the current public
// render path. It does not apply a display ICC profile or a standard transfer.
class RAWENGINE_API OutputClipNode final : public Node {
public:
    explicit OutputClipNode(std::shared_ptr<const Node> input);
    Tile render(Rect bounds) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override {
        return descriptor_;
    }
private:
    std::shared_ptr<const Node> input_;
    ImageDescriptor descriptor_;
};

// Explicit sRGB preview boundary: hard-clip display-linear sRGB values, then
// apply the standard sRGB component transfer function.
class RAWENGINE_API SrgbEncodeNode final : public Node {
public:
    explicit SrgbEncodeNode(std::shared_ptr<const Node> input);
    Tile render(Rect bounds) const override;
    Tile render_level(Rect bounds, RenderLevel level) const override;
    bool supports_level(RenderLevel level) const noexcept override;
    Rect input_region_level(Rect output, Rect, RenderLevel level) const override {
        if (!supports_level(level))
            throw std::invalid_argument("sRGB preview stage has no mapping for this render level");
        return output;
    }
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::srgb_output();
    }
private:
    std::shared_ptr<const Node> input_;
};

// Backend contract for an output ICC transform from display-linear sRGB to
// bounded, profile-encoded float32 RGB. Implementations must be thread-safe
// for concurrent const calls, validate the ICC data/policy they consume, and
// supply the SHA-256 digest of the exact output profile bytes.
class RAWENGINE_API IccDisplayTransform {
public:
    virtual ~IccDisplayTransform() = default;
    virtual std::array<std::uint8_t, 32> profile_sha256() const noexcept = 0;
    virtual std::optional<IccProfileIdentity> output_icc_identity() const { return std::nullopt; }
    virtual void apply(float* interleaved_rgb, std::size_t pixels) const = 0;
};

class RAWENGINE_API IccDisplayNode final : public Node {
public:
    IccDisplayNode(std::shared_ptr<const Node> input,
                   std::shared_ptr<const IccDisplayTransform> transform);
    Tile render(Rect bounds) const override;
    const Node* input_node() const noexcept override { return input_.get(); }
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
private:
    std::shared_ptr<const Node> input_;
    std::shared_ptr<const IccDisplayTransform> transform_;
    ImageDescriptor descriptor_;
};

class RAWENGINE_API ImageGraph final {
public:
    ImageGraph(RawImage image, GraphRecipe recipe = {},
               std::shared_ptr<const IccDisplayTransform> display_transform = nullptr);
    ImageGraph(RasterImage image, GraphRecipe recipe = {},
               std::shared_ptr<const IccDisplayTransform> display_transform = nullptr);
    // Accepts any bounded, scene-linear source node, including optional
    // profile-aware raster adapters. The source must honor these bounds.
    ImageGraph(std::shared_ptr<const Node> scene_linear_source, Rect source_bounds,
               GraphRecipe recipe = {},
               std::shared_ptr<const IccDisplayTransform> display_transform = nullptr);
    // Available only for graphs constructed from RAW input.
    const RawImage& image() const;
    Rect source_bounds() const noexcept { return source_bounds_; }
    const GraphRecipe& recipe() const noexcept { return recipe_; }
    const Node& output() const noexcept { return *output_; }
private:
    std::optional<RawImage> raw_image_;
    Rect source_bounds_;
    GraphRecipe recipe_;
    std::shared_ptr<const Node> output_;
};

class ExecutableEditGraph;

class RAWENGINE_API RenderCancelled final : public std::runtime_error {
public:
    RenderCancelled() : std::runtime_error("render cancelled") {}
};

class RAWENGINE_API CancellationToken final {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    bool is_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }
private:
    std::atomic<bool> cancelled_{false};
};

class RAWENGINE_API Renderer final {
public:
    // The callback receives one temporary tile at a time. Copy data from it
    // before returning if it must outlive the callback.
    using TileCallback = std::function<void(const Tile&)>;
    void render_tiles(const ImageGraph& graph, RenderRequest request,
                      const TileCallback& callback,
                      const CancellationToken* cancellation = nullptr) const;
    void render_tiles(const ExecutableEditGraph& graph, RenderRequest request,
                      const TileCallback& callback,
                      const CancellationToken* cancellation = nullptr) const;
    void render_tiles(const Node& output, Rect source_bounds, RenderRequest request,
                      const TileCallback& callback,
                      const CancellationToken* cancellation = nullptr) const;
    void render_tiles(const ImageGraph& graph, Rect viewport,
                      const TileCallback& callback,
                      std::uint32_t tile_size = 256,
                      const CancellationToken* cancellation = nullptr) const;
    void render_tiles(const ExecutableEditGraph& graph, Rect viewport,
                      const TileCallback& callback,
                      std::uint32_t tile_size = 256,
                      const CancellationToken* cancellation = nullptr) const;
    void render_tiles(const Node& output, Rect source_bounds, Rect viewport,
                      const TileCallback& callback,
                      std::uint32_t tile_size = 256,
                      const CancellationToken* cancellation = nullptr) const;
    // Materializes only the requested viewport, retaining its descriptor.
    Tile render_image(const ImageGraph& graph, RenderRequest request,
                      const CancellationToken* cancellation = nullptr) const;
    Tile render_image(const ExecutableEditGraph& graph, RenderRequest request,
                      const CancellationToken* cancellation = nullptr) const;
    Tile render_image(const Node& output, Rect source_bounds, RenderRequest request,
                      const CancellationToken* cancellation = nullptr) const;
    Tile render_image(const ImageGraph& graph, Rect viewport,
                      std::uint32_t tile_size = 256,
                      const CancellationToken* cancellation = nullptr) const;
    Tile render_image(const ExecutableEditGraph& graph, Rect viewport,
                      std::uint32_t tile_size = 256,
                      const CancellationToken* cancellation = nullptr) const;
    Tile render_image(const Node& output, Rect source_bounds, Rect viewport,
                      std::uint32_t tile_size = 256,
                      const CancellationToken* cancellation = nullptr) const;
    // Legacy convenience API for callers that only need interleaved floats.
    std::vector<float> render_roi(const ImageGraph& graph, Rect viewport,
                                  std::uint32_t tile_size = 256,
                                  const CancellationToken* cancellation = nullptr) const;
    std::vector<float> render_roi(const ExecutableEditGraph& graph, Rect viewport,
                                  std::uint32_t tile_size = 256,
                                  const CancellationToken* cancellation = nullptr) const;
};

} // namespace rawengine
