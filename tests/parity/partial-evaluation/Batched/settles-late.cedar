// The permit is true after the first round, but the forbid keeps the decision open to the last round.
permit (principal, action == Action::"read", resource)
when { principal.level > 5 };

forbid (principal, action == Action::"read", resource)
when { resource.folder.owner.level < 0 };
