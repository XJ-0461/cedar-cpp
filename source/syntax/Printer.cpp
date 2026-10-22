#include "syntax/Printer.hpp"

#include <cedar/Syntax.hpp>

#include "extensions/ExtensionCalls.hpp"
#include "model/EscapeDebug.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Canonical Cedar text, matching what Rust prints for the AST: `Display for Expr` converts to the
// EST and prints that (`cedar-policy-core/src/est/expr.rs:1652`), and `Display for TemplateBody`
// lays out the policy (`cedar-policy-core/src/ast/policy.rs:1257`).

namespace cedar::detail
{

namespace
{

auto IsMethodStyle(const Name& function) -> bool
{
    return IsExtensionFunction(function.QualifiedText(), ExtensionCallStyle::Method);
}

// Rust's `is_normalized_ident`: identifier syntax, not a reserved keyword, not `__cedar`. Such an
// attribute name or record key prints bare; any other is quoted.
auto IsBareIdentifier(const std::string_view text) -> bool
{
    return CheckIdentifier(text).has_value() && text != ReservedNamespaceName;
}

void AppendQuoted(std::string& output, const std::string_view text)
{
    output.push_back('"');
    AppendEscapeDebug(output, text);
    output.push_back('"');
}

class ExpressionPrinter final
{
public:
    // `bindings` replaces each bound slot with its entity; null prints slots as `?principal`.
    ExpressionPrinter(std::string& output, const ExpressionArena& arena, const SlotBindings* bindings) noexcept
        : output_{output}, arena_{arena}, bindings_{bindings}
    {
    }

    // The expression with no parentheses around it.
    void Print(const ExpressionIndex index)
    {
        arena_.Visit(Written(index), Overloaded{
            [&](const node::Literal& literal) { PrintLiteral(literal.value); },
            [](const node::Folded&) {}, // Written() has looked through it.
            [&](const node::Variable& variable) { output_ += ToString(variable.variable); },
            [&](const node::Slot& slot) { PrintSlot(slot.slot); },
            [&](const node::If& branch) {
                output_ += "if ";
                Print(branch.condition);
                output_ += " then ";
                Print(branch.thenBranch);
                output_ += " else ";
                Print(branch.elseBranch);
            },
            [&](const node::And& both) {
                PrintLeftAssociative(both.left, " && ", both.right, Is<node::And>(both.left));
            },
            [&](const node::Or& either) {
                PrintLeftAssociative(either.left, " || ", either.right, Is<node::Or>(either.left));
            },
            [&](const node::Unary& unary) { PrintUnary(unary); },
            [&](const node::Binary& binary) { PrintBinary(binary); },
            [&](const node::ExtensionCall& call) { PrintCall(call); },
            [&](const node::GetAttribute& access) {
                PrintOperand(access.object);
                if (IsBareIdentifier(access.attribute))
                {
                    output_.push_back('.');
                    output_ += access.attribute;
                }
                else
                {
                    output_.push_back('[');
                    AppendQuoted(output_, access.attribute);
                    output_.push_back(']');
                }
            },
            [&](const node::HasAttribute& test) {
                PrintOperand(test.object);
                output_ += " has ";
                bool first = true;
                for (const std::string_view attribute : arena_.Elements(test.path))
                {
                    if (!first) { output_.push_back('.'); }
                    first = false;
                    if (IsBareIdentifier(attribute)) { output_ += attribute; }
                    else { AppendQuoted(output_, attribute); }
                }
            },
            [&](const node::Like& like) {
                PrintOperand(like.operand);
                output_ += " like \"";
                PrintPattern(arena_.PatternOf(like));
                output_.push_back('"');
            },
            [&](const node::Is& test) {
                PrintOperand(test.operand);
                output_ += " is ";
                output_ += test.type.GetName().QualifiedText();
            },
            [&](const node::SetLiteral& set) {
                output_.push_back('[');
                PrintList(arena_.Elements(set.elements));
                output_.push_back(']');
            },
            [&](const node::RecordLiteral& record) {
                output_.push_back('{');
                bool first = true;
                for (const RecordLiteralField& field : arena_.Elements(record.fields))
                {
                    if (!first) { output_ += ", "; }
                    first = false;
                    if (IsBareIdentifier(field.key)) { output_ += field.key; }
                    else { AppendQuoted(output_, field.key); }
                    output_ += ": ";
                    Print(field.value);
                }
                output_.push_back('}');
            },
        });
    }

