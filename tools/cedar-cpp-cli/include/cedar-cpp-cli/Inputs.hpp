#pragma once

#include <cedar-cpp-cli/Arguments.hpp>
#include <cedar-cpp-cli/Command.hpp>

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/PolicySet.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace cedar::cli
{

// Options and readers shared by the commands that take policies (`cedar-policy-cli/src/utils/`).

inline constexpr OptionSpecification PoliciesOption{
    .longName = "policies", .shortName = 'p', .valueName = "FILE",
    .help = "File containing static Cedar policies and/or templates"};
inline constexpr OptionSpecification PolicyFormatOption{
    .longName = "policy-format", .valueName = "POLICY_FORMAT",
    .help = "Format of policies in the --policies file [possible values: cedar, json]"};
inline constexpr OptionSpecification TemplateLinkedOption{
    .longName = "template-linked", .shortName = 'k', .valueName = "FILE",
    .help = "File containing template-linked policies"};
inline constexpr OptionSpecification SchemaOption{
    .longName = "schema", .shortName = 's', .valueName = "FILE", .help = "File containing the schema"};
inline constexpr OptionSpecification SchemaFormatOption{
    .longName = "schema-format", .valueName = "SCHEMA_FORMAT",
    .help = "Schema format [possible values: cedar, json]"};

/// The authorizer options of the commands that authorize: `CEDAR_CPP_POLICY_SELECTION=every`
/// selects PolicySelection::EvaluateEveryPolicy, and `index` or no such variable ScopeIndex. It is
/// an environment variable, not a flag, so the differential tests can give both CLIs one argument
/// list. Fails for any other value.
[[nodiscard]] auto AuthorizerOptionsFromEnvironment() -> std::expected<AuthorizerOptions, UsageError>;

/// Fails, as clap does, for a `--schema-format` other than `cedar` or `json`.
[[nodiscard]] auto CheckSchemaFormat(const ParsedArguments& arguments) -> std::expected<void, UsageError>;

/// Reads the whole file `path`, or standard input when `path` is nothing. Fails, as Rust's
/// `read_to_string` does, when the file cannot be read or is not UTF-8. `context` names the input
/// in the message ("policy set", "entities").
[[nodiscard]] auto ReadInput(std::optional<std::string_view> path, std::string_view context)
    -> std::expected<std::string, Failure>;

enum class PolicyFormat
{
    Cedar,
    Json,
};

/// The policy input options of one command line (Rust's `PoliciesArgs`). A missing
/// `policiesFile` means standard input.
struct PolicyInput
{
    std::optional<std::string_view> policiesFile;
    PolicyFormat format = PolicyFormat::Cedar;
    std::optional<std::string_view> templateLinkedFile;
    /// A `templateLinkedFile` that does not exist holds no links (Rust's
    /// `get_policy_set_allow_missing_links`, for `link`).
    bool allowMissingTemplateLinkedFile = false;
};

/// The policy input options in `arguments`. Fails for a `--policy-format` other than `cedar` or
/// `json`.
[[nodiscard]] auto ReadPolicyInput(const ParsedArguments& arguments) -> std::expected<PolicyInput, UsageError>;

/// Reads the policy set (Rust's `PoliciesArgs::get_policy_set`). Cedar text is parsed, then each
/// template and policy that has an `@id` annotation is renamed to that annotation's value,
/// templates first (`utils/policies.rs` `rename_from_id_annotation`); ids that clash after renaming,
/// with each other or with an unrenamed `policyN`, fail as Rust's do. The JSON policy format is read
/// as `read_json_policy_set` reads it, without renaming. Then the links of the `--template-linked`
/// file are added in order (`utils/links.rs`); a link Rust refuses fails. Every uid in that file is
/// read in normalized form (ParseNormalizedEntityUid).
[[nodiscard]] auto LoadPolicySet(const PolicyInput& input) -> std::expected<PolicySet, Failure>;

/// Reads the entities file `path` without a schema (`utils/entities.rs` `load_entities`).
[[nodiscard]] auto LoadEntities(std::string_view path) -> std::expected<Entities, Failure>;

} // namespace cedar::cli
