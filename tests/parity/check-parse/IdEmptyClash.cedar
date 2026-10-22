// An @id without a value and an empty @id both rename to the empty id.
@id permit(principal, action, resource);
@id("") permit(principal, action, resource);