    void PrintEntityReference(const EntityReference& reference, const SlotId slot)
    {
        if (const auto* uid = std::get_if<EntityUid>(&reference)) { output_ += uid->ToString(); }
        else { PrintSlot(slot); }
    }

private:
    // A folded subtree prints as written.
    [[nodiscard]] auto Written(ExpressionIndex index) const noexcept -> ExpressionIndex
    {
        while (const auto* folded = std::get_if<node::Folded>(&arena_.Node(index))) { index = folded->written; }
        return index;
    }

    // Rust's `maybe_with_parens` (`cedar-policy-core/src/est/expr.rs:1952`): literals, variables,
    // slots, set and record literals and the member-access forms print bare; everything else in
    // parentheses.
    [[nodiscard]] auto NeedsParentheses(const ExpressionIndex index) const noexcept -> bool
    {
        return arena_.Visit(Written(index), Overloaded{
            [](const node::Literal&) { return false; },
            [](const node::Folded&) { return false; },
            [](const node::Variable&) { return false; },
            [](const node::Slot&) { return false; },
            [](const node::SetLiteral&) { return false; },
            [](const node::RecordLiteral&) { return false; },
            [](const node::GetAttribute&) { return false; },
            [](const node::ExtensionCall&) { return false; },
            [](const node::Unary& unary) { return unary.operation != UnaryOperator::IsEmpty; },
            [](const node::Binary& binary) {
                switch (binary.operation)
                {
                case BinaryOperator::Contains:
                case BinaryOperator::ContainsAll:
                case BinaryOperator::ContainsAny:
                case BinaryOperator::GetTag:
                case BinaryOperator::HasTag: return false;
                default: return true;
                }
            },
            [](const auto&) { return true; },
        });
    }

    void PrintOperand(const ExpressionIndex index)
    {
        if (NeedsParentheses(index))
        {
            output_.push_back('(');
            Print(index);
            output_.push_back(')');
        }
        else
        {
            Print(index);
        }
    }

    // `&&`, `||`, `+`, `-` and `*` associate to the left, so a left operand of the same operator
    // prints without parentheses.
    void PrintLeftAssociative(const ExpressionIndex left, const std::string_view text, const ExpressionIndex right,
                              const bool leftIsSameOperator)
    {
        if (leftIsSameOperator) { Print(left); }
        else { PrintOperand(left); }
        output_ += text;
        PrintOperand(right);
    }

    template <typename Connective>
    [[nodiscard]] auto Is(const ExpressionIndex index) const noexcept -> bool
    {
        return std::holds_alternative<Connective>(arena_.Node(Written(index)));
    }

    [[nodiscard]] auto IsBinary(const ExpressionIndex index, const BinaryOperator operation) const noexcept -> bool
    {
        const auto* binary = std::get_if<node::Binary>(&arena_.Node(Written(index)));
        return binary != nullptr && binary->operation == operation;
    }

    void PrintList(const std::span<const ExpressionIndex> elements)
    {
        bool first = true;
        for (const ExpressionIndex element : elements)
        {
            if (!first) { output_ += ", "; }
            first = false;
            Print(element);
        }
    }

    void PrintLiteral(const Value& value)
    {
        switch (value.Kind())
        {
        case ValueKind::Bool: output_ += value.AsBool() ? "true" : "false"; break;
        case ValueKind::Long:
            // A negative literal is parenthesized so `(-1).x` does not read back as `-(1.x)`.
            if (value.AsLong() < 0) { std::format_to(std::back_inserter(output_), "({})", value.AsLong()); }
            else { std::format_to(std::back_inserter(output_), "{}", value.AsLong()); }
            break;
        case ValueKind::String: AppendQuoted(output_, value.AsString()); break;
        case ValueKind::Entity: output_ += value.AsEntity().ToString(); break;
        default: AppendCedar(output_, value); break;
        }
    }

    void PrintSlot(const SlotId slot)
    {
        if (bindings_ != nullptr)
        {
            if (const EntityUid* bound = bindings_->Get(slot))
            {
                output_ += bound->ToString();
                return;
            }
        }
        output_ += ToString(slot);
    }

