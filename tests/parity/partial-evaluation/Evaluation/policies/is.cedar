@arm("is-decide-false-type-not-in-lub")
permit (principal, action, resource)
when { principal is Admin };

@arm("is-decide-true-single-type")
permit (principal, action, resource)
when { principal is User };

@arm("is-decide-none-can-error-residual")
permit (principal, action, resource)
when { resource.owner is User };

@arm("is-error")
permit (principal, action, resource)
when { (if context.n + 9223372036854775807 > 0 then principal else principal) is User };
