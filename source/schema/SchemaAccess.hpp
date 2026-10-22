#pragma once

#include <cedar/Schema.hpp>

#include <memory>
#include <utility>

namespace cedar::detail
{

class SchemaFragmentModel;
class ValidatorSchema;

/// Makes and opens the public schema handles (`SchemaFragment` over SchemaFragmentModel,
/// `Schema` over ValidatorSchema). Neither model needs to be complete here.
struct SchemaAccess
{
    [[nodiscard]] static auto MakeFragment(std::shared_ptr<const SchemaFragmentModel> model) noexcept -> SchemaFragment
    {
        return SchemaFragment{std::move(model)};
    }

    [[nodiscard]] static auto Model(const SchemaFragment& fragment) noexcept -> const SchemaFragmentModel&
    {
        return *fragment.model_;
    }

    [[nodiscard]] static auto SharedModel(const SchemaFragment& fragment) noexcept
        -> const std::shared_ptr<const SchemaFragmentModel>&
    {
        return fragment.model_;
    }

    [[nodiscard]] static auto MakeSchema(std::shared_ptr<const ValidatorSchema> schema) noexcept -> Schema
    {
        return Schema{std::move(schema)};
    }

    [[nodiscard]] static auto Validator(const Schema& schema) noexcept -> const ValidatorSchema&
    {
        return *schema.schema_;
    }
};

} // namespace cedar::detail
