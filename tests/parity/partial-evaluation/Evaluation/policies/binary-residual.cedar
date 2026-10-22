@arm("binary-error-left")
permit (principal, action, resource)
when { context.n + 9223372036854775807 == principal.age };

@arm("binary-error-right")
permit (principal, action, resource)
when { principal.age == context.n + 9223372036854775807 };

@arm("binary-residual-both-partial")
permit (principal, action, resource)
when { principal.age == resource.owner.age };

@arm("binary-residual-left-partial")
permit (principal, action, resource)
when { principal.age < 40 };

@arm("binary-residual-right-partial")
permit (principal, action, resource)
when { 40 <= principal.age };

@arm("binary-residual-contains")
permit (principal, action, resource)
when { principal.roles.contains(context.s) };
