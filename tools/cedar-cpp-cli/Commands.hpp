#pragma once

#include "Command.hpp"

namespace cedar::cli
{

// Each command is defined in its own source file and listed in Commands.cpp.

/// `authorize`: answers a request against policies and entities (`command/authorize.rs`).
extern const Command AuthorizeCommand;

/// `evaluate`: evaluates an expression in a request's environment (`command/evaluate.rs`).
extern const Command EvaluateCommand;

/// `check-parse`: whether policies, an expression and entities parse (`command/check_parse.rs`).
extern const Command CheckParseCommand;

/// `run-tests`: checks requests against their expected answers (`command/run_test.rs`).
extern const Command RunTestsCommand;

/// `translate-policy`: Cedar policy text to the JSON policy format and back
/// (`command/translate_policy.rs`).
extern const Command TranslatePolicyCommand;

/// `link`: links a template and records the link in a template-linked file (`command/link.rs`).
extern const Command LinkCommand;

/// `validate`: validates policies against a schema (`command/validate.rs`).
extern const Command ValidateCommand;

/// `translate-schema`: converts a schema between the Cedar and JSON formats (`command/translate_schema.rs`).
extern const Command TranslateSchemaCommand;

} // namespace cedar::cli
