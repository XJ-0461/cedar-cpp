#pragma once

#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace cedar::detail
{

/// The Levenshtein distance between two strings, counted in Unicode scalar values (Rust's
/// `chars()`): insertions, deletions and substitutions each cost one.
[[nodiscard]] auto LevenshteinDistance(std::string_view first, std::string_view second) -> std::size_t;

/// The word of `words` nearest to `key` by Levenshtein distance; the first of equally near words
/// wins (Rust's `fuzzy_search_limited`, `cedar-policy-core/src/fuzzy_match.rs`). Nothing when `key`
/// or `words` is empty, or when `maxDistance` is given and the nearest word is farther than it.
/// Without `maxDistance` any nearest word is suggested however far it is.
template <std::ranges::input_range Words>
[[nodiscard]] auto FuzzySearch(const std::string_view key, Words&& words,
                               const std::optional<std::size_t> maxDistance = std::nullopt)
    -> std::optional<std::string>
{
    if (key.empty()) { return std::nullopt; }
    std::optional<std::string_view> best;
    std::size_t bestDistance = 0;
    for (const std::string_view word : words)
    {
        const std::size_t distance = LevenshteinDistance(key, word);
        if (!best || distance < bestDistance)
        {
            best = word;
            bestDistance = distance;
        }
    }
    if (!best) { return std::nullopt; }
    if (maxDistance && bestDistance > *maxDistance) { return std::nullopt; }
    return std::string{*best};
}

} // namespace cedar::detail
