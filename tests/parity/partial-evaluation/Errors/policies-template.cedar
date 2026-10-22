permit (principal, action == Action::"view", resource)
when { principal.age > context.n };

@id("template")
permit (principal == ?principal, action, resource in ?resource);
