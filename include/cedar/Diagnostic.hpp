#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cedar
{

/// A half-open range of byte offsets `[begin, end)` into one source text. Offsets are 32-bit because
/// spans are stored per expression node; inputs are capped below 4 GiB by the readers that make
/// spans. An empty span (`begin == end`) marks a position, such as the end of input.
struct SourceSpan
{
    std::uint32_t begin{};
    std::uint32_t end{};

    [[nodiscard]] constexpr auto Length() const noexcept -> std::uint32_t { return end - begin; }
    [[nodiscard]] constexpr auto IsEmpty() const noexcept -> bool { return begin == end; }

    friend constexpr auto operator==(const SourceSpan&, const SourceSpan&) noexcept -> bool = default;
};

/// An owned copy of an input that diagnostics point into: a display name (a file path, or a label
/// such as `<policy>`) and the full text. Held through `SharedSourceText` so every diagnostic about
/// one input shares a single copy.
struct SourceText
{
    std::string name{};
    std::string text{};
};

/// Shared, immutable ownership of a `SourceText`.
using SharedSourceText = std::shared_ptr<const SourceText>;

/// Makes a `SharedSourceText` holding copies of `name` and `text`.
[[nodiscard]] auto MakeSourceText(std::string name, std::string text) -> SharedSourceText;

/// A 1-based line and column. The column counts Unicode scalar values (UTF-8 continuation bytes are
/// not counted), so it matches what an editor shows for non-ASCII text.
struct SourceLocation
{
    std::uint32_t line{1};
    std::uint32_t column{1};

    friend constexpr auto operator==(const SourceLocation&, const SourceLocation&) noexcept -> bool = default;
};

/// The line and column of byte `offset` in `text`. Lines end at `\n`. An offset past the end of
/// `text` is clamped to the end.
[[nodiscard]] auto LocateOffset(std::string_view text, std::uint32_t offset) -> SourceLocation;

/// How serious a diagnostic is. Validator and schema warnings are `Warning`; everything that makes
/// an operation fail is `Error`.
enum class Severity : std::uint8_t
{
    Error,
    Warning,
    Info,
};

/// The lowercase word a severity renders as: `error`, `warning` or `info`.
[[nodiscard]] auto SeverityName(Severity severity) noexcept -> std::string_view;

/// The stable identity of a diagnostic. Tests and callers match on the code, never on the message,
/// which leaves the wording free to change. Numeric values are stable: a code is never renumbered
/// or reused, and new codes take unused values in their area's range.
///
/// Areas: parse 1000-1999 (policy text, policy JSON, generic JSON), policy set and linking
/// 2000-2999, entities 3000-3999, schema 4000-4999, validation 5000-5999 (errors 50xx, warnings
/// 51xx), request and context 6000-6999, evaluation 7000-7999. Each Rust error kind maps to one
/// code; where Rust has the same condition under two enums (for example the escape errors reached
/// from both the policy and schema parsers) both map to the one code.
enum class DiagnosticCode : std::uint16_t
{
    // ---- Parse: lexical and syntactic, for both the policy and the schema grammar ----
    InvalidToken = 1001,
    UnexpectedToken = 1002,
    UnexpectedEndOfInput = 1003,
    ExtraToken = 1004,
    NestingTooDeep = 1005, ///< Expressions nested past the parser's depth limit.

    // ---- Parse: policy text to AST (core/src/parser/err.rs ToASTErrorKind) ----
    DuplicateTemplateId = 1101,
    DuplicatePolicyId = 1102,
    ExpectedStaticPolicy = 1103,
    ExpectedTemplate = 1104,
    DuplicateAnnotation = 1105,
    SlotsInConditionClause = 1106,
    MissingScopeVariable = 1107,
    ExtraScopeElement = 1108,
    ReservedIdentifier = 1109,
    InvalidIdentifier = 1110,
    InvalidSingleEquals = 1111,
    InvalidEffect = 1112,
    InvalidCondition = 1113,
    InvalidScopeVariable = 1114,
    IncorrectVariable = 1115,
    InvalidScopeOperator = 1116,
    InvalidActionScopeOperator = 1117,
    IsInActionScope = 1118,
    IsWithEquals = 1119,
    InvalidActionType = 1120,
    EmptyClause = 1121,
    InvalidString = 1122,
    ArbitraryVariable = 1123,
    InvalidAttribute = 1124,
    InvalidHasRightHandSide = 1125,
    PathAsAttribute = 1126,
    FunctionCallOnMethod = 1127,
    MethodCallOnFunction = 1128,
    InvalidPattern = 1129,
    InvalidIsType = 1130,
    UnexpectedConstruct = 1131, ///< Rust `WrongNode`: one construct where another was required.
    AmbiguousOperators = 1132,
    UnsupportedDivision = 1133,
    UnsupportedModulo = 1134,
    DuplicateRecordKey = 1135, ///< Rust `ExpressionConstructionError::DuplicateKey`.
    IntegerLiteralTooLarge = 1136,
    UnaryOperatorLimit = 1137,
    VariableCall = 1138,
    NoMethods = 1139,
    UnknownMethod = 1140,
    UnknownFunction = 1141,
    InvalidEntityLiteral = 1142,
    ExpressionCall = 1143,
    InvalidAccess = 1144,
    InvalidIndex = 1145,
    NonStringIndex = 1146,
    TypeConstraints = 1147,
    NonNormalizedString = 1148,
    WrongArity = 1149,
    InvalidEscape = 1150, ///< Rust `UnescapeError`, from string literals and patterns.
    WrongEntityArgument = 1151,
    InvalidSlot = 1152,
    ReservedNamespace = 1153, ///< Rust `ReservedNameError`: a name inside `__cedar`.
    InvertedIsIn = 1154,

    // ---- Parse: JSON input in general, and the JSON policy format (core/src/est/err.rs) ----
    JsonSyntax = 1201,          ///< The text is not well-formed JSON.
    JsonUnexpectedShape = 1202, ///< Well-formed JSON with a missing, unknown or wrongly typed field.
    JsonInvalidSlotName = 1203,
    JsonActionSlot = 1204,
    JsonMissingOperator = 1205,
    JsonMultipleOperators = 1206,

    // ---- Policy set and linking (core/src/ast/policy_set.rs, ast/policy.rs, api/err.rs) ----
    PolicyIdAlreadyDefined = 2001,
    LinkWrongSlots = 2002,      ///< Rust `LinkingError::ArityError` and EST `MissedSlot`.
    LinkNoSuchTemplate = 2003,
    LinkPolicyIdConflict = 2004,
    ExpectedStaticPolicyId = 2005, ///< Rust `PolicySetError::ExpectedStatic`.
    ExpectedTemplateId = 2006,     ///< Rust `PolicySetError::ExpectedTemplate`.
    PolicyNonexistent = 2007,
    TemplateNonexistent = 2008,
    RemoveTemplateWithActiveLinks = 2009,
    RemoveTemplateNotTemplate = 2010,
    LinkNonexistent = 2011,
    UnlinkLinkNotLink = 2012,

    // ---- Entities: store structure (core/src/entities/err.rs, transitive_closure/err.rs) ----
    DuplicateEntity = 3001,
    TransitiveClosureMissingEdge = 3002,
    TransitiveClosureCycle = 3003,
    SelfAncestor = 3004,
    DuplicateAncestor = 3005,
    ActionParentIsNotAction = 3006,

    // ---- Entities: JSON reading (core/src/entities/json/err.rs JsonDeserializationError) ----
    InvalidEscapeValue = 3101,      ///< Rust `ParseEscape`: a bad `__entity` or `__extn` payload.
    InvalidRestrictedExpression = 3102,
    ExpectedLiteralEntityReference = 3103,
    ExpectedExtensionValue = 3104,
    MissingImpliedConstructor = 3105,
    IncorrectArgumentCount = 3106,
    ExtensionFunctionLookup = 3107, ///< Also conformance `ExtensionFunctionLookup`.
    DuplicateKey = 3108,
    EntityAttributeEvaluation = 3109,
    UnexpectedRecordAttribute = 3110,
    MissingRequiredRecordAttribute = 3111,
    TypeMismatch = 3112,            ///< Also conformance `TypeMismatch`.
    ExpressionTag = 3113,           ///< Rust `ExprTag`: an `__expr` escape.
    NullValue = 3114,
    ReservedName = 3115,
    UnsupportedEntityTags = 3116,

    // ---- Entities: schema conformance (core/src/entities/conformance/err.rs) ----
    UnexpectedEntityAttribute = 3201,
    UnexpectedEntityTag = 3202,
    MissingRequiredEntityAttribute = 3203,
    InvalidAncestorType = 3204,
    UnexpectedEntityType = 3205,
    ExternalEntityType = 3206,
    UndeclaredAction = 3207,
    ActionDeclarationMismatch = 3208,
    InvalidEnumEntity = 3209,

    // ---- Entities: JSON writing (core/src/entities/json/err.rs JsonSerializationError) ----
    SerializationReservedKey = 3301,
    SerializationExtensionCallArity = 3302, ///< Rust `ExtnCall0Arguments`, `ExtnCall2OrMoreArguments`.

    // ---- Schema: linking and well-formedness (core/src/validator/schema/err.rs SchemaError) ----
    ActionTransitiveClosure = 4001,
    EntityTypeTransitiveClosure = 4002,
    UnsupportedSchemaFeature = 4003,
    UndeclaredEntityTypes = 4004,
    UndeclaredActionDescendants = 4005,
    TypeNotDefined = 4006,
    ActionNotDefined = 4007,
    TypeShadowing = 4008,
    ActionShadowing = 4009,
    DuplicateEntityType = 4010,
    DuplicateAction = 4011,
    DuplicateCommonType = 4012,
    CycleInActionHierarchy = 4013,
    CycleInCommonTypeReferences = 4014,
    ActionEntityTypeDeclared = 4015,
    ContextOrShapeNotRecord = 4016,
    ActionAttributesContainEmptySet = 4017,
    UnsupportedActionAttribute = 4018,
    ActionAttributeEvaluation = 4019,
    ExpressionEscapeUsed = 4020,
    UnknownExtensionType = 4021,
    SchemaInvalidActionType = 4022,
    EnumEntityInHierarchy = 4023,
    ExternalWithEnumAncestor = 4024,

    // ---- Schema: Cedar schema syntax (core/src/validator/cedar_schema/err.rs) ----
    EmptyList = 4101,
    DuplicateDeclarations = 4102,
    DuplicateContext = 4103,
    DuplicatePrincipalOrResource = 4104,
    NoPrincipalOrResource = 4105,
    DuplicateNamespaces = 4106,
    UnknownTypeName = 4107,
    ReservedSchemaKeyword = 4108,

    // ---- Schema: warnings (SchemaWarning) ----
    ShadowsBuiltin = 4201,
    ShadowsEntity = 4202,

    // ---- Schema: printing in Cedar schema syntax (core/src/validator/cedar_schema/fmt.rs) ----
    SchemaNameCollisions = 4301,
    UnconvertibleEntityTypeShape = 4302,

    // ---- Validation errors (core/src/validator/diagnostics.rs ValidationError), one to one ----
    UnrecognizedEntityType = 5001,
    UnrecognizedActionId = 5002,
    UnexpectedType = 5003,
    IncompatibleTypes = 5004,
    UnsafeAttributeAccess = 5005,
    UnsafeOptionalAttributeAccess = 5006,
    UnsafeTagAccess = 5007,
    NoTagsAllowed = 5008,
    UndefinedFunction = 5009,
    WrongNumberArguments = 5010,
    FunctionArgumentValidation = 5011,
    EmptySetForbidden = 5012,
    NonLiteralExtensionConstructor = 5013,
    InternalInvariantViolation = 5014,
    ValidationInvalidEnumEntity = 5015,
    EntityDereferenceLevelViolation = 5016,
    IncompleteSchema = 5017,

    // ---- Validation warnings (ValidationWarning), one to one ----
    MixedScriptString = 5101,
    BidirectionalCharactersInString = 5102,
    BidirectionalCharactersInIdentifier = 5103,
    MixedScriptIdentifier = 5104,
    ConfusableIdentifier = 5105,
    ImpossiblePolicy = 5106,
    InvalidActionApplication = 5107,

    // ---- Request validation (api/err.rs RequestValidationError) and context construction ----
    RequestUndeclaredAction = 6001,
    RequestExternalAction = 6002,
    UndeclaredPrincipalType = 6003,
    UndeclaredResourceType = 6004,
    InvalidPrincipalType = 6005,
    InvalidResourceType = 6006,
    InvalidContext = 6007,
    TypeOfContext = 6008,
    RequestInvalidEnumEntity = 6009,
    ContextNotARecord = 6101,
    ContextMissingAction = 6102, ///< Rust `ContextJsonError::MissingAction`.

    // ---- Evaluation (one per EvaluationErrorKind) ----
    EntityDoesNotExist = 7001,
    EntityAttributeDoesNotExist = 7002,
    RecordAttributeDoesNotExist = 7003,
    EntityTagDoesNotExist = 7004,
    ExtensionFunctionLookupFailed = 7005,
    EvaluationTypeError = 7006,
    WrongArgumentCount = 7007,
    IntegerOverflow = 7008,
    UnlinkedSlot = 7009,
    ExtensionFunctionExecutionFailed = 7010,
    RecursionLimit = 7011,
};

/// The stable dotted name of a code, `<area>.<kebab-case-kind>`, for example
/// `parse.unexpected-token` or `validation.unsafe-attribute-access`. Names are as stable as the
/// numeric values.
[[nodiscard]] auto CodeName(DiagnosticCode code) noexcept -> std::string_view;

/// A secondary span with its own text, such as "first declared here".
struct DiagnosticLabel
{
    SourceSpan span{};
    std::string text{};
};

/// One reported condition. Every string owns its characters, because a diagnostic outlives the
/// operation that made it and is rendered later. Spans refer to `source` when it is set; a
/// diagnostic about an input with no text form (a hand-built request, say) leaves `source` null and
/// `primary` empty.
struct Diagnostic
{
    DiagnosticCode code{};
    Severity severity{Severity::Error};
    std::string message{};
    std::optional<std::string> help{};
    std::optional<SourceSpan> primary{};
    std::vector<DiagnosticLabel> labels{};
    SharedSourceText source{};
};

/// Renders one diagnostic for a person:
///
///     policy.cedar:3:14: error[parse.unexpected-token]: unexpected `)`
///       permit(principal, action, resource) when { ) };
///                                                  ^
///     policy.cedar:3:1: note: label text
///       ...
///     help: the help text
///
/// The location prefix is omitted without a source, and the excerpt is omitted without a span.
/// The output ends with a newline.
[[nodiscard]] auto RenderDiagnostic(const Diagnostic& diagnostic) -> std::string;

/// Renders every diagnostic in order, separated by blank lines.
[[nodiscard]] auto RenderDiagnostics(std::span<const Diagnostic> diagnostics) -> std::string;

} // namespace cedar
