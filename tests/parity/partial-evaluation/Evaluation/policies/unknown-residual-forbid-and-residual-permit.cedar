@arm("residual-permit")
permit (principal, action, resource)
when { principal.age < 40 };

@arm("residual-forbid")
forbid (principal, action, resource)
when { principal in Group::"admins" };
