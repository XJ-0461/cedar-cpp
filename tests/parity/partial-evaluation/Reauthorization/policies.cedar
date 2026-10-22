// policy0: anyone may view a public document.
permit (
  principal,
  action == Action::"View",
  resource is Document
)
when { resource.isPublic };

// policy1: an owner may view a document with MFA.
permit (
  principal,
  action == Action::"View",
  resource is Document
)
when { context.hasMFA && resource.owner == principal };

// policy2: a user with a negative level is denied everything.
forbid (
  principal is User,
  action,
  resource
)
when { principal.level < 0 };

// policy3: overflows, and so errors, for a user above level 1000.
permit (
  principal is User,
  action,
  resource
)
when { principal.level > 1000 && principal.level + 9223372036854775807 > 0 };

// policy4: an administrator may do anything.
permit (
  principal is Admin,
  action,
  resource
);
