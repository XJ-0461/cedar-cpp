#pragma once

#include <cedar/Expression.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cedar::detail
{

// What the typechecker knows to be present at a point of a policy (Rust's "capabilities",
// `cedar-policy-core/src/validator/types/capability.rs`): after `e has a`, attribute `a` of `e`
// may be read; after `e.hasTag(t)`, tag `t` of `e`. A capability names its expressions by shape,
// so two occurrences of `principal.manager` are the same expression wherever they are written.

/// Structural hashes and equality of the expressions of one arena, ignoring spans (Rust's
/// `ExprShapeOnly`). A folded subtree is compared as written, as Rust has no folding.
class ExpressionShapes final
{
public:
    /// Hashes every node of `arena`, which must outlive this.
    explicit ExpressionShapes(const ExpressionArena& arena);

    [[nodiscard]] auto Arena() const noexcept -> const ExpressionArena& { return arena_; }

    /// `index`, or for a folded node the subtree it was written as.
    [[nodiscard]] auto Resolve(ExpressionIndex index) const noexcept -> ExpressionIndex;

    [[nodiscard]] auto Hash(const ExpressionIndex index) const noexcept -> std::uint64_t
    {
        return hashes_[std::to_underlying(index)];
    }

    /// The hash of `base.path[0].path[1]...`: equal to the hash of a written attribute chain.
    [[nodiscard]] auto Hash(ExpressionIndex base, std::span<const std::string_view> path) const noexcept
        -> std::uint64_t;

    /// Whether the two subtrees have the same shape.
    [[nodiscard]] auto Equal(ExpressionIndex left, ExpressionIndex right) const noexcept -> bool;

    /// Whether `leftBase.leftPath...` and `rightBase.rightPath...` have the same shape.
    [[nodiscard]] auto Equal(ExpressionIndex leftBase, std::span<const std::string_view> leftPath,
                             ExpressionIndex rightBase, std::span<const std::string_view> rightPath) const noexcept
        -> bool;

private:
    const ExpressionArena& arena_;
    std::vector<std::uint64_t> hashes_;
};

enum class CapabilityKind : std::uint8_t
{
    Attribute,
    Tag,
};

/// Attribute `attribute`, or tag `tag`, of `object.objectPath...` is known to be present. The
/// object path is the attribute chain a `has` with a path (`e has a.b`) tests one step at a time.
struct Capability
{
    CapabilityKind kind;
    ExpressionIndex object;
    std::span<const std::string_view> objectPath;
    std::string_view attribute;
    ExpressionIndex tag;
    std::uint64_t hash;

    [[nodiscard]] static auto ForAttribute(const ExpressionShapes& shapes, ExpressionIndex object,
                                           std::span<const std::string_view> objectPath, std::string_view attribute)
        -> Capability;

    [[nodiscard]] static auto ForTag(const ExpressionShapes& shapes, ExpressionIndex object, ExpressionIndex tag)
        -> Capability;
};

/// A set of capabilities (Rust's `CapabilitySet`). Small in practice; kept as a vector.
class CapabilitySet final
{
public:
    CapabilitySet() = default;

    [[nodiscard]] static auto Singleton(const Capability& capability) -> CapabilitySet;

    [[nodiscard]] auto Union(const CapabilitySet& other, const ExpressionShapes& shapes) const -> CapabilitySet;
    [[nodiscard]] auto Intersect(const CapabilitySet& other, const ExpressionShapes& shapes) const -> CapabilitySet;
    [[nodiscard]] auto Contains(const Capability& capability, const ExpressionShapes& shapes) const noexcept -> bool;

    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return capabilities_.empty(); }

private:
    std::vector<Capability> capabilities_;
};

} // namespace cedar::detail
