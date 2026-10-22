@id("staff may view with mfa")
permit(principal in Group::"staff", action == Action::"view", resource)
when { context.mfa == true };

@id("owner")
permit(principal, action, resource is Photo)
when { resource.owner == principal && principal.level > context.minimumLevel };

forbid(principal, action, resource)
when { context has blocked && context.blocked };
