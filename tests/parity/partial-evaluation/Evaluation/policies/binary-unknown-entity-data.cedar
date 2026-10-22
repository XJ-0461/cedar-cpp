@arm("binary-in-unknown-ancestors-residual")
permit (principal, action, resource)
when { principal in Group::"staff" };

@arm("binary-in-same-uid-unknown-ancestors")
permit (principal, action, resource)
when { principal in User::"alice" };

@arm("binary-in-set-unknown-ancestors-residual")
permit (principal, action, resource)
when { principal in [Group::"admins", Group::"staff"] };

@arm("binary-in-set-contains-uid-unknown-ancestors")
permit (principal, action, resource)
when { principal in [User::"alice"] };

@arm("binary-in-empty-set-unknown-ancestors-false")
permit (principal, action, resource)
when { principal in context.groups };

@arm("binary-get-tag-unknown-tags-residual")
permit (principal, action, resource)
when { principal.hasTag("dept") && principal.getTag("dept") == "eng" };

@arm("binary-has-tag-unknown-tags-residual")
permit (principal, action, resource)
when { principal.hasTag("dept") };
