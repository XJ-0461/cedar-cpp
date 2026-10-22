// Distinct @ids on policies and templates.
@id("read") permit(principal, action, resource);
@id("write") permit(principal == ?principal, action, resource);
@id("delete") forbid(principal, action, resource);
