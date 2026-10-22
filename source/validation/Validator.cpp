#include <cedar/Validator.hpp>

#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Rbac.hpp"
#include "validation/LevelValidation.hpp"
#include "validation/RequestEnvironments.hpp"
#include "validation/StringChecks.hpp"
#include "validation/Typechecker.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace cedar
{

namespace
{

// Rust collects the typechecker's errors per policy into a set; two diagnostics are the same
// error when code, message, help and span are.
[[nodiscard]] auto SameDiagnostic(const Diagnostic& left, const Diagnostic& right) noexcept -> bool
{
    return left.code == right.code && left.primary == right.primary && left.message == right.message &&
           left.help == right.help;
}

auto AddUnique(std::vector<Diagnostic>& diagnostics, Diagnostic diagnostic) -> void
{
    const bool seen = std::ranges::any_of(
        diagnostics, [&](const Diagnostic& existing) { return SameDiagnostic(existing, diagnostic); });
    if (!seen) { diagnostics.push_back(std::move(diagnostic)); }
}

struct TypecheckFindings
{
    std::vector<Diagnostic> errors;
    std::optional<Diagnostic> impossible;
    std::vector<Diagnostic> levelErrors{};
};

// Rust's `Typechecker::typecheck_policy`: the body in every linked request environment. The policy
// is impossible when no environment gives it a type other than False, including when the schema
// has no environments at all. With a level, each environment whose check did not fail is also
// level-checked (Rust's `validate_policy_with_level`).
[[nodiscard]] auto TypecheckPolicy(const detail::ValidatorSchema& schema, const Template& body,
                                   const ValidationMode mode, const std::optional<std::uint32_t> level)
    -> TypecheckFindings
{
    TypecheckFindings findings;
    const detail::Typechecker typechecker{schema, mode, body};
    bool allFalse = true;
    for (const detail::TypecheckEnvironment& environment : detail::LinkedEnvironments(schema, body))
    {
        std::optional<detail::NodeTypes> nodeTypes;
        if (level) { nodeTypes.emplace(); }
        detail::TypecheckResult result =
            typechecker.Check(body.Condition(), environment, nodeTypes ? &*nodeTypes : nullptr);
        if (level && result.outcome != detail::PolicyCheckOutcome::Fail)
        {
            detail::CheckDereferenceLevels(schema, body, environment, *nodeTypes, *level, findings.levelErrors);
        }
        if (result.outcome != detail::PolicyCheckOutcome::Irrelevant) { allFalse = false; }
        for (Diagnostic& error : result.errors) { AddUnique(findings.errors, std::move(error)); }
    }
    if (allFalse)
    {
        findings.impossible = Diagnostic{
            .code = DiagnosticCode::ImpossiblePolicy,
            .severity = Severity::Warning,
            .message = "policy is impossible: the policy expression evaluates to false for all valid requests",
            .help = std::nullopt,
            .primary = body.SourceExtent(),
            .labels = {},
            .source = body.Source(),
        };
    }
    return findings;
}

// Rust's `PolicySet::all_templates`: every template and the body of every static policy.
[[nodiscard]] auto AllBodies(const PolicySet& policies) -> std::vector<const Template*>
{
    return policies.AllTemplates();
}

auto Append(std::vector<ValidationDiagnostic>& to, const PolicyId& id, Diagnostic diagnostic) -> void
{
    to.push_back(ValidationDiagnostic{id, std::move(diagnostic)});
}

} // namespace

namespace
{

// Rust's `validate`, and with a level `validate_with_level`: the same checks, each body's level
// errors after its other errors.
[[nodiscard]] auto ValidatePolicies(const Schema& validatorSchema, const PolicySet& policies, const ValidationMode mode,
                                    const std::optional<std::uint32_t> level) -> ValidationResult
{
    const detail::ValidatorSchema& schema = detail::SchemaAccess::Validator(validatorSchema);
    const std::vector<const Template*> bodies = AllBodies(policies);
    ValidationResult result;

    if (!schema.IsComplete())
    {
        for (const Template* body : bodies)
        {
            if (auto error = detail::IncompleteSchemaError(schema, *body)) { Append(result.errors, body->Id(), *error); }
        }
        return result;
    }

    // Rust's `validate_policy` per body: the scope and literal checks, then the typechecker; the
    // action application warning comes before the impossible-policy warning.
    for (const Template* body : bodies)
    {
        for (Diagnostic& error : detail::ScopeAndLiteralErrors(schema, *body))
        {
            Append(result.errors, body->Id(), std::move(error));
        }
        if (auto warning = detail::ActionApplicationWarning(schema, body->Principal(), body->Action(),
                                                            body->Resource(), *body))
        {
            Append(result.warnings, body->Id(), std::move(*warning));
        }
        TypecheckFindings findings = TypecheckPolicy(schema, *body, mode, level);
        for (Diagnostic& error : findings.errors) { Append(result.errors, body->Id(), std::move(error)); }
        for (Diagnostic& error : findings.levelErrors) { Append(result.errors, body->Id(), std::move(error)); }
        if (findings.impossible) { Append(result.warnings, body->Id(), std::move(*findings.impossible)); }
    }

    // Rust's `validate_slots` per template-linked policy: the linked entities' types, and action
    // application with the slots replaced by them. The link itself is not typechecked again.
    for (const std::shared_ptr<const Policy>& policy : policies.Policies())
    {
        if (policy->IsStatic()) { continue; }
        for (Diagnostic& error : detail::SlotEntityTypeErrors(schema, *policy))
        {
            Append(result.errors, policy->Id(), std::move(error));
        }
        if (auto warning = detail::ActionApplicationWarning(schema, policy->Principal(), policy->Action(),
                                                            policy->Resource(), policy->GetTemplate()))
        {
            Append(result.warnings, policy->Id(), std::move(*warning));
        }
    }

    // Rust's `confusable_string_checks` over every body, after the link warnings.
    for (const Template* body : bodies)
    {
        for (Diagnostic& warning : detail::StringCheckWarnings(*body))
        {
            Append(result.warnings, body->Id(), std::move(warning));
        }
    }
    return result;
}

} // namespace

auto Validator::Validate(const PolicySet& policies, const ValidationMode mode) const -> ValidationResult
{
    return ValidatePolicies(schema_, policies, mode, std::nullopt);
}

auto Validator::ValidateWithLevel(const PolicySet& policies, const ValidationMode mode,
                                  const std::uint32_t maximumDereferenceLevel) const -> ValidationResult
{
    return ValidatePolicies(schema_, policies, mode, maximumDereferenceLevel);
}

auto ValidRequestEnvironments(const Template& body, const Schema& schema) -> std::vector<RequestEnvironment>
{
    // Rust's `get_valid_request_envs`: strict mode, the environments where the body typechecks
    // with a type other than False, ordered and each once.
    const detail::ValidatorSchema& validatorSchema = detail::SchemaAccess::Validator(schema);
    const detail::Typechecker typechecker{validatorSchema, ValidationMode::Strict, body};
    std::vector<RequestEnvironment> environments;
    for (const detail::TypecheckEnvironment& environment : detail::LinkedEnvironments(validatorSchema, body))
    {
        if (typechecker.Check(body.Condition(), environment).outcome != detail::PolicyCheckOutcome::Success)
        {
            continue;
        }
        environments.push_back(RequestEnvironment{environment.principal, environment.action->name,
                                                  environment.resource, environment.principalSlot,
                                                  environment.resourceSlot});
    }
    const auto key = [](const RequestEnvironment& environment) {
        return std::tie(environment.principal, environment.action, environment.resource, environment.principalSlot,
                        environment.resourceSlot);
    };
    std::ranges::sort(environments, [&](const RequestEnvironment& left, const RequestEnvironment& right) {
        return key(left) < key(right);
    });
    const auto duplicates = std::ranges::unique(environments, [&](const RequestEnvironment& left,
                                                                  const RequestEnvironment& right) {
        return key(left) == key(right);
    });
    environments.erase(duplicates.begin(), duplicates.end());
    return environments;
}

} // namespace cedar
