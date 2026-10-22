#pragma once

#include <cedar/Errors.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cedar
{

// Cedar policy text in and out. Every reader takes the text first and, last, the name the input
// goes by in diagnostics. A reader keeps its own copy of the text: diagnostics point into it, and a
// parsed policy keeps it so it can be printed exactly as written (Template::OriginalText).

// ---- Reading ----

/// Reads one expression (Rust's `Expression::from_str`).
[[nodiscard]] auto ParseExpression(std::string_view text, std::string_view sourceName = "<expression>")
    -> std::expected<Expression, ParseError>;

/// Reads an entity uid literal such as `Photos::User::"alice"` (Rust's `EntityUid::from_str`).
/// Whitespace and comments are allowed between the tokens, as in policy text.
[[nodiscard]] auto ParseEntityUid(std::string_view text, std::string_view sourceName = "<entity uid>")
    -> std::expected<EntityUid, ParseError>;

/// Reads one static policy (Rust's `Policy::parse`). Without `id` the policy is `policy0`. Fails
/// with `parse.expected-static-policy` for a template.
[[nodiscard]] auto ParsePolicy(std::string_view text, std::optional<PolicyId> id = std::nullopt,
                               std::string_view sourceName = "<policy>") -> std::expected<Policy, ParseError>;

/// Reads one template (Rust's `Template::parse`). Without `id` the template is `policy0`. Fails with
/// `parse.expected-template` for a policy without slots.
[[nodiscard]] auto ParseTemplate(std::string_view text, std::optional<PolicyId> id = std::nullopt,
                                 std::string_view sourceName = "<template>")
    -> std::expected<std::shared_ptr<const Template>, ParseError>;

/// Reads a sequence of static policies and templates (Rust's `PolicySet::from_str`). Each is
/// named by its position, `policy0`, `policy1`, and so on, templates and policies counted together.
/// Reading continues past an error to the next `;`, so one call reports every error in the text.
[[nodiscard]] auto ParsePolicySet(std::string_view text, std::string_view sourceName = "<policies>")
    -> std::expected<PolicySet, ParseError>;

// ---- Printing ----

/// The expression in canonical Cedar syntax.
[[nodiscard]] auto ToCedar(const Expression& expression) -> std::string;

/// The template in canonical Cedar syntax, with its slots as `?principal` and `?resource`.
[[nodiscard]] auto ToCedar(const Template& body) -> std::string;

/// The policy in canonical Cedar syntax. A template-linked policy prints as its template with each
/// slot replaced by the entity bound to it.
[[nodiscard]] auto ToCedar(const Policy& policy) -> std::string;

/// Every static and template-linked policy, then every template, each group sorted by id, in
/// canonical Cedar syntax (the order of Rust's `PolicySet::to_cedar`, `cedar-policy/src/api.rs:2984`).
/// Ids are not part of Cedar text, so reading the output back assigns new ones.
[[nodiscard]] auto ToCedar(const PolicySet& policies) -> std::string;

} // namespace cedar
