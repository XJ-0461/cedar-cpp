@arm("set-all-concrete")
permit (principal, action, resource)
when { [context.n, 2].contains(1) };

@arm("set-error-element")
permit (principal, action, resource)
when { [principal.age, context.n + 9223372036854775807].contains(1) };

@arm("set-residual")
permit (principal, action, resource)
when { [principal.age, context.n].contains(30) };

@arm("record-all-concrete")
permit (principal, action, resource)
when { {a: context.n, b: context.s} == {a: 1, b: "hello"} };

@arm("record-error-field")
permit (principal, action, resource)
when { {a: principal.age, b: context.n + 9223372036854775807} == {a: 1, b: 2} };

@arm("record-residual")
permit (principal, action, resource)
when { {a: principal.age, b: context.n} == {a: 30, b: 1} };
