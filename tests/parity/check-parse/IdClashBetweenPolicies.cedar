// Two policies renamed to the same @id.
@id("a") permit(principal, action, resource);
@id("a") forbid(principal, action, resource);