    void PrintUnary(const node::Unary& unary)
    {
        switch (unary.operation)
        {
        case UnaryOperator::Not:
            output_.push_back('!');
            PrintOperand(unary.operand);
            break;
        case UnaryOperator::Negate:
            // Always parenthesized, so the text reads back as a negation, not a negative literal.
            output_ += "-(";
            Print(unary.operand);
            output_.push_back(')');
            break;
        case UnaryOperator::IsEmpty:
            PrintOperand(unary.operand);
            output_ += ".isEmpty()";
            break;
        }
    }

    void PrintBinary(const node::Binary& binary)
    {
        switch (binary.operation)
        {
        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
        case BinaryOperator::Multiply:
            PrintLeftAssociative(binary.left, std::format(" {} ", ToString(binary.operation)), binary.right,
                                 IsBinary(binary.left, binary.operation));
            return;
        case BinaryOperator::Contains:
        case BinaryOperator::ContainsAll:
        case BinaryOperator::ContainsAny:
        case BinaryOperator::GetTag:
        case BinaryOperator::HasTag:
            PrintOperand(binary.left);
            output_.push_back('.');
            output_ += ToString(binary.operation);
            output_.push_back('(');
            Print(binary.right);
            output_.push_back(')');
            return;
        case BinaryOperator::Equal:
        case BinaryOperator::Less:
        case BinaryOperator::LessEqual:
        case BinaryOperator::In:
            PrintOperand(binary.left);
            output_.push_back(' ');
            output_ += ToString(binary.operation);
            output_.push_back(' ');
            PrintOperand(binary.right);
            return;
        }
    }

    void PrintCall(const node::ExtensionCall& call)
    {
        const std::span<const ExpressionIndex> arguments = arena_.Elements(call.arguments);
        if (IsMethodStyle(call.function) && !arguments.empty())
        {
            PrintOperand(arguments.front());
            output_.push_back('.');
            output_ += call.function.QualifiedText();
            output_.push_back('(');
            PrintList(arguments.subspan(1));
            output_.push_back(')');
            return;
        }
        output_ += call.function.QualifiedText();
        output_.push_back('(');
        PrintList(arguments);
        output_.push_back(')');
    }

    // Rust's `Display for Pattern`: each character escaped as `char::escape_debug` does, a literal
    // star as `\*`, a wildcard as `*`.
    void PrintPattern(const Pattern pattern)
    {
        bool first = true;
        for (const std::string_view literal : pattern.Literals())
        {
            if (!first) { output_.push_back('*'); }
            first = false;
            for (const auto run : std::views::split(literal, '*') | std::views::enumerate)
            {
                if (std::get<0>(run) != 0) { output_ += "\\*"; }
                AppendCharacterEscapeDebug(output_, std::string_view{std::get<1>(run)});
            }
        }
    }

