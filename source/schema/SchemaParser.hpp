#pragma once

#include "schema/SchemaFragmentModel.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Schema.hpp>

#include <cstddef>
#include <expected>
#include <memory>

namespace cedar::detail
{

/// The deepest a type may nest in Cedar schema syntax (`Set<...>` and record types, each one
/// level). Rust has no fixed limit; this one is far above anything written by hand.
inline constexpr std::size_t MaximumSchemaTypeDepth = 1000;

/// Reads a schema fragment in Cedar schema syntax (Rust's `json_schema::Fragment::from_cedarschema_str`,
/// `cedar-policy-core/src/validator/cedar_schema/{grammar.lalrpop,to_json_schema.rs}`), then makes
/// the checks `api::SchemaFragment::from_cedarschema_str` adds (CheckFragmentRestrictions). The
/// model's spans point into `source`. Every syntax error is fatal, as in Rust; conversion errors
/// (duplicate declarations, reserved names, a malformed `appliesTo`) are reported together.
[[nodiscard]] auto ParseCedarSchema(SharedSourceText source)
    -> std::expected<WithWarnings<std::shared_ptr<const SchemaFragmentModel>>, SchemaError>;

} // namespace cedar::detail
