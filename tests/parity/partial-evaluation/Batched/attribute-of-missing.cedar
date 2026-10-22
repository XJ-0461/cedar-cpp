// Reads an attribute of the principal, which may be missing from the store.
permit (principal, action == Action::"read", resource)
when { principal.level > 5 };
