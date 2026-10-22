permit (principal, action == Action::"view", resource is Photo)
when { resource.isPublic + 1 > 0 };
