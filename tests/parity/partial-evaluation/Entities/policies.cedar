permit (principal, action == Action::"view", resource)
when { principal.name == context.name && principal.age == context.age };

permit (principal, action == Action::"view", resource)
when { principal.address == context.address && principal.names == context.names };

permit (principal, action == Action::"view", resource)
when { principal has friends && principal.friends == context.friends };

permit (principal, action == Action::"view", resource)
when { principal has manager && principal.manager == context.manager };

permit (principal, action == Action::"view", resource)
when { principal has favorite && principal.favorite == context.favorite };

permit (principal, action == Action::"view", resource)
when { principal has ip && principal.ip == context.ip };

permit (principal, action == Action::"view", resource)
when { principal has score && principal.score == context.score };

permit (principal, action == Action::"view", resource)
when { principal has since && principal has wait && principal.since == context.since && principal.wait == context.wait };

permit (principal in Group::"root", action == Action::"view", resource);

permit (principal, action == Action::"view", resource)
when { resource.hasTag("k") && resource.getTag("k") == context.tag };

forbid (principal, action == Action::"view", resource)
when { resource.owner != principal && principal in Group::"blocked" };
