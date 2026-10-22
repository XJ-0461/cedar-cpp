@id("viewer")
permit(principal in ?principal, action == Action::"view", resource == ?resource);

forbid(principal == ?principal, action, resource);

@id("static")
permit(principal, action, resource == Doc::"plan") when { principal.missing };
