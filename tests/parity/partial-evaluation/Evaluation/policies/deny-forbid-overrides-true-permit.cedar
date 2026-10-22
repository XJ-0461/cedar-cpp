@arm("true-permit")
permit (principal, action, resource)
when { context.flag };

@arm("true-forbid")
forbid (principal, action, resource)
when { context.n == 1 };

@arm("residual-permit")
permit (principal, action, resource)
when { principal.age > 0 };
