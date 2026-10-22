@arm("reshape-and-left-typed-false")
permit (principal, action, resource)
when { (principal.age > 0 && false) && ("a" < 1) };

@arm("reshape-or-left-typed-true")
permit (principal, action, resource)
when { (principal.age > 0 || true) || ("a" < 1) };

@arm("reshape-if-test-typed-true")
permit (principal, action, resource)
when { if (principal.age > 0 || true) then resource.isPublic else principal.age > 1 };

@arm("reshape-if-test-typed-false")
permit (principal, action, resource)
when { if (principal.age > 0 && false) then resource.isPublic else principal.age > 1 };

@arm("reshape-and-literal-false")
permit (principal, action, resource)
when { false && ("a" < 1) };
