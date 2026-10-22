// Three rounds: the document, then its folder, then the folder's owner.
permit (principal, action == Action::"read", resource)
when { resource.folder.owner.level > 5 };
