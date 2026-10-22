#pragma once

#include "evaluation/ScopeIndex.hpp"

#include <cedar/PolicySet.hpp>

#include <mutex>

namespace cedar::detail
{

/// The PolicySet operations only the library's readers use.
struct PolicySetAccess
{
    /// Rust's `PolicySet::from_est` lists only the templates with slots; the others stay in the
    /// set for their links and to keep their ids taken.
    static auto HideSlotFreeTemplates(PolicySet& set) -> void { set.HideSlotFreeTemplates(); }

    /// The scope index of `set`'s policies, building it on the first call; safe to call from any
    /// number of threads on a set nobody is changing. Null for a moved-from set.
    static auto GetScopeIndex(const PolicySet& set) -> const ScopeIndex*
    {
        ScopeIndexCache* const cache = set.scopeIndex_.get();
        if (cache == nullptr) { return nullptr; }
        std::call_once(cache->built, [&] { cache->index.emplace(set.policies_); });
        return &*cache->index;
    }
};

} // namespace cedar::detail
