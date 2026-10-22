#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Schema.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace cedar
{

/// How strictly the validator types policies (Rust's `ValidationMode`).
enum class ValidationMode : std::uint8_t
{
    /// Policies must type-check and keep to a restricted form that is amenable to analysis: no
    /// comparison of differently typed values, no empty set literals, extension constructors on
    /// literals only.
    Strict,
    /// Policies must type-check.
    Permissive,
};

/// One validation finding about one policy. `diagnostic.code` is a validation code (one per Rust
/// `ValidationError` or `ValidationWarning` kind), its severity is Error or Warning accordingly, and
/// its spans point into the policy's source text.
struct ValidationDiagnostic
{
    PolicyId policyId;
    Diagnostic diagnostic;
};

/// What validating a policy set found (Rust's `ValidationResult`). Errors and warnings are each in
/// policy set order. A policy whose id has no error passed.
struct ValidationResult
{
    std::vector<ValidationDiagnostic> errors{};
    std::vector<ValidationDiagnostic> warnings{};

    /// No errors; warnings are allowed.
    [[nodiscard]] auto Passed() const noexcept -> bool { return errors.empty(); }

    /// Neither errors nor warnings.
    [[nodiscard]] auto PassedWithoutWarnings() const noexcept -> bool { return errors.empty() && warnings.empty(); }
};

/// Checks policies against a schema (Rust's `Validator`): entity types and actions are declared,
/// every action is applied to principal and resource types it allows, and every expression
/// type-checks. Validating against a schema that is not complete (Schema::IsComplete) always fails.
class Validator final
{
public:
    explicit Validator(Schema schema) noexcept : schema_{std::move(schema)} {}

    [[nodiscard]] auto GetSchema() const noexcept -> const Schema& { return schema_; }

    /// Validates every policy and template in `policies`.
    [[nodiscard]] auto Validate(const PolicySet& policies, ValidationMode mode = ValidationMode::Strict) const
        -> ValidationResult;

    /// Validates as Validate does and also checks, in every request environment where a policy
    /// type-checks, that it reads no entity data more than `maximumDereferenceLevel` entity
    /// dereferences away from the request (RFC 76, Rust's `validate_with_level`). Attribute
    /// access, `has`, `in`, `hasTag` and `getTag` on an entity each dereference it; the request
    /// variables are at level 0, and an entity literal other than the request's action may not be
    /// dereferenced at all. A violation is a `validation.entity-dereference-level-violation`
    /// error.
    [[nodiscard]] auto ValidateWithLevel(const PolicySet& policies, ValidationMode mode,
                                         std::uint32_t maximumDereferenceLevel) const -> ValidationResult;

private:
    Schema schema_;
};

/// The request environments of `schema` in which `body` type-checks (Rust's
/// `Template::get_valid_request_envs`); for a template, with the slot types it may be linked to.
[[nodiscard]] auto ValidRequestEnvironments(const Template& body, const Schema& schema)
    -> std::vector<RequestEnvironment>;

} // namespace cedar
