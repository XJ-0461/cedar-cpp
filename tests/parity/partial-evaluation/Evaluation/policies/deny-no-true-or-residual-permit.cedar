@arm("false-permit")
permit (principal, action, resource)
when { context.n == 2 };

@arm("error-permit")
permit (principal, action, resource)
when { context.n + 9223372036854775807 > 0 };

@arm("residual-forbid")
forbid (principal, action, resource)
when { principal.age > 40 };
