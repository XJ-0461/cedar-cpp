#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Policy.hpp>

#include <vector>

namespace cedar::detail
{

/// The string and identifier warnings for one template or static policy body (Rust's
/// `confusable_string_checks` in `validator/str_checks.rs`), in the order Rust's expression
/// iterator meets them over the combined condition, scope included.
///
/// Each string literal and `like` pattern (as Rust prints it) gives at most one warning:
/// `validation.bidirectional-characters-in-string` when it holds a bidirectional formatting
/// character, else `validation.mixed-script-string` when its characters share no script. Each
/// identifier (entity type path components, entity ids, extension function names, attribute
/// names, `has` paths and record keys) gives at most one of, in this order of precedence:
/// `validation.bidirectional-characters-in-identifier`, `validation.confusable-identifier` (a
/// character other than space that is neither printable ASCII nor allowed by the General
/// Security Profile), `validation.mixed-script-identifier`.
[[nodiscard]] auto StringCheckWarnings(const Template& body) -> std::vector<Diagnostic>;

} // namespace cedar::detail
