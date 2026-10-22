@arm("var-action")
permit (principal, action, resource)
when { action == Action::"view" };

@arm("var-principal")
permit (principal, action, resource)
when { principal == User::"alice" };

@arm("var-resource")
permit (principal, action, resource)
when { resource == Doc::"d1" };

@arm("var-context")
permit (principal, action, resource)
when { context.flag };
