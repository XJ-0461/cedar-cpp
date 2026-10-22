#pragma once

#include "json/ValueJson.hpp"

#include <cedar/Errors.hpp>
#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>

#include <expected>
#include <memory>
#include <string_view>

namespace cedar::detail
{

// The JSON policy format (Rust's EST, `cedar-policy-core/src/est/`), read as serde reads it. The
// public readers in Json.hpp fix how a repeated key reads to match the Rust function each mirrors;
// these take it as a parameter, because the Rust CLI reads every JSON policy file into a
// `serde_json::Value` first (`cedar-policy-cli/src/utils/policies.rs`), so a repeated key keeps its
// last value there, where Rust's `PolicySet::from_json_str` refuses one.

/// Reads one policy or template (Rust's `est::Policy` converted by
/// `try_into_ast_policy_or_template`): static when it has no slots, a template otherwise.
/// `duplicates` is LastWins or Reject.
[[nodiscard]] auto ReadPolicyOrTemplateJson(std::string_view json, DuplicateKeys duplicates, PolicyId id,
                                            std::string_view sourceName)
    -> std::expected<std::shared_ptr<const Template>, ParseError>;

/// Reads a policy set (Rust's `est::PolicySet` converted to `ast::PolicySet`, then wrapped as
/// `PolicySet::from_est` wraps it, which hides a template without slots that nothing links).
/// `duplicates` is LastWins or Reject.
[[nodiscard]] auto ReadPolicySetJson(std::string_view json, DuplicateKeys duplicates, std::string_view sourceName)
    -> std::expected<PolicySet, PolicySetError>;

} // namespace cedar::detail
