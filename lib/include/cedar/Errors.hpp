#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Names.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cedar
{

/// Why evaluating an expression failed. Mirrors `EvaluationError` in
/// `cedar-policy-core/src/evaluator/err.rs:35`, with one split: Rust reports a missing entity
/// attribute and a missing entity tag as one variant (`EntityAttrDoesNotExist` with `was_attr`),
/// and here they are two kinds. Rust's `NonValue` (partial evaluation) and `ASTErrorExpr`
/// (tolerant AST) belong to features cedar-cpp does not build.
enum class EvaluationErrorKind : std::uint8_t
{
    EntityDoesNotExist,
    EntityAttributeDoesNotExist,
    RecordAttributeDoesNotExist,
    EntityTagDoesNotExist,
    ExtensionFunctionLookup,
    TypeError,
    WrongArgumentCount,
    IntegerOverflow,
    UnlinkedSlot,
    ExtensionFunctionExecution,
    RecursionLimit,
};

/// The diagnostic code an evaluation error kind reports under. The evaluation codes are numbered
/// in kind order from 7001.
[[nodiscard]] constexpr auto CodeFor(EvaluationErrorKind kind) noexcept -> DiagnosticCode
{
    return static_cast<DiagnosticCode>(7001 + static_cast<int>(kind));
}

static_assert(CodeFor(EvaluationErrorKind::EntityDoesNotExist) == DiagnosticCode::EntityDoesNotExist);
static_assert(CodeFor(EvaluationErrorKind::EntityTagDoesNotExist) == DiagnosticCode::EntityTagDoesNotExist);
static_assert(CodeFor(EvaluationErrorKind::TypeError) == DiagnosticCode::EvaluationTypeError);
static_assert(CodeFor(EvaluationErrorKind::RecursionLimit) == DiagnosticCode::RecursionLimit);

/// The diagnostic code a name or identifier check failure reports under, the codes Rust's parser
/// uses for the same conditions (`InvalidIdentifier`, `ReservedIdentifier`, `ReservedNameError`).
[[nodiscard]] constexpr auto CodeFor(const NameError error) noexcept -> DiagnosticCode
{
    switch (error)
    {
    case NameError::InvalidSyntax: return DiagnosticCode::InvalidIdentifier;
    case NameError::ReservedKeyword: return DiagnosticCode::ReservedIdentifier;
    case NameError::ReservedNamespace: return DiagnosticCode::ReservedNamespace;
    }
    return DiagnosticCode::InvalidIdentifier;
}

static_assert(CodeFor(NameError::InvalidSyntax) == DiagnosticCode::InvalidIdentifier);
static_assert(CodeFor(NameError::ReservedKeyword) == DiagnosticCode::ReservedIdentifier);
static_assert(CodeFor(NameError::ReservedNamespace) == DiagnosticCode::ReservedNamespace);

/// The human-facing part of an evaluation error, built only when the evaluator has something more
/// specific to say than the kind's fixed description.
struct EvaluationErrorDetail
{
    std::string message{};
    std::optional<std::string> help{};
};

/// One evaluation failure: its kind, the span of the expression that failed (into the source of
/// the policy being evaluated), and an optional detail. It is 32 bytes and copies by reference
/// count, so `std::expected<Value, EvaluationError>` stays small on the evaluator's return path and
/// the authorizer can record one per erroring policy cheaply.
class EvaluationError
{
public:
    /// An error described by its kind alone.
    explicit EvaluationError(EvaluationErrorKind kind, std::optional<SourceSpan> span = std::nullopt) noexcept
        : kind_{kind}, span_{span}
    {
    }

    /// An error with a specific message and optional help.
    EvaluationError(EvaluationErrorKind kind, std::optional<SourceSpan> span, std::string message,
                    std::optional<std::string> help = std::nullopt);

    [[nodiscard]] auto Kind() const noexcept -> EvaluationErrorKind { return kind_; }
    [[nodiscard]] auto Code() const noexcept -> DiagnosticCode { return CodeFor(kind_); }
    [[nodiscard]] auto Span() const noexcept -> std::optional<SourceSpan> { return span_; }

    /// The detail message, or the kind's fixed description when there is no detail.
    [[nodiscard]] auto Message() const noexcept -> std::string_view;

    /// The help text, if the detail has one.
    [[nodiscard]] auto Help() const noexcept -> std::optional<std::string_view>;

    /// Gives the error `span` when it has none. An error raised inside a callee (an extension
    /// function, say) takes the location of the call that reached it, as Rust's
    /// `with_maybe_source_loc` does.
    auto AttachSpanIfMissing(SourceSpan span) noexcept -> void
    {
        if (!span_) { span_ = span; }
    }

    /// The error as a diagnostic pointing into `source`, which should be the text of the policy
    /// whose evaluation failed (null when there is none).
    [[nodiscard]] auto ToDiagnostic(SharedSourceText source) const -> Diagnostic;

private:
    EvaluationErrorKind kind_;
    std::optional<SourceSpan> span_;
    std::shared_ptr<const EvaluationErrorDetail> detail_{};
};

/// A failed operation's report: one or more diagnostics, never zero. `Tag` makes each domain's
/// error its own type (see the aliases below), so a function's `std::expected` error type says
/// which operation failed. Callers branch on `HasCode` or on `Primary().code`.
template <typename Tag>
class DomainError
{
public:
    /// An error with exactly one diagnostic.
    explicit DomainError(Diagnostic diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }

    /// An error with `first` followed by `rest`.
    DomainError(Diagnostic first, std::vector<Diagnostic> rest) : diagnostics_{std::move(rest)}
    {
        diagnostics_.insert(diagnostics_.begin(), std::move(first));
    }

    /// The error holding `diagnostics`, or nothing when the list is empty. This is how an
    /// accumulating reader (a parser with recovery) turns what it collected into an error.
    [[nodiscard]] static auto FromDiagnostics(std::vector<Diagnostic> diagnostics) -> std::optional<DomainError>
    {
        if (diagnostics.empty()) { return std::nullopt; }
        return DomainError{std::move(diagnostics), NonEmpty{}};
    }

    /// Rewraps another domain's diagnostics, for an operation that fails because a step from
    /// another domain did (parsing a policy set from JSON fails on a policy-format error).
    template <typename OtherTag>
    explicit DomainError(DomainError<OtherTag> other)
        : diagnostics_{std::move(other).TakeDiagnostics()}
    {
    }

    /// Every diagnostic, in the order they were reported.
    [[nodiscard]] auto Diagnostics() const noexcept -> std::span<const Diagnostic> { return diagnostics_; }

    /// The first diagnostic.
    [[nodiscard]] auto Primary() const noexcept -> const Diagnostic& { return diagnostics_.front(); }

    /// Whether any diagnostic has `code`.
    [[nodiscard]] auto HasCode(DiagnosticCode code) const noexcept -> bool
    {
        for (const Diagnostic& diagnostic : diagnostics_)
        {
            if (diagnostic.code == code) { return true; }
        }
        return false;
    }

    /// Adds a further diagnostic.
    auto Append(Diagnostic diagnostic) -> void { diagnostics_.push_back(std::move(diagnostic)); }

    /// Every diagnostic rendered, as `RenderDiagnostics` does.
    [[nodiscard]] auto Render() const -> std::string { return RenderDiagnostics(diagnostics_); }

    /// Moves the diagnostics out. The error is left empty and may only be destroyed or assigned.
    [[nodiscard]] auto TakeDiagnostics() && noexcept -> std::vector<Diagnostic> { return std::move(diagnostics_); }

private:
    struct NonEmpty
    {
    };

    DomainError(std::vector<Diagnostic> diagnostics, NonEmpty) : diagnostics_{std::move(diagnostics)} {}

    std::vector<Diagnostic> diagnostics_{};
};

/// Tag types for the domain errors. They carry no data.
struct ParseErrorTag
{
};
struct PolicySetErrorTag
{
};
struct EntitiesErrorTag
{
};
struct SchemaErrorTag
{
};
struct RequestValidationErrorTag
{
};
struct ContextErrorTag
{
};
struct PartialEvaluationErrorTag
{
};
struct PartialEntitiesErrorTag
{
};
struct ReauthorizationErrorTag
{
};
struct BatchedAuthorizationErrorTag
{
};
struct PermissionQueryErrorTag
{
};

/// Policy text, an expression, a name or a policy in the JSON format could not be read.
using ParseError = DomainError<ParseErrorTag>;

/// A policy set operation failed: a duplicate id, a bad link, an unlink or removal of the wrong
/// kind of policy.
using PolicySetError = DomainError<PolicySetErrorTag>;

/// Entities could not be read, closed or checked against a schema.
using EntitiesError = DomainError<EntitiesErrorTag>;

/// A schema, in either format, could not be read or is not well formed.
using SchemaError = DomainError<SchemaErrorTag>;

/// A request does not conform to the schema it was checked against.
using RequestValidationError = DomainError<RequestValidationErrorTag>;

/// A context could not be built: it is not a record, its JSON is invalid, or a value in it failed
/// to evaluate.
using ContextError = DomainError<ContextErrorTag>;

/// Partial authorization failed for a policy set (Rust's `TpeError`): the schema is not complete
/// (`validation.incomplete-schema`), no request environment matches the partial request, a policy
/// failed to validate in it (`partial-evaluation.policy-validation`, followed by the validator's
/// diagnostics), or a typed policy could not be turned into a residual.
using PartialEvaluationError = DomainError<PartialEvaluationErrorTag>;

/// Partial entities could not be read, checked against the schema or closed (Rust's
/// `tpe::err::EntitiesError`). The concrete entity codes report the conditions concrete entities
/// share; the `partial-evaluation.*` entity codes report the rest.
using PartialEntitiesError = DomainError<PartialEntitiesErrorTag>;

/// A concrete request and entities could not complete a partial authorization (Rust's
/// `TpeReauthorizationError`): the request or the entities do not conform to the schema, or they
/// are inconsistent with the partial request and entities.
using ReauthorizationError = DomainError<ReauthorizationErrorTag>;

/// Batched authorization failed (Rust's `BatchedEvalError`): partial authorization failed, the
/// request does not conform to the schema, a loaded entity was rejected, or the iteration limit
/// was reached before a decision (`partial-evaluation.insufficient-iterations`).
using BatchedAuthorizationError = DomainError<BatchedAuthorizationErrorTag>;

/// A permission query failed (Rust's `PermissionQueryError`): the entities were rejected, or
/// partial authorization failed.
using PermissionQueryError = DomainError<PermissionQueryErrorTag>;

} // namespace cedar
