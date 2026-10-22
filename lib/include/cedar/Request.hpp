#pragma once

#include <cedar/Errors.hpp>
#include <cedar/Names.hpp>
#include <cedar/Value.hpp>

#include <expected>
#include <memory>
#include <span>
#include <utility>

namespace cedar
{

class Schema;

/// A request's context: always a record (Rust's `Context::Value`). It owns the storage its values
/// live in, and copies share it.
class Context final
{
public:
    /// The empty context.
    Context() noexcept;

    /// The context `value`, which must be a record (`request.context-not-a-record` otherwise).
    [[nodiscard]] static auto FromValue(OwnedValue value) -> std::expected<Context, ContextError>;

    /// Adopts `record`, whose keys and values live in `storage` (or are inline in the Value).
    /// `storage` may be null when the record points nowhere.
    Context(Record record, std::shared_ptr<const ValueArena> storage) noexcept;

    /// A context of `pairs`, deep-copied into storage the context owns. Fails, naming the key, when a
    /// key appears twice (`parse.duplicate-record-key`, as Rust's `Context::from_pairs` reports it).
    [[nodiscard]] static auto FromPairs(std::span<const RecordEntry> pairs) -> std::expected<Context, ContextError>;

    /// The context record. The view is valid while this context or a copy of it lives, so it is
    /// not available from a temporary.
    [[nodiscard]] auto GetRecord() const& noexcept -> Record { return value_.Get().AsRecord(); }
    auto GetRecord() const&& noexcept -> Record = delete;

    /// The context as a record Value, which is what the `context` variable evaluates to. Valid as
    /// GetRecord's view is.
    [[nodiscard]] auto AsValue() const& noexcept -> const Value& { return value_.Get(); }
    auto AsValue() const&& noexcept -> const Value& = delete;

    /// Checks the context against the context type `schema` declares for `action` (Rust's
    /// `Context::validate`). Fails with `request.undeclared-action` when the schema does not declare
    /// `action` and with `request.invalid-context` when the context does not have the declared type.
    [[nodiscard]] auto Validate(const Schema& schema, const EntityUid& action) const
        -> std::expected<void, RequestValidationError>;

private:
    explicit Context(OwnedValue value) noexcept : value_{std::move(value)} {}

    OwnedValue value_;
};

/// An authorization request: principal, action and resource uids, and the context. Values the
/// evaluator makes from it (`principal`, `context`) are views into the request, so the request must
/// outlive the evaluation and must not be moved during it.
class Request final
{
public:
    Request(EntityUid principal, EntityUid action, EntityUid resource, Context context = {}) noexcept
        : principal_{std::move(principal)}, action_{std::move(action)}, resource_{std::move(resource)},
          context_{std::move(context)}
    {
    }

    /// The request checked against `schema` (Rust's `Request::new` with a schema): the action must be
    /// declared, the principal and resource types must be ones it applies to, and the context must
    /// have the type it declares (`request.*` codes).
    [[nodiscard]] static auto Create(EntityUid principal, EntityUid action, EntityUid resource, Context context,
                                     const Schema& schema) -> std::expected<Request, RequestValidationError>;

    [[nodiscard]] auto Principal() const noexcept -> const EntityUid& { return principal_; }
    [[nodiscard]] auto Action() const noexcept -> const EntityUid& { return action_; }
    [[nodiscard]] auto Resource() const noexcept -> const EntityUid& { return resource_; }
    [[nodiscard]] auto GetContext() const noexcept -> const Context& { return context_; }

private:
    EntityUid principal_;
    EntityUid action_;
    EntityUid resource_;
    Context context_;
};

} // namespace cedar
