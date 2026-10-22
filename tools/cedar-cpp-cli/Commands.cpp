#include "Command.hpp"
#include "Commands.hpp"

#include <array>

namespace cedar::cli
{

auto Commands() noexcept -> std::span<const Command* const>
{
    static constexpr std::array<const Command*, 8> commands{&AuthorizeCommand,       &EvaluateCommand,
                                                             &ValidateCommand,        &CheckParseCommand,
                                                             &RunTestsCommand,        &LinkCommand,
                                                             &TranslatePolicyCommand, &TranslateSchemaCommand};
    return commands;
}

} // namespace cedar::cli
