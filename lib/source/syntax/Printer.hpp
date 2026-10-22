#pragma once

#include <cedar/PolicySet.hpp>

#include <cstdint>
#include <string>

namespace cedar::detail
{

/// Whether a parsed policy prints as the text it was read from or in canonical form.
enum class TextChoice : std::uint8_t
{
    Retained,  ///< The original text when the policy has one (what ToCedar prints).
    Canonical, ///< Always the canonical form, as Rust prints its AST.
};

/// The policy set as ToCedar(PolicySet) prints it, with the text of each policy chosen by `choice`.
/// The canonical form is what the parity checks compare with the reference implementation.
[[nodiscard]] auto PrintPolicySet(const PolicySet& policies, TextChoice choice) -> std::string;

} // namespace cedar::detail
