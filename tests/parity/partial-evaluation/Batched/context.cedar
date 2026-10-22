// Decided from the concrete context alone, before any round.
permit (principal, action == Action::"read", resource)
when { context.mfa };

forbid (principal, action == Action::"read", resource)
unless { context.mfa };
