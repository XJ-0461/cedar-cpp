// Two rounds: the principal, then the user its tag names.
permit (principal, action == Action::"read", resource)
when { principal.hasTag("delegate") && principal.getTag("delegate").level > 5 };
