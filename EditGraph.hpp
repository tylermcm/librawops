#pragma once

#include "RawEngine.hpp"

#include <compare>
#include <map>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>

namespace rawengine {

// JSON-compatible typed parameter tree. Object keys are ordered so the edit
// manifest has one deterministic byte representation for a given state.
struct EditValue {
    using Array = std::vector<EditValue>;
    using Object = std::map<std::string, EditValue>;
    std::variant<std::nullptr_t, bool, std::int64_t, double,
                 std::string, Array, Object> data = nullptr;
    bool operator==(const EditValue&) const = default;
};

enum class EditSourceKind { DecodedBayerU16, SceneLinearRasterF32, IccRasterU16 };
enum class EditDomain { CameraLinear, SceneLinearProPhotoD50,
                        SceneLinearRec2020D65, SceneLinearSrgb,
                        ToneMappedUnmanaged, DisplayLinearSrgb,
                        DisplayEncodedSrgb, DisplayEncodedIcc, UnmanagedBounded };
enum class LegacyRecipeEra { ImplicitRec2020, ImplicitProPhoto };
inline constexpr std::uint32_t kLegacyRec2020ProcessingVersion = 1;
inline constexpr std::uint32_t kCurrentEditProcessingVersion = 2;

struct EditSource {
    std::string id; // Lowercase UUID; stable across revisions.
    EditSourceKind kind = EditSourceKind::SceneLinearRasterF32;
    std::optional<WorkingSpace> working_space; // Required for raster sources, absent for Bayer.
    // Canonical v1 source fingerprint, verified against built-in source nodes.
    std::array<std::uint8_t, 32> content_sha256{};
    std::optional<IccProfileIdentity> icc_input;
    bool operator==(const EditSource&) const = default;
};

struct EditOperation {
    std::string id; // Lowercase UUID; never reused for a different operation.
    std::string type_id; // Stable identifier, e.g. rawengine.exposure.
    std::uint32_t schema_version = 1;
    std::uint32_t processing_version = 1;
    bool enabled = true;
    EditDomain input_domain = EditDomain::SceneLinearProPhotoD50;
    EditDomain output_domain = EditDomain::SceneLinearProPhotoD50;
    EditValue::Object parameters;
    std::map<std::string, std::string> inputs; // Named edges to source/operation IDs.
    std::map<std::string, std::string> masks;
    std::string blend_mode = "normal";
    double opacity = 1.0;
    EditValue::Object extra_fields; // Unknown operation fields survive replay/save.
    bool operator==(const EditOperation&) const = default;
};

struct EditManifest {
    std::uint32_t format_version = 2;
    std::uint32_t processing_version = kCurrentEditProcessingVersion;
    WorkingSpace working_space = WorkingSpace::LinearProPhotoD50;
    std::vector<EditSource> sources;
    std::vector<EditOperation> operations;
    std::string output_id;
    std::optional<IccProfileIdentity> output_profile;
    EditValue::Object extra_fields;
    bool operator==(const EditManifest&) const = default;
};

// Format v2 requires source working spaces. The v1 reader migrates raster
// sources to the document's explicit working space; no unknown color default
// is inferred. Unknown operations and extra fields survive parse/save.
RAWENGINE_API void validate_edit_manifest(const EditManifest& manifest);
RAWENGINE_API std::string serialize_edit_manifest(const EditManifest& manifest);
RAWENGINE_API EditManifest parse_edit_manifest(std::string_view json);

// Old GraphRecipe had no file format or processing version. A host importing
// an in-memory legacy recipe must explicitly declare which implicit default
// it came from; no guess is made from current C++ defaults.
RAWENGINE_API EditManifest snapshot_legacy_recipe(
    EditSource source, GraphRecipe recipe, LegacyRecipeEra era,
    std::string operation_id,
    std::optional<IccProfileIdentity> output_profile = std::nullopt);

struct BoundEditSource {
    EditSource identity; // Must exactly match the saved source record and runtime content.
    std::shared_ptr<const Node> node;
    Rect bounds;
};

// Process-local LRU cache. Share one instance across graph revisions to reuse
// unchanged upstream tiles. The byte budget includes a fixed per-entry charge;
// oversized tiles render normally without entering the cache.
class RAWENGINE_API TileCache final {
public:
    struct Stats {
        std::size_t entries = 0, used_bytes = 0, hits = 0, misses = 0;
    };
    explicit TileCache(std::size_t max_bytes);
    Tile render(const Node& node, std::array<std::uint8_t, 32> signature, Rect bounds);
    Stats stats() const;
    void clear();
private:
    struct Key {
        std::array<std::uint8_t, 32> signature{};
        std::array<std::uint32_t, 4> bounds{};
        auto operator<=>(const Key&) const = default;
    };
    struct Entry {
        Tile tile;
        std::list<Key>::iterator recency;
        std::size_t charged_bytes = 0;
    };
    std::size_t max_bytes_ = 0, used_bytes_ = 0, hits_ = 0, misses_ = 0;
    std::uint64_t generation_ = 0;
    std::map<Key, Entry> entries_;
    std::list<Key> recency_;
    mutable std::mutex mutex_;
};

// Immutable executable view of a format-v2 manifest. Only registered core
// point/color operations execute; unknown operations remain serializable but
// fail closed at execution. Source buffers and ICC transform are owned by the
// caller's shared nodes/transform and retained by this graph.
class RAWENGINE_API ExecutableEditGraph final {
public:
    ExecutableEditGraph(EditManifest manifest, std::vector<BoundEditSource> sources,
                        std::shared_ptr<const IccDisplayTransform> display_transform = nullptr,
                        std::shared_ptr<TileCache> cache = nullptr);
    const Node& output() const noexcept { return *output_; }
    std::shared_ptr<const Node> output_handle() const noexcept { return output_; }
    Rect source_bounds() const noexcept { return bounds_; }
    const EditManifest& manifest() const noexcept { return manifest_; }
private:
    EditManifest manifest_;
    Rect bounds_;
    std::shared_ptr<const Node> output_;
};

} // namespace rawengine
