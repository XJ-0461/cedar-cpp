permit(principal, action, resource);
forbid(principal, action, resource) when { principal.department.isEmpty() };
forbid(principal, action, resource) when { 9223372036854775807 + principal.age > 0 };
