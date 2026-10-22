#include "Arguments.hpp"

#include <algorithm>
#include <format>

namespace cedar::cli
{

auto ParsedArguments::Has(const std::string_view longName) const noexcept -> bool
{
    return Value(longName).has_value();
}

auto ParsedArguments::Value(const std::string_view longName) const noexcept -> std::optional<std::string_view>
{
    for (const auto& [name, value] : entries_)
    {
        if (name == longName) { return value; }
    }
    return std::nullopt;
}

auto ParsedArguments::Set(const std::string_view longName, const std::string_view value) -> void
{
    for (auto& [name, existing] : entries_)
    {
        if (name == longName)
        {
            existing = value;
            return;
        }
    }
    entries_.emplace_back(longName, value);
}

namespace
{

auto Describe(const OptionSpecification& option) -> std::string
{
    std::string text = std::format("--{}", option.longName);
    if (option.takesValue) { text += std::format(" <{}>", option.valueName.empty() ? "VALUE" : option.valueName); }
    return text;
}

auto FindLong(const std::span<const OptionSpecification> options, const std::string_view name)
    -> const OptionSpecification*
{
    const auto found = std::ranges::find(options, name, &OptionSpecification::longName);
    return found == options.end() ? nullptr : &*found;
}

auto FindShort(const std::span<const OptionSpecification> options, const char name) -> const OptionSpecification*
{
    const auto found = std::ranges::find(options, name, &OptionSpecification::shortName);
    return found == options.end() ? nullptr : &*found;
}

} // namespace

auto ParseArguments(const std::span<const std::string_view> arguments,
                    const std::span<const OptionSpecification> options, const PositionalArguments positional)
    -> std::expected<ParseResult, UsageError>
{
    ParseResult result;
    std::size_t index = 0;
    bool optionsEnded = false;

    // Records `option`, taking its value from `attached` or else from the next argument.
    const auto take = [&](const OptionSpecification& option, const std::string_view argument,
                          const std::optional<std::string_view> attached) -> std::expected<void, UsageError> {
        if (!option.takesValue && attached)
        {
            return std::unexpected{UsageError{std::format("unexpected argument '{}' found", argument)}};
        }
        if (result.options.Has(option.longName))
        {
            return std::unexpected{
                UsageError{std::format("the argument '{}' cannot be used multiple times", Describe(option))}};
        }
        std::string_view value{};
        if (option.takesValue)
        {
            if (attached) { value = *attached; }
            else if (index < arguments.size() && (arguments[index] == "-" || !arguments[index].starts_with('-')))
            {
                value = arguments[index];
                ++index;
            }
            else
            {
                return std::unexpected{
                    UsageError{std::format("a value is required for '{}' but none was supplied", Describe(option))}};
            }
        }
        result.options.Set(option.longName, value);
        return {};
    };

    while (index < arguments.size())
    {
        const std::string_view argument = arguments[index];
        const bool isOption = !optionsEnded && argument.size() > 1 && argument.front() == '-';
        if (!isOption)
        {
            if (positional == PositionalArguments::Stop)
            {
                result.rest = arguments.subspan(index);
                return result;
            }
            if (positional == PositionalArguments::Collect)
            {
                result.positionals.push_back(argument);
                ++index;
                continue;
            }
            return std::unexpected{UsageError{std::format("unexpected argument '{}' found", argument)}};
        }
        if (argument == "--")
        {
            optionsEnded = true;
            ++index;
            continue;
        }
        ++index;

        if (argument.starts_with("--"))
        {
            std::string_view name = argument.substr(2);
            std::optional<std::string_view> attached;
            if (const auto equals = name.find('='); equals != std::string_view::npos)
            {
                attached = name.substr(equals + 1);
                name = name.substr(0, equals);
            }
            const OptionSpecification* option = FindLong(options, name);
            if (option == nullptr)
            {
                return std::unexpected{UsageError{std::format("unexpected argument '{}' found", argument)}};
            }
            if (auto taken = take(*option, argument, attached); !taken) { return std::unexpected{taken.error()}; }
            continue;
        }

        // A group of short options: flags, then at most one option that takes the rest as its value.
        std::size_t position = 1;
        while (position < argument.size())
        {
            const OptionSpecification* option = FindShort(options, argument[position]);
            if (option == nullptr)
            {
                return std::unexpected{UsageError{std::format("unexpected argument '{}' found", argument)}};
            }
            ++position;
            std::optional<std::string_view> attached;
            if (position < argument.size() && (option->takesValue || argument[position] == '='))
            {
                std::string_view tail = argument.substr(position);
                if (tail.front() == '=') { tail.remove_prefix(1); }
                attached = tail;
                position = argument.size();
            }
            if (auto taken = take(*option, argument, attached); !taken) { return std::unexpected{taken.error()}; }
        }
    }
    return result;
}

auto DescribeOptions(const std::span<const OptionSpecification> options) -> std::string
{
    std::string text;
    for (const OptionSpecification& option : options)
    {
        const std::string shortPart = option.shortName == '\0' ? "    " : std::format("-{}, ", option.shortName);
        text += std::format("  {}{:<32} {}\n", shortPart, Describe(option), option.help);
    }
    return text;
}

} // namespace cedar::cli
