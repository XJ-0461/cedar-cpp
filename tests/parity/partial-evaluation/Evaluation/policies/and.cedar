@arm("and-left-false")
permit (principal, action, resource)
when { context.n == 2 && principal.age > 0 };

@arm("and-left-true")
permit (principal, action, resource)
when { context.n == 1 && principal.age > 0 };

@arm("and-left-error")
permit (principal, action, resource)
when { context.n + 9223372036854775807 > 0 && principal.age > 0 };

@arm("and-left-residual-right-true")
permit (principal, action, resource)
when { principal.age > 0 && context.n == 1 };

@arm("and-left-residual-right-false-error-free")
permit (principal, action, resource)
when { principal == User::"alice" && context.n == 2 };

@arm("and-left-residual-right-false-can-error")
permit (principal, action, resource)
when { principal.age > 0 && context.n == 2 };

@arm("and-left-residual-right-residual")
permit (principal, action, resource)
when { principal.age > 0 && resource.isPublic };

@arm("and-left-residual-right-error")
permit (principal, action, resource)
when { principal.age > 0 && context.n + 9223372036854775807 > 0 };
