// The forbid is true after the first round, before the permit's chain resolves.
forbid (principal, action == Action::"read", resource)
when { principal.level < 0 };

permit (principal, action == Action::"read", resource)
when { resource.folder.owner.level > 5 };
