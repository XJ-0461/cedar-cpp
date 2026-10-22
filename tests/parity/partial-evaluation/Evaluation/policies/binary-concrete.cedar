@arm("binary-eq")
permit (principal, action, resource)
when { principal.name == "Alice" };

@arm("binary-less")
permit (principal, action, resource)
when { principal.age < 40 };

@arm("binary-less-eq")
permit (principal, action, resource)
when { principal.age <= 29 };

@arm("binary-add")
permit (principal, action, resource)
when { principal.age + 1 == 31 };

@arm("binary-sub")
permit (principal, action, resource)
when { principal.age - 1 == 29 };

@arm("binary-mul")
permit (principal, action, resource)
when { principal.age * 2 == 60 };

@arm("binary-arithmetic-overflow-error")
permit (principal, action, resource)
when { principal.age * 9223372036854775807 > 0 };

@arm("binary-in-same-uid")
permit (principal, action, resource)
when { principal in User::"alice" };

@arm("binary-in-known-ancestors-true")
permit (principal, action, resource)
when { principal in Group::"staff" };

@arm("binary-in-known-ancestors-false")
permit (principal, action, resource)
when { principal in Group::"admins" };

@arm("binary-in-set-contains-uid")
permit (principal, action, resource)
when { principal in [User::"alice", User::"bob"] };

@arm("binary-in-set-known-ancestor-hit")
permit (principal, action, resource)
when { principal in [Group::"admins", Group::"staff"] };

@arm("binary-in-set-known-ancestors-miss")
permit (principal, action, resource)
when { principal in [Group::"admins"] };

@arm("binary-get-tag-known")
permit (principal, action, resource)
when { principal.hasTag("dept") && principal.getTag("dept") == "eng" };

@arm("binary-has-tag-known-true")
permit (principal, action, resource)
when { principal.hasTag("dept") };

@arm("binary-has-tag-known-false")
permit (principal, action, resource)
when { principal.hasTag("floor") };

@arm("binary-contains")
permit (principal, action, resource)
when { principal.roles.contains("reader") };

@arm("binary-contains-all")
permit (principal, action, resource)
when { principal.roles.containsAll(["reader", "admin"]) };

@arm("binary-contains-any")
permit (principal, action, resource)
when { principal.roles.containsAny(["writer", "admin"]) };
