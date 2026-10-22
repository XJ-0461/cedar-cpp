@arm("can-error-binary-eq-var-concrete-false")
permit (principal, action, resource)
when { principal == User::"alice" && context.n == 2 };

@arm("can-error-and-false")
permit (principal, action, resource)
when { (principal == User::"alice" && resource == Doc::"d1") && context.n == 2 };

@arm("can-error-or-false")
permit (principal, action, resource)
when { (principal == User::"alice" || resource == Doc::"d1") && context.n == 2 };

@arm("can-error-if-false")
permit (principal, action, resource)
when { (if principal == User::"alice" then resource == Doc::"d1" else false) && context.n == 2 };

@arm("can-error-is-true")
permit (principal, action, resource)
when { (resource.owner is User) && context.n == 2 };

@arm("can-error-like-false")
permit (principal, action, resource)
when { ((if principal == User::"alice" then "a" else "b") like "a*") && context.n == 2 };

@arm("can-error-like-true")
permit (principal, action, resource)
when { (principal.name like "a*") && context.n == 2 };

@arm("can-error-add-true")
permit (principal, action, resource)
when { (if principal == User::"alice" then 1 else 2) + 1 == 2 && context.n == 2 };

@arm("can-error-sub-true")
permit (principal, action, resource)
when { (if principal == User::"alice" then 1 else 2) - 1 == 0 && context.n == 2 };

@arm("can-error-mul-true")
permit (principal, action, resource)
when { (if principal == User::"alice" then 1 else 2) * 2 == 2 && context.n == 2 };

@arm("can-error-get-tag-true")
permit (principal, action, resource)
when { (principal.hasTag("dept") && principal.getTag("dept") == "eng") && context.n == 2 };

@arm("can-error-has-tag-false")
permit (principal, action, resource)
when { principal.hasTag("dept") && context.n == 2 };

@arm("can-error-in-false")
permit (principal, action, resource)
when { principal in Group::"staff" && context.n == 2 };

@arm("can-error-less-false")
permit (principal, action, resource)
when { (if principal == User::"alice" then 1 else 2) < 2 && context.n == 2 };

@arm("can-error-less-eq-false")
permit (principal, action, resource)
when { (if principal == User::"alice" then 1 else 2) <= 2 && context.n == 2 };

@arm("can-error-contains-set-false")
permit (principal, action, resource)
when { [principal].contains(User::"alice") && context.n == 2 };

@arm("can-error-contains-all-false")
permit (principal, action, resource)
when { [principal].containsAll([User::"alice"]) && context.n == 2 };

@arm("can-error-contains-any-false")
permit (principal, action, resource)
when { [principal].containsAny([User::"alice"]) && context.n == 2 };

@arm("can-error-extension-function-true")
permit (principal, action, resource)
when { (if principal == User::"alice" then ip("1.1.1.1") else ip("2.2.2.2")).isLoopback() && context.n == 2 };

@arm("can-error-get-attribute-true")
permit (principal, action, resource)
when { principal.age > 0 && context.n == 2 };

@arm("can-error-has-attribute-false")
permit (principal, action, resource)
when { principal has manager && context.n == 2 };

@arm("can-error-extension-has-attribute-false")
permit (principal, action, resource)
when { principal has profile.nick && context.n == 2 };

@arm("can-error-neg-true")
permit (principal, action, resource)
when { -(if principal == User::"alice" then 1 else 2) == -1 && context.n == 2 };

@arm("can-error-not-false")
permit (principal, action, resource)
when { !(principal == User::"alice") && context.n == 2 };

@arm("can-error-is-empty-false")
permit (principal, action, resource)
when { [principal].isEmpty() && context.n == 2 };

@arm("can-error-record-false")
permit (principal, action, resource)
when { {a: principal} == {a: User::"alice"} && context.n == 2 };

@arm("can-error-record-true")
permit (principal, action, resource)
when { {a: principal.age} == {a: 1} && context.n == 2 };

@arm("can-error-set-true")
permit (principal, action, resource)
when { [principal.age].contains(1) && context.n == 2 };

@arm("can-error-error-true")
permit (principal, action, resource)
when { (principal == User::"alice" || context.n + 9223372036854775807 > 0) && context.n == 2 };

@arm("can-error-or-right-true-error-free")
forbid (principal, action, resource)
when { principal in Group::"staff" || context.n == 1 };
