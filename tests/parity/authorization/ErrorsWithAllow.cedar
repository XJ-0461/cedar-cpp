permit(principal, action, resource) when { principal.missing };
permit(principal, action, resource) when { principal.age > 18 };
permit(principal, action, resource) when { context.nothing == 1 };
permit(principal, action, resource) when { principal.age + "one" == 2 };
