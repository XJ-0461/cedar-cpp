@id("public-view")
permit (principal, action == Action::"view", resource is Photo)
when { resource.isPublic };

@id("owner")
permit (principal, action in [Action::"view", Action::"edit"], resource is Photo)
when { resource.owner == principal };

@id("mfa")
forbid (principal, action == Action::"view", resource)
unless { context.mfa };

@id("admins")
permit (principal in Group::"admins", action, resource);

@id("tagged")
permit (principal, action == Action::"edit", resource)
when { principal.level > 3 && resource.hasTag("x") && resource.getTag("x") == "y" };

@id("draft")
permit (principal, action == Action::"publish", resource == Status::"draft");

@id("overflow")
permit (principal, action == Action::"view", resource is Photo)
when { principal.level + 9223372036854775807 > 0 };

@id("network")
permit (principal, action == Action::"view", resource in Album::"a1")
when { context has ip && context.ip.isInRange(ip("10.0.0.0/8")) && "eng" like "e*" };
