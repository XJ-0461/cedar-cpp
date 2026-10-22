@arm("error-whole-policy-permit")
permit (principal, action, resource)
when { context.n + 9223372036854775807 > 0 };

@arm("error-whole-policy-forbid")
forbid (principal, action, resource)
when { context.at.offset(duration("9223372036854775807ms")) == context.at };

@arm("error-inside-residual-and")
permit (principal, action, resource)
when { principal.age > 0 && context.n + 9223372036854775807 > 0 };

@arm("error-inside-residual-or")
permit (principal, action, resource)
when { principal.age > 0 || context.n + 9223372036854775807 > 0 };

@arm("error-inside-residual-if-branch")
permit (principal, action, resource)
when { if principal.age > 0 then context.n + 9223372036854775807 > 0 else true };
