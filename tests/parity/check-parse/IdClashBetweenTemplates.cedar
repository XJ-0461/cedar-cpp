// A template renamed to the positional id of a later template.
@id("policy1") permit(principal == ?principal, action, resource);
permit(principal, action, resource == ?resource);
