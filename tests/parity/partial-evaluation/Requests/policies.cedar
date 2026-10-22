permit (
  principal == A::"foo",
  action in [Action::"a", Action::"c"],
  resource
)
when { context.flag };

forbid (
  principal,
  action == Action::"a",
  resource
)
when { resource.level > 10 };

permit (
  principal,
  action == Action::"b",
  resource == A::"bar"
);

permit (
  principal,
  action == Action::"a",
  resource
)
when { context has limit && context.limit < resource.level && context.who == principal };
