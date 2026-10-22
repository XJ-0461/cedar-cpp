// policy0
permit (principal, action == Action::"view", resource)
when { resource.isPublic };

// policy1
permit (principal, action in [Action::"view", Action::"edit"], resource)
when { resource.owner == principal };

// policy2
forbid (principal, action == Action::"edit", resource)
when { principal.level < 0 };

// policy3
forbid (principal, action == Action::"edit", resource)
when { resource.locked };

// policy4
permit (principal == User::"alice", action == Action::"view", resource);

// policy5
permit (principal is Admin, action == Action::"purge", resource)
unless { resource.locked };

// policy6: a template, linked by some cases.
permit (principal == ?principal, action == Action::"share", resource == ?resource);
