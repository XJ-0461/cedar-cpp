forbid(principal, action, resource) unless { context has mfa && context.mfa == true };
