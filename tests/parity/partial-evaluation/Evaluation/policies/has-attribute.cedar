@arm("has-attribute-record-present")
permit (principal, action, resource)
when { context has opt };

@arm("has-attribute-record-absent")
permit (principal, action, resource)
when { context.rec has b };

@arm("has-attribute-entity-known-true")
permit (principal, action, resource)
when { resource.owner has manager };

@arm("has-attribute-entity-attributes-unknown")
permit (principal, action, resource)
when { principal has manager };

@arm("has-attribute-error")
permit (principal, action, resource)
when { (if context.n + 9223372036854775807 > 0 then principal else principal) has manager };
