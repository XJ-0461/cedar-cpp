@arm("extension-has-record-all-present")
permit (principal, action, resource)
when { context has rec.b };

@arm("extension-has-entity-known-all-present")
permit (principal, action, resource)
when { principal has profile.nick };

@arm("extension-has-entity-known-missing")
permit (principal, action, resource)
when { resource.owner has nonexistent.x };

@arm("extension-has-entity-attributes-unknown-mid-chain-residual")
permit (principal, action, resource)
when { principal has manager.manager };

@arm("extension-has-entity-attributes-unknown-mid-chain-decided-false")
permit (principal, action, resource)
when { principal has manager.nonexistent };

@arm("extension-has-error")
permit (principal, action, resource)
when { (if context.n + 9223372036854775807 > 0 then principal else principal) has profile.nick };
