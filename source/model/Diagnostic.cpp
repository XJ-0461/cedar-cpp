#include <cedar/Diagnostic.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <utility>

namespace cedar
{

namespace
{

[[nodiscard]] auto IsContinuationByte(char byte) noexcept -> bool
{
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

[[nodiscard]] auto ClampOffset(std::string_view text, std::uint32_t offset) noexcept -> std::size_t
{
    return std::min<std::size_t>(offset, text.size());
}

// The byte range of the line holding byte `offset`, without its `\n` or a trailing `\r`.
struct LineRange
{
    std::size_t begin{};
    std::size_t end{};
};

[[nodiscard]] auto LineContaining(std::string_view text, std::size_t offset) noexcept -> LineRange
{
    const std::size_t previous_newline = offset == 0 ? std::string_view::npos : text.rfind('\n', offset - 1);
    const std::size_t begin = previous_newline == std::string_view::npos ? 0 : previous_newline + 1;
    std::size_t end = text.find('\n', offset);
    if (end == std::string_view::npos) { end = text.size(); }
    if (end > begin && text[end - 1] == '\r') { --end; }
    return LineRange{.begin = begin, .end = end};
}

// The quoted source line under a location, and a caret rule under the span. The caret rule copies
// tabs from the line so it stays aligned however the reader's terminal expands them.
auto AppendExcerpt(std::string& out, std::string_view text, SourceSpan span) -> void
{
    const std::size_t begin = ClampOffset(text, span.begin);
    const std::size_t end = std::max(begin, ClampOffset(text, span.end));
    const LineRange line = LineContaining(text, begin);

    out += "  ";
    out += text.substr(line.begin, line.end - line.begin);
    out += "\n  ";

    for (std::size_t index = line.begin; index < begin && index < line.end; ++index)
    {
        const char byte = text[index];
        if (byte == '\t') { out += '\t'; }
        else if (!IsContinuationByte(byte)) { out += ' '; }
    }

    std::size_t caret_count = 0;
    for (std::size_t index = begin; index < std::min(end, line.end); ++index)
    {
        if (!IsContinuationByte(text[index])) { ++caret_count; }
    }
    out.append(std::max<std::size_t>(caret_count, 1), '^');
    out += '\n';
}

auto AppendLocationPrefix(std::string& out, const SourceText& source, std::optional<SourceSpan> span) -> void
{
    if (!span)
    {
        std::format_to(std::back_inserter(out), "{}: ", source.name);
        return;
    }
    const SourceLocation location = LocateOffset(source.text, span->begin);
    std::format_to(std::back_inserter(out), "{}:{}:{}: ", source.name, location.line, location.column);
}

} // namespace

auto MakeSourceText(std::string name, std::string text) -> SharedSourceText
{
    return std::make_shared<const SourceText>(SourceText{.name = std::move(name), .text = std::move(text)});
}

auto LocateOffset(std::string_view text, std::uint32_t offset) -> SourceLocation
{
    const std::size_t clamped = ClampOffset(text, offset);
    const std::string_view before = text.substr(0, clamped);

    const auto line = static_cast<std::uint32_t>(std::ranges::count(before, '\n') + 1);
    const std::size_t last_newline = before.rfind('\n');
    const std::string_view line_prefix =
        last_newline == std::string_view::npos ? before : before.substr(last_newline + 1);
    const auto column = static_cast<std::uint32_t>(
        std::ranges::count_if(line_prefix, [](char byte) { return !IsContinuationByte(byte); }) + 1);

    return SourceLocation{.line = line, .column = column};
}

auto SeverityName(Severity severity) noexcept -> std::string_view
{
    switch (severity)
    {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Info: return "info";
    }
    return "error";
}

auto CodeName(DiagnosticCode code) noexcept -> std::string_view
{
    switch (code)
    {
    case DiagnosticCode::InvalidToken: return "parse.invalid-token";
    case DiagnosticCode::UnexpectedToken: return "parse.unexpected-token";
    case DiagnosticCode::UnexpectedEndOfInput: return "parse.unexpected-end-of-input";
    case DiagnosticCode::ExtraToken: return "parse.extra-token";
    case DiagnosticCode::NestingTooDeep: return "parse.nesting-too-deep";
    case DiagnosticCode::DuplicateTemplateId: return "parse.duplicate-template-id";
    case DiagnosticCode::DuplicatePolicyId: return "parse.duplicate-policy-id";
    case DiagnosticCode::ExpectedStaticPolicy: return "parse.expected-static-policy";
    case DiagnosticCode::ExpectedTemplate: return "parse.expected-template";
    case DiagnosticCode::DuplicateAnnotation: return "parse.duplicate-annotation";
    case DiagnosticCode::SlotsInConditionClause: return "parse.slots-in-condition-clause";
    case DiagnosticCode::MissingScopeVariable: return "parse.missing-scope-variable";
    case DiagnosticCode::ExtraScopeElement: return "parse.extra-scope-element";
    case DiagnosticCode::ReservedIdentifier: return "parse.reserved-identifier";
    case DiagnosticCode::InvalidIdentifier: return "parse.invalid-identifier";
    case DiagnosticCode::InvalidSingleEquals: return "parse.invalid-single-equals";
    case DiagnosticCode::InvalidEffect: return "parse.invalid-effect";
    case DiagnosticCode::InvalidCondition: return "parse.invalid-condition";
    case DiagnosticCode::InvalidScopeVariable: return "parse.invalid-scope-variable";
    case DiagnosticCode::IncorrectVariable: return "parse.incorrect-variable";
    case DiagnosticCode::InvalidScopeOperator: return "parse.invalid-scope-operator";
    case DiagnosticCode::InvalidActionScopeOperator: return "parse.invalid-action-scope-operator";
    case DiagnosticCode::IsInActionScope: return "parse.is-in-action-scope";
    case DiagnosticCode::IsWithEquals: return "parse.is-with-equals";
    case DiagnosticCode::InvalidActionType: return "parse.invalid-action-type";
    case DiagnosticCode::EmptyClause: return "parse.empty-clause";
    case DiagnosticCode::InvalidString: return "parse.invalid-string";
    case DiagnosticCode::ArbitraryVariable: return "parse.arbitrary-variable";
    case DiagnosticCode::InvalidAttribute: return "parse.invalid-attribute";
    case DiagnosticCode::InvalidHasRightHandSide: return "parse.invalid-has-right-hand-side";
    case DiagnosticCode::PathAsAttribute: return "parse.path-as-attribute";
    case DiagnosticCode::FunctionCallOnMethod: return "parse.function-call-on-method";
    case DiagnosticCode::MethodCallOnFunction: return "parse.method-call-on-function";
    case DiagnosticCode::InvalidPattern: return "parse.invalid-pattern";
    case DiagnosticCode::InvalidIsType: return "parse.invalid-is-type";
    case DiagnosticCode::UnexpectedConstruct: return "parse.unexpected-construct";
    case DiagnosticCode::AmbiguousOperators: return "parse.ambiguous-operators";
    case DiagnosticCode::UnsupportedDivision: return "parse.unsupported-division";
    case DiagnosticCode::UnsupportedModulo: return "parse.unsupported-modulo";
    case DiagnosticCode::DuplicateRecordKey: return "parse.duplicate-record-key";
    case DiagnosticCode::IntegerLiteralTooLarge: return "parse.integer-literal-too-large";
    case DiagnosticCode::UnaryOperatorLimit: return "parse.unary-operator-limit";
    case DiagnosticCode::VariableCall: return "parse.variable-call";
    case DiagnosticCode::NoMethods: return "parse.no-methods";
    case DiagnosticCode::UnknownMethod: return "parse.unknown-method";
    case DiagnosticCode::UnknownFunction: return "parse.unknown-function";
    case DiagnosticCode::InvalidEntityLiteral: return "parse.invalid-entity-literal";
    case DiagnosticCode::ExpressionCall: return "parse.expression-call";
    case DiagnosticCode::InvalidAccess: return "parse.invalid-access";
    case DiagnosticCode::InvalidIndex: return "parse.invalid-index";
    case DiagnosticCode::NonStringIndex: return "parse.non-string-index";
    case DiagnosticCode::TypeConstraints: return "parse.type-constraints";
    case DiagnosticCode::NonNormalizedString: return "parse.non-normalized-string";
    case DiagnosticCode::WrongArity: return "parse.wrong-arity";
    case DiagnosticCode::InvalidEscape: return "parse.invalid-escape";
    case DiagnosticCode::WrongEntityArgument: return "parse.wrong-entity-argument";
    case DiagnosticCode::InvalidSlot: return "parse.invalid-slot";
    case DiagnosticCode::ReservedNamespace: return "parse.reserved-namespace";
    case DiagnosticCode::InvertedIsIn: return "parse.inverted-is-in";
    case DiagnosticCode::JsonSyntax: return "parse.json-syntax";
    case DiagnosticCode::JsonUnexpectedShape: return "parse.json-unexpected-shape";
    case DiagnosticCode::JsonInvalidSlotName: return "parse.json-invalid-slot-name";
    case DiagnosticCode::JsonActionSlot: return "parse.json-action-slot";
    case DiagnosticCode::JsonMissingOperator: return "parse.json-missing-operator";
    case DiagnosticCode::JsonMultipleOperators: return "parse.json-multiple-operators";
    case DiagnosticCode::PolicyIdAlreadyDefined: return "policy-set.policy-id-already-defined";
    case DiagnosticCode::LinkWrongSlots: return "policy-set.link-wrong-slots";
    case DiagnosticCode::LinkNoSuchTemplate: return "policy-set.link-no-such-template";
    case DiagnosticCode::LinkPolicyIdConflict: return "policy-set.link-policy-id-conflict";
    case DiagnosticCode::ExpectedStaticPolicyId: return "policy-set.expected-static-policy-id";
    case DiagnosticCode::ExpectedTemplateId: return "policy-set.expected-template-id";
    case DiagnosticCode::PolicyNonexistent: return "policy-set.policy-nonexistent";
    case DiagnosticCode::TemplateNonexistent: return "policy-set.template-nonexistent";
    case DiagnosticCode::RemoveTemplateWithActiveLinks: return "policy-set.remove-template-with-active-links";
    case DiagnosticCode::RemoveTemplateNotTemplate: return "policy-set.remove-template-not-template";
    case DiagnosticCode::LinkNonexistent: return "policy-set.link-nonexistent";
    case DiagnosticCode::UnlinkLinkNotLink: return "policy-set.unlink-link-not-link";
    case DiagnosticCode::DuplicateEntity: return "entities.duplicate-entity";
    case DiagnosticCode::TransitiveClosureMissingEdge: return "entities.transitive-closure-missing-edge";
    case DiagnosticCode::TransitiveClosureCycle: return "entities.transitive-closure-cycle";
    case DiagnosticCode::SelfAncestor: return "entities.self-ancestor";
    case DiagnosticCode::DuplicateAncestor: return "entities.duplicate-ancestor";
    case DiagnosticCode::ActionParentIsNotAction: return "entities.action-parent-is-not-action";
    case DiagnosticCode::InvalidEscapeValue: return "entities.invalid-escape-value";
    case DiagnosticCode::InvalidRestrictedExpression: return "entities.invalid-restricted-expression";
    case DiagnosticCode::ExpectedLiteralEntityReference: return "entities.expected-literal-entity-reference";
    case DiagnosticCode::ExpectedExtensionValue: return "entities.expected-extension-value";
    case DiagnosticCode::MissingImpliedConstructor: return "entities.missing-implied-constructor";
    case DiagnosticCode::IncorrectArgumentCount: return "entities.incorrect-argument-count";
    case DiagnosticCode::ExtensionFunctionLookup: return "entities.extension-function-lookup";
    case DiagnosticCode::DuplicateKey: return "entities.duplicate-key";
    case DiagnosticCode::EntityAttributeEvaluation: return "entities.entity-attribute-evaluation";
    case DiagnosticCode::UnexpectedRecordAttribute: return "entities.unexpected-record-attribute";
    case DiagnosticCode::MissingRequiredRecordAttribute: return "entities.missing-required-record-attribute";
    case DiagnosticCode::TypeMismatch: return "entities.type-mismatch";
    case DiagnosticCode::ExpressionTag: return "entities.expression-tag";
    case DiagnosticCode::NullValue: return "entities.null-value";
    case DiagnosticCode::ReservedName: return "entities.reserved-name";
    case DiagnosticCode::UnsupportedEntityTags: return "entities.unsupported-entity-tags";
    case DiagnosticCode::UnexpectedEntityAttribute: return "entities.unexpected-entity-attribute";
    case DiagnosticCode::UnexpectedEntityTag: return "entities.unexpected-entity-tag";
    case DiagnosticCode::MissingRequiredEntityAttribute: return "entities.missing-required-entity-attribute";
    case DiagnosticCode::InvalidAncestorType: return "entities.invalid-ancestor-type";
    case DiagnosticCode::UnexpectedEntityType: return "entities.unexpected-entity-type";
    case DiagnosticCode::ExternalEntityType: return "entities.external-entity-type";
    case DiagnosticCode::UndeclaredAction: return "entities.undeclared-action";
    case DiagnosticCode::ActionDeclarationMismatch: return "entities.action-declaration-mismatch";
    case DiagnosticCode::InvalidEnumEntity: return "entities.invalid-enum-entity";
    case DiagnosticCode::SerializationReservedKey: return "entities.serialization-reserved-key";
    case DiagnosticCode::SerializationExtensionCallArity: return "entities.serialization-extension-call-arity";
    case DiagnosticCode::ActionTransitiveClosure: return "schema.action-transitive-closure";
    case DiagnosticCode::EntityTypeTransitiveClosure: return "schema.entity-type-transitive-closure";
    case DiagnosticCode::UnsupportedSchemaFeature: return "schema.unsupported-schema-feature";
    case DiagnosticCode::UndeclaredEntityTypes: return "schema.undeclared-entity-types";
    case DiagnosticCode::UndeclaredActionDescendants: return "schema.undeclared-action-descendants";
    case DiagnosticCode::TypeNotDefined: return "schema.type-not-defined";
    case DiagnosticCode::ActionNotDefined: return "schema.action-not-defined";
    case DiagnosticCode::TypeShadowing: return "schema.type-shadowing";
    case DiagnosticCode::ActionShadowing: return "schema.action-shadowing";
    case DiagnosticCode::DuplicateEntityType: return "schema.duplicate-entity-type";
    case DiagnosticCode::DuplicateAction: return "schema.duplicate-action";
    case DiagnosticCode::DuplicateCommonType: return "schema.duplicate-common-type";
    case DiagnosticCode::CycleInActionHierarchy: return "schema.cycle-in-action-hierarchy";
    case DiagnosticCode::CycleInCommonTypeReferences: return "schema.cycle-in-common-type-references";
    case DiagnosticCode::ActionEntityTypeDeclared: return "schema.action-entity-type-declared";
    case DiagnosticCode::ContextOrShapeNotRecord: return "schema.context-or-shape-not-record";
    case DiagnosticCode::ActionAttributesContainEmptySet: return "schema.action-attributes-contain-empty-set";
    case DiagnosticCode::UnsupportedActionAttribute: return "schema.unsupported-action-attribute";
    case DiagnosticCode::ActionAttributeEvaluation: return "schema.action-attribute-evaluation";
    case DiagnosticCode::ExpressionEscapeUsed: return "schema.expression-escape-used";
    case DiagnosticCode::UnknownExtensionType: return "schema.unknown-extension-type";
    case DiagnosticCode::SchemaInvalidActionType: return "schema.invalid-action-type";
    case DiagnosticCode::EnumEntityInHierarchy: return "schema.enum-entity-in-hierarchy";
    case DiagnosticCode::ExternalWithEnumAncestor: return "schema.external-with-enum-ancestor";
    case DiagnosticCode::EmptyList: return "schema.empty-list";
    case DiagnosticCode::DuplicateDeclarations: return "schema.duplicate-declarations";
    case DiagnosticCode::DuplicateContext: return "schema.duplicate-context";
    case DiagnosticCode::DuplicatePrincipalOrResource: return "schema.duplicate-principal-or-resource";
    case DiagnosticCode::NoPrincipalOrResource: return "schema.no-principal-or-resource";
    case DiagnosticCode::DuplicateNamespaces: return "schema.duplicate-namespaces";
    case DiagnosticCode::UnknownTypeName: return "schema.unknown-type-name";
    case DiagnosticCode::ReservedSchemaKeyword: return "schema.reserved-schema-keyword";
    case DiagnosticCode::ShadowsBuiltin: return "schema.shadows-builtin";
    case DiagnosticCode::ShadowsEntity: return "schema.shadows-entity";
    case DiagnosticCode::SchemaNameCollisions: return "schema.name-collisions";
    case DiagnosticCode::UnconvertibleEntityTypeShape: return "schema.unconvertible-entity-type-shape";
    case DiagnosticCode::UnrecognizedEntityType: return "validation.unrecognized-entity-type";
    case DiagnosticCode::UnrecognizedActionId: return "validation.unrecognized-action-id";
    case DiagnosticCode::UnexpectedType: return "validation.unexpected-type";
    case DiagnosticCode::IncompatibleTypes: return "validation.incompatible-types";
    case DiagnosticCode::UnsafeAttributeAccess: return "validation.unsafe-attribute-access";
    case DiagnosticCode::UnsafeOptionalAttributeAccess: return "validation.unsafe-optional-attribute-access";
    case DiagnosticCode::UnsafeTagAccess: return "validation.unsafe-tag-access";
    case DiagnosticCode::NoTagsAllowed: return "validation.no-tags-allowed";
    case DiagnosticCode::UndefinedFunction: return "validation.undefined-function";
    case DiagnosticCode::WrongNumberArguments: return "validation.wrong-number-arguments";
    case DiagnosticCode::FunctionArgumentValidation: return "validation.function-argument-validation";
    case DiagnosticCode::EmptySetForbidden: return "validation.empty-set-forbidden";
    case DiagnosticCode::NonLiteralExtensionConstructor: return "validation.non-literal-extension-constructor";
    case DiagnosticCode::InternalInvariantViolation: return "validation.internal-invariant-violation";
    case DiagnosticCode::ValidationInvalidEnumEntity: return "validation.invalid-enum-entity";
    case DiagnosticCode::EntityDereferenceLevelViolation: return "validation.entity-dereference-level-violation";
    case DiagnosticCode::IncompleteSchema: return "validation.incomplete-schema";
    case DiagnosticCode::MixedScriptString: return "validation.mixed-script-string";
    case DiagnosticCode::BidirectionalCharactersInString: return "validation.bidirectional-characters-in-string";
    case DiagnosticCode::BidirectionalCharactersInIdentifier: return "validation.bidirectional-characters-in-identifier";
    case DiagnosticCode::MixedScriptIdentifier: return "validation.mixed-script-identifier";
    case DiagnosticCode::ConfusableIdentifier: return "validation.confusable-identifier";
    case DiagnosticCode::ImpossiblePolicy: return "validation.impossible-policy";
    case DiagnosticCode::InvalidActionApplication: return "validation.invalid-action-application";
    case DiagnosticCode::RequestUndeclaredAction: return "request.undeclared-action";
    case DiagnosticCode::RequestExternalAction: return "request.external-action";
    case DiagnosticCode::UndeclaredPrincipalType: return "request.undeclared-principal-type";
    case DiagnosticCode::UndeclaredResourceType: return "request.undeclared-resource-type";
    case DiagnosticCode::InvalidPrincipalType: return "request.invalid-principal-type";
    case DiagnosticCode::InvalidResourceType: return "request.invalid-resource-type";
    case DiagnosticCode::InvalidContext: return "request.invalid-context";
    case DiagnosticCode::TypeOfContext: return "request.type-of-context";
    case DiagnosticCode::RequestInvalidEnumEntity: return "request.invalid-enum-entity";
    case DiagnosticCode::ContextNotARecord: return "request.context-not-a-record";
    case DiagnosticCode::ContextMissingAction: return "request.context-missing-action";
    case DiagnosticCode::EntityDoesNotExist: return "evaluation.entity-does-not-exist";
    case DiagnosticCode::EntityAttributeDoesNotExist: return "evaluation.entity-attribute-does-not-exist";
    case DiagnosticCode::RecordAttributeDoesNotExist: return "evaluation.record-attribute-does-not-exist";
    case DiagnosticCode::EntityTagDoesNotExist: return "evaluation.entity-tag-does-not-exist";
    case DiagnosticCode::ExtensionFunctionLookupFailed: return "evaluation.extension-function-lookup-failed";
    case DiagnosticCode::EvaluationTypeError: return "evaluation.type-error";
    case DiagnosticCode::WrongArgumentCount: return "evaluation.wrong-argument-count";
    case DiagnosticCode::IntegerOverflow: return "evaluation.integer-overflow";
    case DiagnosticCode::UnlinkedSlot: return "evaluation.unlinked-slot";
    case DiagnosticCode::ExtensionFunctionExecutionFailed: return "evaluation.extension-function-execution-failed";
    case DiagnosticCode::RecursionLimit: return "evaluation.recursion-limit";
    }
    return "unknown";
}

auto RenderDiagnostic(const Diagnostic& diagnostic) -> std::string
{
    std::string out{};
    const SourceText* source = diagnostic.source.get();

    if (source != nullptr) { AppendLocationPrefix(out, *source, diagnostic.primary); }
    std::format_to(std::back_inserter(out), "{}[{}]: {}\n", SeverityName(diagnostic.severity),
                   CodeName(diagnostic.code), diagnostic.message);
    if (source != nullptr && diagnostic.primary) { AppendExcerpt(out, source->text, *diagnostic.primary); }

    for (const DiagnosticLabel& label : diagnostic.labels)
    {
        if (source != nullptr) { AppendLocationPrefix(out, *source, label.span); }
        std::format_to(std::back_inserter(out), "note: {}\n", label.text);
        if (source != nullptr) { AppendExcerpt(out, source->text, label.span); }
    }

    if (diagnostic.help) { std::format_to(std::back_inserter(out), "help: {}\n", *diagnostic.help); }
    return out;
}

auto RenderDiagnostics(std::span<const Diagnostic> diagnostics) -> std::string
{
    std::string out{};
    for (const Diagnostic& diagnostic : diagnostics)
    {
        if (!out.empty()) { out += '\n'; }
        out += RenderDiagnostic(diagnostic);
    }
    return out;
}

} // namespace cedar
