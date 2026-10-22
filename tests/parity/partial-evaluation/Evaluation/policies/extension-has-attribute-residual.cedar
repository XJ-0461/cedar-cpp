@arm("extension-has-residual-try-decide-none")
permit (principal, action, resource)
when { principal has profile.nick };

@arm("extension-has-residual-try-decide-false")
permit (principal, action, resource)
when { principal has nonexistent.x };

@arm("extension-has-record-missing")
permit (principal, action, resource)
when { context has rec.b };
