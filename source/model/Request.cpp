#include <cedar/Request.hpp>

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cedar
{

Context::Context() noexcept : value_{nullptr, Value::FromRecord(Record{})} {}

Context::Context(const Record record, std::shared_ptr<const ValueArena> storage) noexcept
    : value_{std::move(storage), Value::FromRecord(record)}
{
}

auto Context::FromValue(OwnedValue value) -> std::expected<Context, ContextError>
{
    if (value.Get().Kind() != ValueKind::Record)
    {
        return std::unexpected{ContextError{Diagnostic{
            .code = DiagnosticCode::ContextNotARecord,
            .message = std::format("the context must be a record, not `{}`", ToCedar(value.Get())),
        }}};
    }
    return Context{std::move(value)};
}

auto Context::FromPairs(const std::span<const RecordEntry> pairs) -> std::expected<Context, ContextError>
{
    auto storage = std::make_shared<ValueArena>();
    std::vector<RecordEntry> copied;
    copied.reserve(pairs.size());
    for (const RecordEntry& pair : pairs)
    {
        copied.push_back(RecordEntry{storage->CopyText(pair.key), storage->DeepCopy(pair.value)});
    }
    auto record = storage->MakeRecord(copied);
    if (!record)
    {
        return std::unexpected{ContextError{Diagnostic{
            .code = DiagnosticCode::DuplicateRecordKey,
            .message = std::format("the context has the key `{}` more than once", std::string{record.error().key}),
        }}};
    }
    return Context{OwnedValue{std::move(storage), *record}};
}

} // namespace cedar
