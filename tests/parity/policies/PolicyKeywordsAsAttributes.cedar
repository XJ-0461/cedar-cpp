permit(principal, action, resource) when { principal.permit == context.when && resource has unless };
