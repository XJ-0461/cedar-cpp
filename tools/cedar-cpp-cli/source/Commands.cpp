#include <cedar-cpp-cli/Command.hpp>
#include <cedar-cpp-cli/Commands.hpp>

#include <array>

namespace cedar::cli
{

auto Commands() noexcept -> std::span<const Command* const>
{
    static constexpr std::array<const Command*, 9> commands{
        &AuthorizeCommand,       &EvaluateCommand,        &ValidateCommand,
        &CheckParseCommand,      &RunTestsCommand,        &LinkCommand,
        &TranslatePolicyCommand, &TranslateSchemaCommand, &PartialEvaluationCommand};
    return commands;
}

} // namespace cedar::cli
