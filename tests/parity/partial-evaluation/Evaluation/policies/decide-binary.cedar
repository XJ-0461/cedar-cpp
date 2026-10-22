@arm("decide-in-false-not-descendant")
permit (principal, action, resource)
when { principal in resource };

@arm("decide-in-none-descendant-possible")
permit (principal, action, resource)
when { resource in Folder::"root" };

@arm("decide-in-none-set-descendant-possible")
permit (principal, action, resource)
when { principal in context.groups };

@arm("decide-in-none-can-error")
permit (principal, action, resource)
when { resource.owner in Group::"staff" };

@arm("decide-in-false-set-not-descendant")
permit (principal, action, resource)
when { resource in [Group::"staff"] };

@arm("decide-eq-false-disjoint-types")
permit (principal, action, resource)
when { principal == Doc::"d1" };

@arm("decide-eq-none-overlapping-types")
permit (principal, action, resource)
when { principal == User::"alice" };

@arm("decide-eq-none-can-error")
permit (principal, action, resource)
when { resource.owner == Doc::"d1" };

@arm("decide-has-tag-false-no-tags")
permit (principal, action, resource)
when { resource.hasTag("x") };

@arm("decide-has-tag-none-may-have-tags")
permit (principal, action, resource)
when { principal.hasTag("x") };

@arm("decide-has-tag-none-can-error")
permit (principal, action, resource)
when { resource.owner.hasTag("x") };

@arm("decide-other-op-none")
permit (principal, action, resource)
when { principal.age < 40 };
