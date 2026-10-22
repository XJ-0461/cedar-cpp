#include <cedar/Errors.hpp>

#include <utility>

namespace cedar
{

namespace
{

// The fixed description used when an error carries no detail of its own.
[[nodiscard]] auto DescribeKind(EvaluationErrorKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
    case EvaluationErrorKind::EntityDoesNotExist: return "entity does not exist";
    case EvaluationErrorKind::EntityAttributeDoesNotExist: return "entity does not have the attribute";
    case EvaluationErrorKind::RecordAttributeDoesNotExist: return "record does not have the attribute";
    case EvaluationErrorKind::EntityTagDoesNotExist: return "entity does not have the tag";
    case EvaluationErrorKind::ExtensionFunctionLookup: return "extension function does not exist";
    case EvaluationErrorKind::TypeError: return "operand has the wrong type";
    case EvaluationErrorKind::WrongArgumentCount: return "wrong number of arguments";
    case EvaluationErrorKind::IntegerOverflow: return "integer overflow";
    case EvaluationErrorKind::UnlinkedSlot: return "template slot is not linked";
    case EvaluationErrorKind::ExtensionFunctionExecution: return "extension function failed";
    case EvaluationErrorKind::RecursionLimit: return "expression is nested too deeply to evaluate";
    }
    return "evaluation failed";
}

} // namespace

static_assert(sizeof(EvaluationError) <= 32, "EvaluationError is returned on the evaluator's hot path");

EvaluationError::EvaluationError(EvaluationErrorKind kind, std::optional<SourceSpan> span, std::string message,
                                 std::optional<std::string> help)
    : kind_{kind}, span_{span},
      detail_{std::make_shared<const EvaluationErrorDetail>(
          EvaluationErrorDetail{.message = std::move(message), .help = std::move(help)})}
{
}

auto EvaluationError::Message() const noexcept -> std::string_view
{
    return detail_ ? std::string_view{detail_->message} : DescribeKind(kind_);
}

auto EvaluationError::Help() const noexcept -> std::optional<std::string_view>
{
    if (!detail_ || !detail_->help) { return std::nullopt; }
    return std::string_view{*detail_->help};
}

auto EvaluationError::ToDiagnostic(SharedSourceText source) const -> Diagnostic
{
    Diagnostic diagnostic{
        .code = Code(),
        .severity = Severity::Error,
        .message = std::string{Message()},
        .help = std::nullopt,
        .primary = source ? span_ : std::nullopt,
        .labels = {},
        .source = std::move(source),
    };
    if (const auto help = Help()) { diagnostic.help = std::string{*help}; }
    return diagnostic;
}

} // namespace cedar
