#pragma once

#include "RawEngine.hpp"

#include <compare>
#include <map>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
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
    // The caller supplies a node that renders bounds in the stated level's
    // coordinate system. Distinct mip/quality values never share an entry.
    Tile render(const Node& node, std::array<std::uint8_t, 32> signature, Rect bounds,
                RenderLevel level = {});
    Stats stats() const;
    void clear();
private:
    struct Key {
        std::array<std::uint8_t, 32> signature{};
        std::array<std::uint32_t, 4> bounds{};
        std::uint32_t mip = 0;
        RenderQuality quality = RenderQuality::Final;
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
// point/color/spatial operations execute; unknown operations remain serializable but
// fail closed at execution. Source buffers and ICC transform are owned by the
// caller's shared nodes/transform and retained by this graph.
class RAWENGINE_API ExecutableEditGraph final {
public:
    ExecutableEditGraph(EditManifest manifest, std::vector<BoundEditSource> sources,
                        std::shared_ptr<const IccDisplayTransform> display_transform = nullptr,
                        std::shared_ptr<TileCache> cache = nullptr);
    const Node& output() const noexcept { return *output_; }
    std::shared_ptr<const Node> output_handle() const noexcept { return output_; }
    // Singular helpers reject outputs depending on multiple source IDs.
    Rect source_bounds() const;
    // Rendering/scheduling use the output extent, which transforms may rebase.
    Rect output_bounds() const noexcept { return output_bounds_; }
    Rect required_source_region(Rect output) const;
    Rect required_source_region(Rect output, RenderLevel level) const;
    std::map<std::string, Rect> required_source_regions(Rect output, RenderLevel level = {}) const;
    const EditManifest& manifest() const noexcept { return manifest_; }
private:
    EditManifest manifest_;
    Rect output_bounds_;
    std::map<const Node*, Rect> input_bounds_;
    std::map<const Node*, std::vector<std::pair<const Node*, Rect>>> branch_inputs_;
    std::map<const Node*, std::vector<std::pair<std::string, Rect>>> source_nodes_;
    std::shared_ptr<const Node> output_;
};

// Published snapshots never change and remain usable after navigation/eviction.
// Their graphs own pinned source handles; history stores edit states, not images.
struct EditRevision {
    std::uint64_t id = 0;
    std::string manifest_json;
    std::shared_ptr<const ExecutableEditGraph> graph;
};

class EditRevisionUnavailable : public std::out_of_range {
public:
    using std::out_of_range::out_of_range;
};

class RAWENGINE_API EditHistory final {
public:
    struct Limits {
        std::size_t max_revisions = 64;
        std::size_t max_manifest_bytes = 4 * 1024 * 1024;
    };
    struct Stats {
        std::uint64_t current_id = 0;
        std::vector<std::uint64_t> revision_ids;
        std::size_t manifest_bytes = 0;
        bool can_undo = false, can_redo = false;
        Limits limits;
    };
    using Snapshot = std::shared_ptr<const EditRevision>;
    EditHistory(EditManifest initial, std::vector<BoundEditSource> sources, Limits limits,
                std::shared_ptr<const IccDisplayTransform> transform = nullptr,
                std::shared_ptr<TileCache> cache = nullptr);
    Snapshot current() const;
    Snapshot revision(std::uint64_t id) const;
    std::pair<Snapshot, Snapshot> comparison(std::uint64_t first, std::uint64_t second) const;
    // Validates before publication; commits discard redo and evict oldest states
    // to meet both limits. IDs increase monotonically and are never reused.
    std::uint64_t commit(EditManifest manifest);
    std::uint64_t undo();
    std::uint64_t redo();
    Stats stats() const;
    std::string serialize() const;
    static std::unique_ptr<EditHistory> restore(
        std::string_view json, std::vector<BoundEditSource> sources,
        std::shared_ptr<const IccDisplayTransform> transform = nullptr,
        std::shared_ptr<TileCache> cache = nullptr);
    const std::vector<BoundEditSource>& source_bindings() const noexcept { return sources_; }
private:
    Snapshot prepare(EditManifest manifest, std::uint64_t id) const;
    std::vector<BoundEditSource> sources_;
    std::vector<EditSource> identities_;
    std::shared_ptr<const IccDisplayTransform> transform_;
    std::shared_ptr<TileCache> cache_;
    Limits limits_;
    std::vector<Snapshot> revisions_;
    std::size_t cursor_ = 0, manifest_bytes_ = 0;
    std::uint64_t next_id_ = 1;
    mutable std::mutex mutex_;
};

} // namespace rawengine
