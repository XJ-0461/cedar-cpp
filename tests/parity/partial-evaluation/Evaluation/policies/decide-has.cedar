@arm("decide-has-false-undeclared-attribute")
permit (principal, action, resource)
when { resource has nonexistent };

@arm("decide-has-none-may-have-attribute-residual")
permit (principal, action, resource)
when { principal has manager };

@arm("decide-has-none-can-error-residual")
permit (principal, action, resource)
when { resource.owner has manager };

@arm("decide-has-none-record")
permit (principal, action, resource)
when { context has opt };
