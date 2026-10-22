@arm("residual-permit")
permit (principal, action, resource)
when { principal.age > 0 };

@arm("true-forbid")
forbid (principal, action, resource)
when { context.flag };
