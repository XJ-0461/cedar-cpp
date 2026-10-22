forbid(principal, action, resource) when { true };
permit(principal, action, resource);
forbid(principal in Group::"users", action, resource);
forbid(principal, action, resource) when { principal.nope };
forbid(principal, action in Action::"read", resource in Album::"trips");
permit(principal, action, resource) when { resource.owner == principal };
