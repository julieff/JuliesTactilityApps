# Repository guidance

- `Apps/` contains independent Tactility app projects; follow an existing app's
  structure and the [Tactility app-development documentation](https://github.com/TactilityProject/TactilityDocs/tree/main/docs/main/app-development).
- Keep changes scoped to the affected app or tooling. Preserve each app's
  manifest, SDK compatibility, and existing behavior unless a change is requested.
- Document user-visible setup, dependencies, and limitations in the affected
  app's README. Validate changes with focused checks, respecting any request not
  to build or run tests.
