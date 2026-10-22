@arm("unary-not-concrete")
permit (principal, action, resource)
when { !context.flag };

@arm("unary-neg-concrete")
permit (principal, action, resource)
when { -context.n == -1 };

@arm("unary-is-empty-concrete")
permit (principal, action, resource)
when { context.words.isEmpty() };

@arm("unary-neg-overflow-error")
permit (principal, action, resource)
when { -(context.n - 9223372036854775807 - 2) == 0 };

@arm("unary-residual")
permit (principal, action, resource)
when { !(principal.age > 0) };

@arm("unary-is-empty-residual")
permit (principal, action, resource)
when { principal.roles.isEmpty() };

@arm("unary-error-operand")
permit (principal, action, resource)
when { !(context.n + 9223372036854775807 > 0) };
