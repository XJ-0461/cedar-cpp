@arm("normalize-decimal-literal")
permit (principal, action, resource)
when { principal.balance.lessThan(decimal("1.50")) };

@arm("normalize-ipaddr-literal")
permit (principal, action, resource)
when { principal.ip == ip("10.0.0.1") };

@arm("normalize-ipaddr-range-literal")
permit (principal, action, resource)
when { principal.ip.isInRange(ip("10.0.0.0/8")) };

@arm("normalize-ipv6-literal")
permit (principal, action, resource)
when { principal.ip == ip("0:0:0:0:0:0:0:1") };

@arm("normalize-datetime-literal")
permit (principal, action, resource)
when { principal.joined < datetime("2024-01-01") };

@arm("normalize-duration-literal")
permit (principal, action, resource)
when { principal.session == duration("1h30m") };

@arm("normalize-context-values")
permit (principal, action, resource)
when { principal.balance.lessThan(context.amount) && principal.ip.isInRange(context.addr) };

@arm("normalize-computed-datetime")
permit (principal, action, resource)
when { principal.joined < context.at.offset(context.span) };

@arm("normalize-computed-duration")
permit (principal, action, resource)
when { principal.session < datetime("2024-01-02").durationSince(datetime("2024-01-01T12:00:00Z")) };

@arm("normalize-in-record")
permit (principal, action, resource)
when { principal.limits == {max: decimal("2.0")} };

@arm("normalize-in-set")
permit (principal, action, resource)
when { [ip("1.1.1.1"), ip("::1")].contains(principal.ip) };

@arm("normalize-to-date")
permit (principal, action, resource)
when { principal.joined.toDate() == datetime("2024-01-01T23:59:59.999Z").toDate() };
