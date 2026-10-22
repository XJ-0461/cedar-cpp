@arm("residual-permit")
permit (principal, action, resource)
when { principal.age > 40 };

@arm("false-permit")
permit (principal, action, resource)
when { context.n == 2 };
