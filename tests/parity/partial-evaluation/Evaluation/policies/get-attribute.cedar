@arm("get-attribute-record-present")
permit (principal, action, resource)
when { context.rec.a == 1 };

@arm("get-attribute-entity-known-present")
permit (principal, action, resource)
when { resource.owner.age == 30 };

@arm("get-attribute-entity-attributes-unknown")
permit (principal, action, resource)
when { principal.age == 50 };

@arm("get-attribute-residual")
permit (principal, action, resource)
when { principal.profile.level == 5 };

@arm("get-attribute-error")
permit (principal, action, resource)
when { (if context.n + 9223372036854775807 > 0 then principal else principal).age == 1 };
