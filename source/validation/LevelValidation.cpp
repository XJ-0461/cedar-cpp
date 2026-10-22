#include "validation/LevelValidation.hpp"

#include "validation/Types.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cedar::detail
{

namespace
{

[[nodiscard]] auto IsNamedEntity(const Type& type) noexcept -> bool
{
    return std::holds_alternative<types::Entity>(type.kind);
}

[[nodiscard]] auto IsAnyEntityKind(const Type& type) noexcept -> bool
{
    return std::holds_alternative<types::Entity>(type.kind) || std::holds_alternative<types::AnyEntity>(type.kind);
}

[[nodiscard]] auto IsRecord(const Type& type) noexcept -> bool
{
    return std::holds_alternative<types::Record>(type.kind);
}

// Rust's `LevelChecker`. A node's type is the one the typechecker recorded for it; the operands
// it skipped are skipped here too, as they are absent from Rust's typed expression.
class LevelChecker final
{
public:
    LevelChecker(const ValidatorSchema& schema, const Template& body, const TypecheckEnvironment& environment,
                 const NodeTypes& nodeTypes, const std::uint32_t maximumLevel, std::vector<Diagnostic>& errors)
        : schema_{schema}, body_{body}, arena_{body.Arena()}, environment_{environment}, nodeTypes_{nodeTypes},
          maximumLevel_{maximumLevel}, errors_{errors}
    {
    }

    // Rust's `check_expr_level`.
    auto Check(ExpressionIndex index) -> void;

private:
    // Rust's `check_entity_deref_target_level`: how many dereferences reaching the entity `index`
    // computes takes. `accessPath` (innermost attribute last) is the chain of record attributes
    // read from `index`, so that only the accessed field of a record literal counts.
    [[nodiscard]] auto TargetLevel(ExpressionIndex index, std::vector<std::string_view> accessPath) -> std::uint32_t;

    auto CheckHasPath(ExpressionIndex index, const node::HasAttribute& node) -> void;

    // Rust's `attr_chain_cost_from_root`: the dereferences `root has a.b.c` makes along the
    // chain, not counting the last attribute, which is only tested.
    [[nodiscard]] auto ChainCost(const Type& root, std::span<const std::string_view> path) const -> std::uint32_t;

    // Rust's `ext_has_attr_first_entity_path_rev`: the attributes from the record `type` to the
    // first entity the chain dereferences, innermost first; nothing when no entity is reached
    // before the last attribute.
    [[nodiscard]] auto FirstEntityPath(const Type& type, std::span<const std::string_view> path) const
        -> std::optional<std::vector<std::string_view>>;

    [[nodiscard]] auto Resolve(ExpressionIndex index) const noexcept -> ExpressionIndex
    {
        while (const auto* folded = std::get_if<node::Folded>(&arena_.Node(index))) { index = folded->written; }
        return index;
    }

    [[nodiscard]] auto IsTyped(const ExpressionIndex index) const noexcept -> bool
    {
        return nodeTypes_.Find(Resolve(index)) != nullptr;
    }

    // The type recorded for `index`; null when it was not typechecked or has no type.
    [[nodiscard]] auto TypeOf(const ExpressionIndex index) const noexcept -> const Type*
    {
        const std::optional<Type>* type = nodeTypes_.Find(Resolve(index));
        return type != nullptr && type->has_value() ? &**type : nullptr;
    }

    auto Report(const ExpressionIndex at, std::string message) -> void
    {
        Diagnostic diagnostic{.code = DiagnosticCode::EntityDereferenceLevelViolation,
                              .severity = Severity::Error,
                              .message = std::move(message),
                              .help = std::nullopt,
                              .primary = arena_.Span(at),
                              .labels = {},
                              .source = body_.Source()};
        Add(std::move(diagnostic));
    }

    auto ReportLevelExceeded(const ExpressionIndex at, const std::uint32_t actualLevel) -> void
    {
        Report(at, std::format("this policy requires level {}, which exceeds the maximum allowed level ({})",
                               actualLevel, maximumLevel_));
    }

    auto ReportLiteralTarget(const ExpressionIndex at) -> void
    {
        Report(at, "entity literals cannot be dereferenced at any level");
    }

    auto ReportInvariant(const ExpressionIndex at) -> void
    {
        Add(Diagnostic{.code = DiagnosticCode::InternalInvariantViolation,
                       .severity = Severity::Error,
                       .message = "internal invariant violated: level validation reached an expression of an "
                                  "unexpected type",
                       .help = std::nullopt,
                       .primary = arena_.Span(at),
                       .labels = {},
                       .source = body_.Source()});
    }

    // Rust collects the level errors into a set.
    auto Add(Diagnostic diagnostic) -> void
    {
        const bool seen = std::ranges::any_of(errors_, [&](const Diagnostic& existing) {
            return existing.code == diagnostic.code && existing.primary == diagnostic.primary &&
                   existing.message == diagnostic.message && existing.help == diagnostic.help;
        });
        if (!seen) { errors_.push_back(std::move(diagnostic)); }
    }

    // The dereference of the entity `target` at `at`.
    auto CheckDereference(const ExpressionIndex at, const ExpressionIndex target) -> void
    {
        const std::uint32_t level = TargetLevel(target, {});
        if (level >= maximumLevel_) { ReportLevelExceeded(at, level + 1); }
    }

    const ValidatorSchema& schema_;
    const Template& body_;
    const ExpressionArena& arena_;
    const TypecheckEnvironment& environment_;
    const NodeTypes& nodeTypes_;
    std::uint32_t maximumLevel_;
    std::vector<Diagnostic>& errors_;
};

auto LevelChecker::Check(ExpressionIndex index) -> void
{
    index = Resolve(index);
    if (!IsTyped(index)) { return; }
    const auto checkIfTyped = [&](const ExpressionIndex child) {
        if (IsTyped(child)) { Check(child); }
    };
    arena_.Visit(
        index,
        Overloaded{
            [](const node::Literal&) {},
            [](const node::Folded&) {},
            [](const node::Variable&) {},
            [](const node::Slot&) {},
            [&](const node::If& node) {
                Check(node.condition);
                checkIfTyped(node.thenBranch);
                checkIfTyped(node.elseBranch);
            },
            [&](const node::And& node) {
                Check(node.left);
                checkIfTyped(node.right);
            },
            [&](const node::Or& node) {
                Check(node.left);
                checkIfTyped(node.right);
            },
            [&](const node::Unary& node) { Check(node.operand); },
            [&](const node::Binary& node) {
                switch (node.operation)
                {
                case BinaryOperator::HasTag:
                case BinaryOperator::GetTag:
                case BinaryOperator::In:
                    CheckDereference(index, node.left);
                    Check(node.right);
                    return;
                default:
                    Check(node.left);
                    Check(node.right);
                    return;
                }
            },
            [&](const node::ExtensionCall& node) {
                for (const ExpressionIndex argument : arena_.Elements(node.arguments)) { Check(argument); }
            },
            [&](const node::GetAttribute& node) {
                const Type* object = TypeOf(node.object);
                if (object != nullptr && IsNamedEntity(*object)) { CheckDereference(index, node.object); }
                else if (object != nullptr && IsRecord(*object)) { Check(node.object); }
                else { ReportInvariant(index); }
            },
            [&](const node::HasAttribute& node) {
                if (node.path.count > 1)
                {
                    CheckHasPath(index, node);
                    return;
                }
                const Type* object = TypeOf(node.object);
                if (object != nullptr && IsNamedEntity(*object)) { CheckDereference(index, node.object); }
                else if (object != nullptr && IsRecord(*object)) { Check(node.object); }
                else { ReportInvariant(index); }
            },
            [&](const node::Like& node) { Check(node.operand); },
            [&](const node::Is& node) { Check(node.operand); },
            [&](const node::SetLiteral& node) {
                for (const ExpressionIndex element : arena_.Elements(node.elements)) { Check(element); }
            },
            [&](const node::RecordLiteral& node) {
                for (const RecordLiteralField& field : arena_.Elements(node.fields)) { Check(field.value); }
            },
        });
}

auto LevelChecker::CheckHasPath(const ExpressionIndex index, const node::HasAttribute& node) -> void
{
    const Type* object = TypeOf(node.object);
    if (object == nullptr || !(IsNamedEntity(*object) || IsRecord(*object)))
    {
        // Any other type is a type error the typechecker reported.
        Check(node.object);
        return;
    }
    const std::span<const std::string_view> path = arena_.Elements(node.path);
    const bool recordRoot = IsRecord(*object);
    const std::uint32_t chainCost = ChainCost(*object, path);
    if (chainCost > maximumLevel_)
    {
        ReportLevelExceeded(index, chainCost);
        return;
    }
    if (recordRoot) { Check(node.object); }
    std::optional<std::vector<std::string_view>> firstEntityPath =
        recordRoot ? FirstEntityPath(*object, path) : std::optional<std::vector<std::string_view>>{std::in_place};
    if (!firstEntityPath) { return; }
    const std::uint32_t budget = maximumLevel_ - chainCost;
    const std::uint32_t level = TargetLevel(node.object, std::move(*firstEntityPath));
    if (level > budget) { ReportLevelExceeded(index, level + chainCost); }
}

auto LevelChecker::ChainCost(const Type& root, const std::span<const std::string_view> path) const -> std::uint32_t
{
    if (path.empty()) { return 0; }
    std::uint32_t cost = IsAnyEntityKind(root) ? 1 : 0;
    if (path.size() == 1) { return cost; }
    Type current = root;
    for (const std::string_view attribute : path.first(path.size() - 1))
    {
        const std::optional<AttributeType> found = LookupAttributeType(schema_, current, attribute);
        if (!found) { break; }
        if (IsNamedEntity(*found->type)) { ++cost; }
        current = *found->type;
    }
    return cost;
}

auto LevelChecker::FirstEntityPath(const Type& type, const std::span<const std::string_view> path) const
    -> std::optional<std::vector<std::string_view>>
{
    if (path.size() <= 1) { return std::nullopt; }
    const std::optional<AttributeType> found = LookupAttributeType(schema_, type, path.front());
    if (!found) { return std::nullopt; }
    if (IsNamedEntity(*found->type)) { return std::vector<std::string_view>{path.front()}; }
    std::optional<std::vector<std::string_view>> rest = FirstEntityPath(*found->type, path.subspan(1));
    if (rest) { rest->push_back(path.front()); }
    return rest;
}

auto LevelChecker::TargetLevel(ExpressionIndex index, std::vector<std::string_view> accessPath) -> std::uint32_t
{
    index = Resolve(index);
    return arena_.Visit(
        index,
        Overloaded{
            [&](const node::Variable&) -> std::uint32_t { return 0; },
            [&](const node::Slot&) -> std::uint32_t {
                ReportLiteralTarget(index);
                return 0;
            },
            [&](const node::Literal& node) -> std::uint32_t {
                if (node.value.Kind() != ValueKind::Entity)
                {
                    ReportInvariant(index);
                    return 0;
                }
                // The environment's own action may be dereferenced (as the Lean model allows).
                if (node.value.AsEntity() != environment_.action->name) { ReportLiteralTarget(index); }
                return 0;
            },
            [&](const node::If& node) -> std::uint32_t {
                Check(node.condition);
                std::uint32_t level = 0;
                if (IsTyped(node.thenBranch)) { level = TargetLevel(node.thenBranch, accessPath); }
                if (IsTyped(node.elseBranch)) { level = std::max(level, TargetLevel(node.elseBranch, accessPath)); }
                return level;
            },
            [&](const node::GetAttribute& node) -> std::uint32_t {
                const Type* object = TypeOf(node.object);
                if (object != nullptr && IsNamedEntity(*object))
                {
                    return TargetLevel(node.object, std::move(accessPath)) + 1;
                }
                if (object != nullptr && IsRecord(*object))
                {
                    accessPath.push_back(node.attribute);
                    return TargetLevel(node.object, std::move(accessPath));
                }
                ReportInvariant(index);
                return 0;
            },
            [&](const node::Binary& node) -> std::uint32_t {
                if (node.operation != BinaryOperator::GetTag)
                {
                    ReportInvariant(index);
                    return 0;
                }
                const std::uint32_t level = TargetLevel(node.left, std::move(accessPath));
                Check(node.right);
                return level + 1;
            },
            [&](const node::RecordLiteral& node) -> std::uint32_t {
                const std::span<const RecordLiteralField> fields = arena_.Elements(node.fields);
                const RecordLiteralField* accessed = nullptr;
                if (!accessPath.empty())
                {
                    const std::string_view attribute = accessPath.back();
                    accessPath.pop_back();
                    const auto found = std::ranges::find(fields, attribute, &RecordLiteralField::key);
                    if (found != fields.end()) { accessed = &*found; }
                }
                if (accessed == nullptr)
                {
                    ReportInvariant(index);
                    return 0;
                }
                for (const RecordLiteralField& field : fields)
                {
                    if (&field != accessed) { Check(field.value); }
                }
                return TargetLevel(accessed->value, std::move(accessPath));
            },
            [&](const auto&) -> std::uint32_t {
                ReportInvariant(index);
                return 0;
            },
        });
}

} // namespace

auto CheckDereferenceLevels(const ValidatorSchema& schema, const Template& body,
                            const TypecheckEnvironment& environment, const NodeTypes& nodeTypes,
                            const std::uint32_t maximumLevel, std::vector<Diagnostic>& errors) -> void
{
    LevelChecker{schema, body, environment, nodeTypes, maximumLevel, errors}.Check(body.Condition());
}

} // namespace cedar::detail
