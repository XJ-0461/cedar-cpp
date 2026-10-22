#pragma once

#include "Arguments.hpp"
#include "Command.hpp"

#include <cedar/Entities.hpp>
#include <cedar/Schema.hpp>

#include <expected>
#include <optional>
#include <string_view>

namespace cedar::cli
{

// The `--schema` input of the commands that read data by a schema (`cedar-policy-cli/src/utils/schema.rs`).

/// The schema `--schema` names, read in the `--schema-format` format (Cedar by default) and
/// linked; nothing when `--schema` is not given (Rust's `OptionalSchemaArgs::get_schema`). The
/// warnings of a Cedar schema go to standard error.
[[nodiscard]] auto LoadSchema(const ParsedArguments& arguments) -> std::expected<std::optional<Schema>, Failure>;

/// Reads the entities file `path` by `schema` when there is one (`utils/entities.rs`
/// `load_entities`).
[[nodiscard]] auto LoadEntities(std::string_view path, const std::optional<Schema>& schema)
    -> std::expected<Entities, Failure>;

} // namespace cedar::cli
