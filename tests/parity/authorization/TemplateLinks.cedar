permit(principal == ?principal, action, resource in ?resource);
forbid(principal in ?principal, action, resource) unless { context.mfa };
permit(principal, action, resource) when { resource.owner == principal };
