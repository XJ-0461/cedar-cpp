@id("owner-template")
permit (principal == ?principal, action == Action::"edit", resource in ?resource)
when { principal.level > 0 };

@id("static")
permit (principal, action == Action::"edit", resource is Photo)
when { resource.isPublic };
