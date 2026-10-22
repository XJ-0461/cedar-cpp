#include <cedar-cpp-cli/Command.hpp>

#include <cstdio>
#include <format>

namespace cedar::cli
{

namespace
{

auto AppendJsonString(std::string& out, const std::string_view text) -> void
{
    out += '"';
    for (const char character : text)
    {
        switch (character)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20)
            {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(character)));
            }
            else { out += character; }
        }
    }
    out += '"';
}

/// One JSON object per failure on one line: `{"message", "severity", "causes": [{"code",
/// "message", "source", "offset", "length"}]}`, the source fields only when the cause has them.
auto ToJsonLine(const Failure& failure) -> std::string
{
    std::string out = "{\"message\":";
    AppendJsonString(out, failure.message);
    out += ",\"severity\":\"error\",\"causes\":[";
    bool first = true;
    for (const Diagnostic& cause : failure.causes)
    {
        if (!first) { out += ','; }
        first = false;
        out += "{\"code\":";
        AppendJsonString(out, CodeName(cause.code));
        out += ",\"message\":";
        AppendJsonString(out, cause.message);
        if (cause.source)
        {
            out += ",\"source\":";
            AppendJsonString(out, cause.source->name);
        }
        if (cause.primary)
        {
            out += std::format(",\"offset\":{},\"length\":{}", cause.primary->begin, cause.primary->Length());
        }
        out += '}';
    }
    out += "]}\n";
    return out;
}

} // namespace

auto Report(const ErrorFormat format, const Failure& failure) -> void
{
    std::string text;
    if (format == ErrorFormat::Json) { text = ToJsonLine(failure); }
    else
    {
        text = std::format("error: {}\n", failure.message);
        if (!failure.causes.empty()) { text += RenderDiagnostics(failure.causes); }
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
}

} // namespace cedar::cli
