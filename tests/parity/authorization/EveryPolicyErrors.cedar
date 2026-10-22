permit(principal, action, resource) when { principal.age > 18 };
forbid(principal, action, resource) when { principal.age < 18 };
permit(principal, action, resource) when { ip(context.mfa).isLoopback() };
