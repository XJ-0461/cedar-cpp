@arm("or-left-true")
permit (principal, action, resource)
when { context.n == 1 || principal.age > 0 };

@arm("or-left-false")
permit (principal, action, resource)
when { context.n == 2 || principal.age > 0 };

@arm("or-left-error")
permit (principal, action, resource)
when { context.n + 9223372036854775807 > 0 || principal.age > 0 };

@arm("or-left-residual-right-false")
permit (principal, action, resource)
when { principal.age > 0 || context.n == 2 };

@arm("or-left-residual-right-true-error-free")
permit (principal, action, resource)
when { principal == User::"alice" || context.n == 1 };

@arm("or-left-residual-right-true-can-error")
permit (principal, action, resource)
when { principal.age > 0 || context.n == 1 };

@arm("or-left-residual-right-residual")
permit (principal, action, resource)
when { principal.age > 0 || resource.isPublic };

@arm("or-left-residual-right-error")
permit (principal, action, resource)
when { principal.age > 0 || context.n + 9223372036854775807 > 0 };
