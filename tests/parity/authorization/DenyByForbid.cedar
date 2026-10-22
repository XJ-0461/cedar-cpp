permit(principal, action, resource);
permit(principal in Group::"admins", action, resource);
forbid(principal, action, resource) when { resource.private && !context.mfa };
forbid(principal, action, resource is Photo);
