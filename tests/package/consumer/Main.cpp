#include <cedar/Cedar.hpp>

#include <cstdio>

auto main() -> int
{
    const auto uid = cedar::EntityUid::Create("Photos::User", "alice");
    if (!uid || uid->ToString() != "Photos::User::\"alice\"")
    {
        std::fputs("cedar::EntityUid::Create gave the wrong uid\n", stderr);
        return 1;
    }
    std::puts(uid->ToString().c_str());
    return 0;
}
