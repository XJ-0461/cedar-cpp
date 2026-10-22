permit(principal == User::"alice", action, resource);
permit(principal, action == Action::"edit", resource);
forbid(principal, action, resource) when { principal.age > 100 };
