#pragma once

#include <cedar/Entities.hpp>
#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>
#include <cedar/Request.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace cedar::detail
{

/// A set of policies of one indexed list, one bit per policy, by position in the list.
using PolicyBits = std::vector<std::uint64_t>;

/// An index of the scope constraints of a list of policies, for finding the policies whose scope
/// can match a request without evaluating any of them.
///
/// Each of the three scope parts is indexed by what its constraint names: `==` by uid, `is` by
/// entity type, `in` (and `is T in`) by the uid it names, and an unconstrained part matches every
/// request. A link's slots are indexed as the uids bound to them. `in` matches a request entity
/// that is the named uid or has it among its ancestors in the entity store of the call, so a query
/// looks up the request entity and each of its ancestors; an entity missing from the store is in
/// itself only (Rust `eval_in`, `cedar-policy-core/src/evaluator.rs:811`). A query gives exactly
/// the policies whose three scope parts all evaluate to true.
class ScopeIndex final
{
public:
    explicit ScopeIndex(std::span<const std::shared_ptr<const Policy>> policies);

    /// Sets `candidates` to the policies whose scope matches `request`, looking entities up in
    /// `entities`. `scratch` is working space; both are resized as needed.
    auto Candidates(const Request& request, EntityStoreView entities, PolicyBits& candidates,
                    PolicyBits& scratch) const -> void;

    /// The number of 64-bit words in a PolicyBits of this index.
    [[nodiscard]] auto WordCount() const noexcept -> std::size_t { return wordCount_; }

private:
    struct UidHash
    {
        [[nodiscard]] auto operator()(const EntityUid& uid) const noexcept -> std::size_t
        {
            return static_cast<std::size_t>(uid.Hash());
        }
    };

    /// The policies one key of a constraint index selects: their positions, or, once there are at
    /// least as many positions as a PolicyBits has words, the PolicyBits itself, so marking them
    /// costs no more than the smaller of the two and the memory stays linear in the policy count.
    class Bucket final
    {
    public:
        auto Add(std::uint32_t position) -> void { positions_.push_back(position); }
        /// Turns a large bucket into its PolicyBits; called once every position is added.
        auto Finish(std::size_t wordCount) -> void;
        auto MarkInto(PolicyBits& bits) const -> void;

    private:
        std::vector<std::uint32_t> positions_;
        PolicyBits bits_;
    };

    /// The `in` and `is in` constraints that name one uid: `in` ones, and `is T in` ones by T.
    struct InBuckets
    {
        Bucket untyped;
        std::unordered_map<EntityType, Bucket> byType;
    };

    /// The constraints of one scope part over every policy.
    class ConstraintIndex final
    {
    public:
        explicit ConstraintIndex(std::size_t wordCount) : unconstrained_(wordCount, 0) {}

        auto AddUnconstrained(std::uint32_t position) -> void;
        auto AddEqual(const EntityUid& uid, std::uint32_t position) -> void;
        auto AddIs(const EntityType& type, std::uint32_t position) -> void;
        /// `in uid`, or `is type in uid` when `type` is given.
        auto AddIn(const EntityUid& uid, const std::optional<EntityType>& type, std::uint32_t position) -> void;
        /// Called once every policy is added.
        auto Finish() -> void;

        /// Sets `bits` to the policies whose constraint on this part matches `uid`, the request's
        /// entity. The store is consulted only when some constraint on this part is an `in`.
        auto Match(const EntityUid& uid, EntityStoreView entities, PolicyBits& bits) const -> void;

    private:
        /// Marks the `in` and `is in` constraints in `buckets`, for a request entity of `type`.
        static auto MarkIn(const InBuckets& buckets, const EntityType& type, PolicyBits& bits) -> void;

        PolicyBits unconstrained_;
        std::unordered_map<EntityUid, Bucket, UidHash> equal_;
        std::unordered_map<EntityType, Bucket> is_;
        std::unordered_map<EntityUid, InBuckets, UidHash> in_;
    };

    auto AddPrincipalOrResource(ConstraintIndex& index, const PrincipalOrResourceConstraint& constraint,
                                std::uint32_t position) -> void;

    std::size_t wordCount_;
    ConstraintIndex principal_;
    ConstraintIndex action_;
    ConstraintIndex resource_;
};

/// The scope index of one list of policies, built on first use. A PolicySet holds one per state
/// of its policy list and replaces it whenever the list changes, so any number of threads reading
/// an unchanging set share one index, built once under `built`.
struct ScopeIndexCache
{
    std::once_flag built;
    std::optional<ScopeIndex> index;
};

} // namespace cedar::detail
