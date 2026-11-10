# Project conventions

## Scope

- Build an independent GPL-3.0-or-later Jellyfin plugin for fooyin using C++23 and Qt 6.
- Use fooyin's native library, playlists, settings pages and decoding interfaces.
- Keep required host changes separate from plugin code and document compatibility accurately.
- Follow upstream contribution requirements for any upstream submission.
- Check branch and worktree state before editing; preserve unfinished changes and existing data.

## Style and structure

- Follow the surrounding fooyin code and the repository's `.clang-format`.
- Use PascalCase for types, lowerCamelCase for functions and local variables, and `m_` for members.
- Use lowercase filenames that correspond to their classes; keep plugin naming consistent.
- Keep concept names consistent across interfaces, storage and tests.
- Group code by responsibility and follow nearby fooyin plugin layouts.
- Put the matching header first, followed by local, fooyin, Qt and standard-library includes in separate groups.
- Use brace initialization, const values where appropriate, `Qt::StringLiterals`, typed connections, `Q_SIGNALS` and `Q_EMIT`.
- Use Qt parent ownership for QObject lifetimes and RAII for other resources.
- Make thread ownership, cancellation, account identity and persistence transitions explicit.
- Introduce abstractions only when they clarify a concrete responsibility.
- Inline one-use values unless a name clarifies a complex expression.
- Require arguments in new internal helpers while respecting host and Qt APIs.
- Keep comments brief and explain constraints or reasons rather than restating the code.
- Keep Markdown paragraphs and commit bodies on single source lines.
- Write non-interactive scripts with full option names.
- Keep license headers consistent and retain correct attribution.
- Match fooyin's user-facing terminology, spelling and sentence case; preserve external API field names.

## Verification

- Add focused tests for changed behavior using synthetic accounts, metadata, network responses and audio.
- Run the plugin suite and affected host tests for code changes, plus a successful build.
- Use structural and configuration checks for documentation-only changes.
- Test with isolated profiles and fixtures; do not change real profiles or server data during tests.
- Preserve credential isolation, redirect restrictions, cancellation and recovery guarantees when refactoring.

## Publication

- Use short branch names without prefixes.
- Use `[area] Component: Imperative summary` for commit and pull request titles; omit the component when unnecessary.
- Describe the resulting behavior in short present-tense paragraphs; include screenshots or recordings when they clarify a visible change.
- Inspect every commit's files, diff, message and author metadata before a public push.
- Use a public no-reply email for commits.
- Keep credentials, local environment configuration, private URLs, machine-specific paths, real user data, logs and captures out of commits and pull requests.
- Never log tokens or store credentials in persistent media URLs.
- Keep pull requests focused and descriptions minimal; include no private review links.
