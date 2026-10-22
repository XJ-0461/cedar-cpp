permit(principal, action, resource) when { 1 };
permit(principal, action, resource) unless { "no" };
permit(principal, action, resource) when { principal.age };
forbid(principal, action, resource) when { context };