    std::string& output_;
    const ExpressionArena& arena_;
    const SlotBindings* bindings_;
};

void PrintScope(std::string& output, ExpressionPrinter& printer, const std::string_view variable,
                const SlotId slot, const PrincipalOrResourceConstraint& constraint)
{
    output += variable;
    std::visit(Overloaded{
                   [](const ScopeAny&) {},
                   [&](const ScopeEqual& equal) {
                       output += " == ";
                       printer.PrintEntityReference(equal.target, slot);
                   },
                   [&](const ScopeIn& in) {
                       output += " in ";
                       printer.PrintEntityReference(in.target, slot);
                   },
                   [&](const ScopeIs& is) {
                       output += " is ";
                       output += is.type.GetName().QualifiedText();
                   },
                   [&](const ScopeIsIn& isIn) {
                       output += " is ";
                       output += isIn.type.GetName().QualifiedText();
                       output += " in ";
                       printer.PrintEntityReference(isIn.target, slot);
                   },
               },
               constraint);
}

// Rust's `Display for TemplateBody` (`cedar-policy-core/src/ast/policy.rs:1257`). An annotation
// written without a value prints as `@key("")`, as the Rust AST stores it.
void PrintTemplate(std::string& output, const Template& body, const SlotBindings* bindings)
{
    for (const Annotation& annotation : body.GetAnnotations())
    {
        output.push_back('@');
        output += annotation.key.Text();
        output.push_back('(');
        AppendQuoted(output, annotation.value.value_or(std::string{}));
        output += ")\n";
    }
    ExpressionPrinter printer{output, body.Arena(), bindings};
    output += ToString(body.GetEffect());
    output += "(\n  ";
    PrintScope(output, printer, "principal", SlotId::Principal, body.Principal().constraint);
    output += ",\n  ";
    std::visit(Overloaded{
                   [&](const ActionAny&) { output += "action"; },
                   [&](const ActionEqual& equal) {
                       output += "action == ";
                       output += equal.action.ToString();
                   },
                   // Always the list form, with no space after the commas, as Rust prints it.
                   [&](const ActionIn& in) {
                       output += "action in [";
                       bool first = true;
                       for (const EntityUid& action : in.actions)
                       {
                           if (!first) { output.push_back(','); }
                           first = false;
                           output += action.ToString();
                       }
                       output.push_back(']');
                   },
               },
               body.Action());
    output += ",\n  ";
    PrintScope(output, printer, "resource", SlotId::Resource, body.Resource().constraint);
    output += "\n)";
    if (const std::optional<ExpressionIndex> condition = body.NonScopeCondition())
    {
        output += " when {\n  ";
        printer.Print(*condition);
        output += "\n};";
    }
    else
    {
        output.push_back(';');
    }
}

void AppendPolicy(std::string& output, const Policy& policy, const TextChoice choice)
{
    const Template& body = policy.GetTemplate();
    if (policy.IsStatic())
    {
        if (const std::optional<std::string_view> text = body.OriginalText(); text && choice == TextChoice::Retained)
        {
            output += *text;
            return;
        }
        PrintTemplate(output, body, nullptr);
        return;
    }
    PrintTemplate(output, body, &policy.Bindings());
}

void AppendTemplate(std::string& output, const Template& body, const TextChoice choice)
{
    if (const std::optional<std::string_view> text = body.OriginalText(); text && choice == TextChoice::Retained)
    {
        output += *text;
    }
    else { PrintTemplate(output, body, nullptr); }
}

} // namespace

auto PrintPolicySet(const PolicySet& policies, const TextChoice choice) -> std::string
{
    std::vector<const Policy*> sortedPolicies;
    sortedPolicies.reserve(policies.Policies().size());
    for (const auto& policy : policies.Policies()) { sortedPolicies.push_back(policy.get()); }
    std::ranges::sort(sortedPolicies, {}, [](const Policy* policy) -> const PolicyId& { return policy->Id(); });

    std::vector<const Template*> sortedTemplates;
    sortedTemplates.reserve(policies.Templates().size());
    for (const auto& body : policies.Templates()) { sortedTemplates.push_back(body.get()); }
    std::ranges::sort(sortedTemplates, {}, [](const Template* body) -> const PolicyId& { return body->Id(); });

    // Rust's `PolicySet::to_cedar` joins the policies, then the templates, with a blank line.
    std::string output;
    bool first = true;
    const auto separate = [&] {
        if (!first) { output += "\n\n"; }
        first = false;
    };
    for (const Policy* policy : sortedPolicies)
    {
        separate();
        AppendPolicy(output, *policy, choice);
    }
    for (const Template* body : sortedTemplates)
    {
        separate();
        AppendTemplate(output, *body, choice);
    }
    return output;
}

} // namespace cedar::detail

namespace cedar
{

auto ToCedar(const Expression& expression) -> std::string
{
    std::string output;
    detail::ExpressionPrinter{output, expression.Arena(), nullptr}.Print(expression.Root());
    return output;
}

auto ToCedar(const Template& body) -> std::string
{
    std::string output;
    detail::AppendTemplate(output, body, detail::TextChoice::Retained);
    return output;
}

auto ToCedar(const Policy& policy) -> std::string
{
    std::string output;
    detail::AppendPolicy(output, policy, detail::TextChoice::Retained);
    return output;
}

auto ToCedar(const PolicySet& policies) -> std::string
{
    return detail::PrintPolicySet(policies, detail::TextChoice::Retained);
}

} // namespace cedar
