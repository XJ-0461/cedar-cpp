forbid(principal, action, resource);
permit(principal, action, resource) when { principal.level > 5 };
