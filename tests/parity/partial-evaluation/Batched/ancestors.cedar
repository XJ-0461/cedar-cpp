// The resource's folder is tested against a literal folder, and the principal against a group.
permit (principal in Group::"admins", action == Action::"read", resource)
when { resource.folder in Folder::"root" };
