@arm("true-permit")
permit (principal, action, resource)
when { context.flag };

@arm("error-permit")
permit (principal, action, resource)
when { context.n + 9223372036854775807 > 0 };

@arm("false-forbid")
forbid (principal, action, resource)
when { principal in resource };
