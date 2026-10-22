@id("view")
permit(principal == User::"alice", action == Action::"view", resource);

@id("broken")
forbid(principal, action, resource) when { resource.missing };
