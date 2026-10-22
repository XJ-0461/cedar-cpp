@arm("if-test-true")
permit (principal, action, resource)
when { if context.n == 1 then principal.age > 0 else resource.isPublic };

@arm("if-test-false")
permit (principal, action, resource)
when { if context.n == 2 then principal.age > 0 else resource.isPublic };

@arm("if-test-residual")
permit (principal, action, resource)
when { if principal.age > 0 then context.n == 1 else resource.isPublic };

@arm("if-test-error")
permit (principal, action, resource)
when { if context.n + 9223372036854775807 > 0 then principal.age > 0 else resource.isPublic };
