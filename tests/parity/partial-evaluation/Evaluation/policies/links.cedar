@arm("template-principal-and-resource-slots")
permit (principal == ?principal, action == Action::"view", resource in ?resource)
when { principal.age > 18 };

@arm("template-principal-slot")
forbid (principal in ?principal, action, resource)
when { resource.isPublic };

@arm("static-alongside-templates")
permit (principal, action, resource)
when { context.flag };
