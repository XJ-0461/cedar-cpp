permit (principal, action == Action::"view", resource)
when { principal.age > context.n };

forbid (principal, action, resource)
when { resource.age > 1 };
