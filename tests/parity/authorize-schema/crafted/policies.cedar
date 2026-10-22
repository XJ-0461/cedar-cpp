@id("staff view")
permit(principal in App::Group::"staff", action == App::Action::"view", resource)
when { context.ip.isInRange(ip("10.0.0.0/8")) && principal.age >= 18 };

@id("owner edit")
permit(principal, action in App::Action::"view", resource is App::Photo)
when { resource.owner == principal && principal.hasTag("level") && principal.getTag("level") > 2 };

@id("minors")
forbid(principal is App::User, action, resource)
when { principal.age < 18 && !(context has who) };

@id("late")
forbid(principal, action == App::Action::"view", resource)
when { context has when && context.when > datetime("2030-01-01") };

@id("decimal")
permit(principal, action, resource is App::Album)
when { principal has score && principal.score.greaterThan(decimal("1.0")) && context has level && context.level > 1 };
