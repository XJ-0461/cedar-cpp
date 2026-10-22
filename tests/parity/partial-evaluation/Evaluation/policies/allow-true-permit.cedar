@arm("true-permit")
permit (principal, action, resource)
when { context.flag };

@arm("false-forbid")
forbid (principal, action, resource)
when { context.n == 2 };

@arm("error-forbid")
forbid (principal, action, resource)
when { context.n + 9223372036854775807 > 0 };
