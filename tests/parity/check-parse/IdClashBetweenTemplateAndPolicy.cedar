// A template and a policy renamed to the same @id.
@id("x") permit(principal == ?principal, action, resource);
@id("x") permit(principal, action, resource);
