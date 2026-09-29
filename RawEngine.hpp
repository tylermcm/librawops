#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
private:
    RawMetadata metadata_;
    std::shared_ptr<const std::vector<std::uint16_t>> bayer_;
};

// Already decoded, scene-linear, interleaved float32 RGB. Encoded raster files
// must be decoded and color-converted by the host before using this source.
struct RasterMetadata {
    std::uint32_t width = 0, height = 0;
    std::uint32_t row_stride_pixels = 0; // 0 means tightly packed width.
    WorkingSpace working_space = WorkingSpace::LinearRec2020D65;
};

class RAWENGINE_API RasterImage {
public:
    RasterImage(RasterMetadata metadata, std::vector<float> pixels);
    const RasterMetadata& metadata() const noexcept { return metadata_; }
    const std::vector<float>& pixels() const noexcept { return *pixels_; }
    std::uint32_t width() const noexcept { return metadata_.width; }
    std::uint32_t height() const noexcept { return metadata_.height; }
private:
    RasterMetadata metadata_;
    std::shared_ptr<const std::vector<float>> pixels_;
};

// Row-major transform from white-balanced camera-linear RGB to XYZ D50 (Y=1
// for diffuse white). This is a fully calibrated transform, not an unmodified
// DNG ForwardMatrix. The host is responsible for camera/profile calibration.
struct CameraColorTransform {
    std::array<double, 9> camera_to_xyz_d50{};
    WorkingSpace target = WorkingSpace::LinearRec2020D65;
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

class RAWENGINE_API Node {
public:
    virtual ~Node() = default;
    virtual Tile render(Rect bounds) const = 0;
    virtual ImageDescriptor output_descriptor() const noexcept = 0;
};

class RAWENGINE_API RawUnpackNode final : public Node {
public:
    explicit RawUnpackNode(RawImage image);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
private:
    RawImage image_;
};

class RAWENGINE_API RasterSourceNode final : public Node {
public:
    explicit RasterSourceNode(RasterImage image);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::scene_linear(image_.metadata().working_space);
    }
private:
    RasterImage image_;
};

class RAWENGINE_API WhiteBalanceNode final : public Node {
public:
    WhiteBalanceNode(std::shared_ptr<const Node> input, float red, float green, float blue);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::camera_linear();
    }
private:
    std::shared_ptr<const Node> input_;
    float gains_[3];
};

class RAWENGINE_API ExposureNode final : public Node {
public:
    ExposureNode(std::shared_ptr<const Node> input, float stops);
    Tile render(Rect bounds) const override;
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
class RAWENGINE_API CameraToWorkingNode final : public Node {
public:
    CameraToWorkingNode(std::shared_ptr<const Node> input, CameraColorTransform transform);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override { return descriptor_; }
private:
    std::shared_ptr<const Node> input_;
    std::array<float, 9> matrix_{};
    ImageDescriptor descriptor_;
};

// Matrix-only conversion from a declared linear working space to linear sRGB.
// Negative and over-range values survive until the later output boundary.
class RAWENGINE_API WorkingToSrgbNode final : public Node {
public:
    explicit WorkingToSrgbNode(std::shared_ptr<const Node> input);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return ImageDescriptor::linear_srgb();
    }
private:
    std::shared_ptr<const Node> input_;
    std::array<float, 9> matrix_{};
};

class RAWENGINE_API ToneCurveNode final : public Node {
public:
    ToneCurveNode(std::shared_ptr<const Node> input, float shoulder, float gamma);
    Tile render(Rect bounds) const override;
    ImageDescriptor output_descriptor() const noexcept override {
        return descriptor_;
    }
private:
    std::shared_ptr<const Node> input_;
    float shoulder_, inverse_gamma_;
    ImageDescriptor descriptor_;
};

// The only stage that clips signed/over-range values for the current public
// render path. It does not apply a display ICC profile or a standard transfer.
class RAWENGINE_API OutputClipNode final : public Node {
public:
    explicit OutputClipNode(std::shared_ptr<const Node> input);
    Tile render(Rect bounds) const override;
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
    virtual void apply(float* interleaved_rgb, std::size_t pixels) const = 0;
};

class RAWENGINE_API IccDisplayNode final : public Node {
public:
    IccDisplayNode(std::shared_ptr<const Node> input,
                   std::shared_ptr<const IccDisplayTransform> transform);
    Tile render(Rect bounds) const override;
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

class RAWENGINE_API Renderer final {
public:
    // The callback receives one temporary tile at a time. Copy data from it
    // before returning if it must outlive the callback.
    using TileCallback = std::function<void(const Tile&)>;
    void render_tiles(const ImageGraph& graph, Rect viewport,
                      const TileCallback& callback,
                      std::uint32_t tile_size = 256) const;
    // Materializes only the requested viewport, retaining its descriptor.
    Tile render_image(const ImageGraph& graph, Rect viewport,
                      std::uint32_t tile_size = 256) const;
    // Legacy convenience API for callers that only need interleaved floats.
    std::vector<float> render_roi(const ImageGraph& graph, Rect viewport,
                                  std::uint32_t tile_size = 256) const;
};

} // namespace rawengine
