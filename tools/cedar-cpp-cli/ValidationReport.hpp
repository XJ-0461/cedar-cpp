#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Validator.hpp>

#include <vector>

namespace cedar::cli
{

/// The errors and then the warnings of `result`, each message led by the policy it is about
/// (`for policy `policy0`, ...`), as the Rust CLI prints them.
[[nodiscard]] auto ValidationDiagnostics(const ValidationResult& result) -> std::vector<Diagnostic>;

} // namespace cedar::cli
