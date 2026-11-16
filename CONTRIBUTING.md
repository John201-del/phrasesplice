# Contributing to LocalCut

Start with a small, reproducible editing scenario. A useful issue includes the Windows version, LocalCut release, exact steps, expected result and actual result. Use synthetic or shareable media. Do not upload login files, API keys, private conversations or personal footage without consent.

For code changes:

1. Read [BUILD.md](docs/BUILD.md), build this fork and create a focused branch.
2. Keep the Kdenlive source style. Add a regression test for changes to edit transactions, paths, protocol handling or media output.
3. Run `scripts/test-unit.ps1` with an official Qt installation and verify a real export when changing the rendering path.
4. Describe the user-visible problem, the change and the checks actually run. State dependencies or skipped model tests explicitly.

The public CI covers the selected unit suites, not every optional model or live provider. Changes to download pins must include verified publisher checksums and a clean installation result. Do not add binaries, models, userdata or credentials to Git.

Contributions are accepted under the existing compatible per-file license. New application code uses GPL-3.0-only. Preserve original copyright and license notices.
