// Distinct @ids, swapped with the positional ids.
@id("policy1") permit(principal, action, resource);
@id("policy0") permit(principal == ?principal, action, resource);
