#include "model/FuzzyMatch.hpp"

#include "syntax/Utf8.hpp"

#include <algorithm>
#include <numeric>
#include <vector>

namespace cedar::detail
{

namespace
{

// The scalar values of `text`; a byte that is not well-formed UTF-8 counts as one scalar of its own.
auto Scalars(const std::string_view text) -> std::vector<char32_t>
{
    std::vector<char32_t> scalars;
    scalars.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();)
    {
        const DecodedScalar decoded = DecodeUtf8(text, offset);
        if (decoded.length == 0)
        {
            scalars.push_back(static_cast<char32_t>(static_cast<unsigned char>(text[offset])));
            ++offset;
        }
        else
        {
            scalars.push_back(decoded.scalar);
            offset += decoded.length;
        }
    }
    return scalars;
}

} // namespace

auto LevenshteinDistance(const std::string_view first, const std::string_view second) -> std::size_t
{
    const std::vector<char32_t> left = Scalars(first);
    const std::vector<char32_t> right = Scalars(second);
    // One row of the distance matrix at a time: row[i] is the distance between the first i scalars
    // of `left` and the scalars of `right` consumed so far.
    std::vector<std::size_t> row(left.size() + 1);
    std::iota(row.begin(), row.end(), std::size_t{0});
    for (std::size_t j = 1; j <= right.size(); ++j)
    {
        std::size_t diagonal = row[0];
        row[0] = j;
        for (std::size_t i = 1; i <= left.size(); ++i)
        {
            const std::size_t above = row[i];
            row[i] = left[i - 1] == right[j - 1] ? diagonal : 1 + std::min({row[i - 1], above, diagonal});
            diagonal = above;
        }
    }
    return row[left.size()];
}

} // namespace cedar::detail
