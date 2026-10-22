@arm("extension-concrete-success")
permit (principal, action, resource)
when { ip("127.0.0.1").isLoopback() };

@arm("extension-concrete-call-fails")
permit (principal, action, resource)
when { context.at.offset(duration("9223372036854775807ms")) == context.at };

@arm("extension-error-argument")
permit (principal, action, resource)
when { context.at.offset(duration("9223372036854775807ms")).durationSince(principal.joined) == duration("1h") };

@arm("extension-residual")
permit (principal, action, resource)
when { principal.ip.isInRange(context.addr) };
