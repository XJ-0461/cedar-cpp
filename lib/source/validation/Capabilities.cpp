#include "validation/Capabilities.hpp"

#include <cedar/Value.hpp>

#include <algorithm>
#include <functional>
#include <utility>

namespace cedar::detail
{

namespace
{

// Mixes `value` into `seed` (the splitmix64 finalizer over the sum keeps nearby inputs apart).
[[nodiscard]] constexpr auto Combine(std::uint64_t seed, const std::uint64_t value) noexcept -> std::uint64_t
{
    seed += value + 0x9e3779b97f4a7c15ULL;
    seed = (seed ^ (seed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    seed = (seed ^ (seed >> 27U)) * 0x94d049bb133111ebULL;
    return seed ^ (seed >> 31U);
}

[[nodiscard]] auto TextHash(const std::string_view text) noexcept -> std::uint64_t
{
    return static_cast<std::uint64_t>(std::hash<std::string_view>{}(text));
}

[[nodiscard]] auto LiteralHash(const Value& value) noexcept -> std::uint64_t
{
    const auto kind = static_cast<std::uint64_t>(value.Kind());
    switch (value.Kind())
    {
    case ValueKind::Bool: return Combine(kind, value.AsBool() ? 1U : 0U);
    case ValueKind::Long: return Combine(kind, static_cast<std::uint64_t>(value.AsLong()));
    case ValueKind::String: return Combine(kind, TextHash(value.AsString()));
    case ValueKind::Entity: return Combine(kind, value.AsEntity().Hash());
    default: return kind;
    }
}

[[nodiscard]] auto AttributeStepHash(const std::uint64_t object, const std::string_view attribute) noexcept
    -> std::uint64_t
{
    return Combine(Combine(object, 101U), TextHash(attribute));
}

} // namespace

ExpressionShapes::ExpressionShapes(const ExpressionArena& arena) : arena_{arena}
{
    const std::uint32_t count = std::to_underlying(arena.Root()) + 1;
    hashes_.resize(count);
    for (std::uint32_t position = 0; position < count; ++position)
    {
        const auto index = ExpressionIndex{position};
        const auto of = [this](const ExpressionIndex child) { return hashes_[std::to_underlying(child)]; };
        const ExpressionNode& node = arena.Node(index);
        const auto tag = static_cast<std::uint64_t>(node.index());
        hashes_[position] = std::visit(
            Overloaded{
                [&](const node::Literal& literal) { return Combine(tag, LiteralHash(literal.value)); },
                [&](const node::Folded& folded) { return of(folded.written); },
                [&](const node::Variable& variable)
                { return Combine(tag, static_cast<std::uint64_t>(variable.variable)); },
                [&](const node::Slot& slot) { return Combine(tag, static_cast<std::uint64_t>(slot.slot)); },
                [&](const node::If& branch)
                {
                    return Combine(Combine(Combine(tag, of(branch.condition)), of(branch.thenBranch)),
                                   of(branch.elseBranch));
                },
                [&](const node::And& both) { return Combine(Combine(tag, of(both.left)), of(both.right)); },
                [&](const node::Or& either) { return Combine(Combine(tag, of(either.left)), of(either.right)); },
                [&](const node::Unary& unary)
                { return Combine(Combine(tag, static_cast<std::uint64_t>(unary.operation)), of(unary.operand)); },
                [&](const node::Binary& binary)
                {
                    return Combine(Combine(Combine(tag, static_cast<std::uint64_t>(binary.operation)), of(binary.left)),
                                   of(binary.right));
                },
                [&](const node::ExtensionCall& call)
                {
                    std::uint64_t hash = Combine(tag, call.function.Hash());
                    for (const ExpressionIndex argument : arena.Elements(call.arguments))
                    {
                        hash = Combine(hash, of(argument));
                    }
                    return hash;
                },
                [&](const node::GetAttribute& access) { return AttributeStepHash(of(access.object), access.attribute); },
                [&](const node::HasAttribute& test)
                {
                    std::uint64_t hash = Combine(tag, of(test.object));
                    for (const std::string_view attribute : arena.Elements(test.path))
                    {
                        hash = Combine(hash, TextHash(attribute));
                    }
                    return hash;
                },
                [&](const node::Like& like)
                {
                    std::uint64_t hash = Combine(tag, of(like.operand));
                    for (const std::string_view literal : arena.Elements(like.pattern))
                    {
                        hash = Combine(hash, TextHash(literal));
                    }
                    return hash;
                },
                [&](const node::Is& test) { return Combine(Combine(tag, of(test.operand)), test.type.GetName().Hash()); },
                [&](const node::SetLiteral& set)
                {
                    std::uint64_t hash = tag;
                    for (const ExpressionIndex element : arena.Elements(set.elements))
                    {
                        hash = Combine(hash, of(element));
                    }
                    return hash;
                },
                [&](const node::RecordLiteral& record)
                {
                    std::uint64_t hash = tag;
                    for (const RecordLiteralField& field : arena.Elements(record.fields))
                    {
                        hash = Combine(Combine(hash, TextHash(field.key)), of(field.value));
                    }
                    return hash;
                },
            },
            node);
    }
}

auto ExpressionShapes::Resolve(ExpressionIndex index) const noexcept -> ExpressionIndex
{
    while (const auto* folded = std::get_if<node::Folded>(&arena_.Node(index))) { index = folded->written; }
    return index;
}

auto ExpressionShapes::Hash(const ExpressionIndex base, const std::span<const std::string_view> path) const noexcept
    -> std::uint64_t
{
    std::uint64_t hash = Hash(base);
    for (const std::string_view attribute : path) { hash = AttributeStepHash(hash, attribute); }
    return hash;
}

auto ExpressionShapes::Equal(ExpressionIndex left, ExpressionIndex right) const noexcept -> bool
{
    left = Resolve(left);
    right = Resolve(right);
    if (left == right) { return true; }
    if (Hash(left) != Hash(right)) { return false; }
    const ExpressionNode& leftNode = arena_.Node(left);
    const ExpressionNode& rightNode = arena_.Node(right);
    if (leftNode.index() != rightNode.index()) { return false; }
    const auto sameElements = [this](const ArenaRange<ExpressionIndex> one, const ArenaRange<ExpressionIndex> other)
    {
        const auto first = arena_.Elements(one);
        const auto second = arena_.Elements(other);
        return std::ranges::equal(first, second, [this](const ExpressionIndex a, const ExpressionIndex b)
                                  { return Equal(a, b); });
    };
    const auto sameTexts = [this](const ArenaRange<std::string_view> one, const ArenaRange<std::string_view> other)
    { return std::ranges::equal(arena_.Elements(one), arena_.Elements(other)); };
    return std::visit(
        Overloaded{
            [&](const node::Literal& literal) { return literal.value == std::get<node::Literal>(rightNode).value; },
            [&](const node::Folded&) { return false; },
            [&](const node::Variable& variable)
            { return variable.variable == std::get<node::Variable>(rightNode).variable; },
            [&](const node::Slot& slot) { return slot.slot == std::get<node::Slot>(rightNode).slot; },
            [&](const node::If& branch)
            {
                const auto& other = std::get<node::If>(rightNode);
                return Equal(branch.condition, other.condition) && Equal(branch.thenBranch, other.thenBranch) &&
                       Equal(branch.elseBranch, other.elseBranch);
            },
            [&](const node::And& both)
            {
                const auto& other = std::get<node::And>(rightNode);
                return Equal(both.left, other.left) && Equal(both.right, other.right);
            },
            [&](const node::Or& either)
            {
                const auto& other = std::get<node::Or>(rightNode);
                return Equal(either.left, other.left) && Equal(either.right, other.right);
            },
            [&](const node::Unary& unary)
            {
                const auto& other = std::get<node::Unary>(rightNode);
                return unary.operation == other.operation && Equal(unary.operand, other.operand);
            },
            [&](const node::Binary& binary)
            {
                const auto& other = std::get<node::Binary>(rightNode);
                return binary.operation == other.operation && Equal(binary.left, other.left) &&
                       Equal(binary.right, other.right);
            },
            [&](const node::ExtensionCall& call)
            {
                const auto& other = std::get<node::ExtensionCall>(rightNode);
                return call.function == other.function && sameElements(call.arguments, other.arguments);
            },
            [&](const node::GetAttribute& access)
            {
                const auto& other = std::get<node::GetAttribute>(rightNode);
                return access.attribute == other.attribute && Equal(access.object, other.object);
            },
            [&](const node::HasAttribute& test)
            {
                const auto& other = std::get<node::HasAttribute>(rightNode);
                return sameTexts(test.path, other.path) && Equal(test.object, other.object);
            },
            [&](const node::Like& like)
            {
                const auto& other = std::get<node::Like>(rightNode);
                return sameTexts(like.pattern, other.pattern) && Equal(like.operand, other.operand);
            },
            [&](const node::Is& test)
            {
                const auto& other = std::get<node::Is>(rightNode);
                return test.type == other.type && Equal(test.operand, other.operand);
            },
            [&](const node::SetLiteral& set)
            { return sameElements(set.elements, std::get<node::SetLiteral>(rightNode).elements); },
            [&](const node::RecordLiteral& record)
            {
                const auto first = arena_.Elements(record.fields);
                const auto second = arena_.Elements(std::get<node::RecordLiteral>(rightNode).fields);
                return std::ranges::equal(first, second,
                                          [this](const RecordLiteralField& a, const RecordLiteralField& b)
                                          { return a.key == b.key && Equal(a.value, b.value); });
            },
        },
        leftNode);
}

auto ExpressionShapes::Equal(ExpressionIndex leftBase, std::span<const std::string_view> leftPath,
                             ExpressionIndex rightBase, std::span<const std::string_view> rightPath) const noexcept
    -> bool
{
    // Peel attribute steps from the ends: a path step matches a written attribute access.
    while (!leftPath.empty() || !rightPath.empty())
    {
        if (!leftPath.empty() && !rightPath.empty())
        {
            if (leftPath.back() != rightPath.back()) { return false; }
            leftPath = leftPath.first(leftPath.size() - 1);
            rightPath = rightPath.first(rightPath.size() - 1);
            continue;
        }
        auto& path = leftPath.empty() ? rightPath : leftPath;
        auto& base = leftPath.empty() ? leftBase : rightBase;
        const auto* access = std::get_if<node::GetAttribute>(&arena_.Node(Resolve(base)));
        if (access == nullptr || access->attribute != path.back()) { return false; }
        base = access->object;
        path = path.first(path.size() - 1);
    }
    return Equal(leftBase, rightBase);
}

auto Capability::ForAttribute(const ExpressionShapes& shapes, const ExpressionIndex object,
                              const std::span<const std::string_view> objectPath, const std::string_view attribute)
    -> Capability
{
    const std::uint64_t hash = Combine(Combine(shapes.Hash(object, objectPath), TextHash(attribute)), 1U);
    return Capability{CapabilityKind::Attribute, object, objectPath, attribute, ExpressionIndex{}, hash};
}

auto Capability::ForTag(const ExpressionShapes& shapes, const ExpressionIndex object, const ExpressionIndex tag)
    -> Capability
{
    const std::uint64_t hash = Combine(Combine(shapes.Hash(object), shapes.Hash(tag)), 2U);
    return Capability{CapabilityKind::Tag, object, {}, {}, tag, hash};
}

namespace
{

[[nodiscard]] auto SameCapability(const Capability& left, const Capability& right,
                                  const ExpressionShapes& shapes) noexcept -> bool
{
    if (left.hash != right.hash || left.kind != right.kind) { return false; }
    if (left.kind == CapabilityKind::Attribute)
    {
        return left.attribute == right.attribute &&
               shapes.Equal(left.object, left.objectPath, right.object, right.objectPath);
    }
    return shapes.Equal(left.object, right.object) && shapes.Equal(left.tag, right.tag);
}

} // namespace

auto CapabilitySet::Singleton(const Capability& capability) -> CapabilitySet
{
    CapabilitySet set;
    set.capabilities_.push_back(capability);
    return set;
}

auto CapabilitySet::Contains(const Capability& capability, const ExpressionShapes& shapes) const noexcept -> bool
{
    return std::ranges::any_of(capabilities_, [&](const Capability& held)
                               { return SameCapability(held, capability, shapes); });
}

auto CapabilitySet::Union(const CapabilitySet& other, const ExpressionShapes& shapes) const -> CapabilitySet
{
    if (other.IsEmpty()) { return *this; }
    if (IsEmpty()) { return other; }
    CapabilitySet result = *this;
    for (const Capability& capability : other.capabilities_)
    {
        if (!result.Contains(capability, shapes)) { result.capabilities_.push_back(capability); }
    }
    return result;
}

auto CapabilitySet::Intersect(const CapabilitySet& other, const ExpressionShapes& shapes) const -> CapabilitySet
{
    CapabilitySet result;
    for (const Capability& capability : capabilities_)
    {
        if (other.Contains(capability, shapes)) { result.capabilities_.push_back(capability); }
    }
    return result;
}

} // namespace cedar::detail
