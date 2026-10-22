@arm("true-permit")
permit (principal, action, resource)
when { context.flag };

@arm("residual-forbid")
forbid (principal, action, resource)
when { principal.age > 40 };
