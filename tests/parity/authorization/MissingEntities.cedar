permit(principal in Group::"users", action, resource);
permit(principal, action, resource is Photo) when { principal has age };
permit(principal, action, resource) when { principal.age > 0 };
forbid(principal, action, resource) when { principal == User::"ghost" && resource.missing };
