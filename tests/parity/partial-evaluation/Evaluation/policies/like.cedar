@arm("like-concrete")
permit (principal, action, resource)
when { context.s like "he*o" };

@arm("like-concrete-escaped-star")
permit (principal, action, resource)
when { context.s like "h\*llo" };

@arm("like-residual")
permit (principal, action, resource)
when { principal.name like "A*\*" };

@arm("like-error")
permit (principal, action, resource)
when { (if context.n + 9223372036854775807 > 0 then "a" else "b") like "a" };
