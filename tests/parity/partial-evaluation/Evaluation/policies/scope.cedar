@arm("scope-principal-eq-resource-in")
permit (principal == User::"alice", action == Action::"view", resource in Folder::"root")
when { true };

@arm("scope-principal-is-in")
permit (principal is User in Group::"staff", action in [Action::"view"], resource is Doc)
when { true };

@arm("scope-other-action")
permit (principal, action == Action::"edit", resource)
when { true };
