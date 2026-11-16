# Validation scope

Initial release preparation: 2026-11-16. Validation host: Windows 11 x64, build 22631. Results below distinguish real media processing from protocol fixtures and optional model checks.

## Automated checks already executed

| Suite | Result | What it establishes |
| --- | --- | --- |
| Workspace storage | 9 real test cases passed | Official Qt, configurable roots, resource lookup, traversal and junction rejection |
| Assistant panel | 37 passed | Widget state, input and interaction contracts, including external control mode |
| AI connection dialog | 17 passed | Provider presets, validation, session-only keys and safe settings persistence |
| Grouped undo | 5 passed | Undo grouping behavior |
| Codex client | 28 passed | Local fake-server protocol and failure handling; not a live model quality assessment |
| Compatible AI client | 32 passed | Local HTTP tool loops, error handling, cancellation, redirects and stale replies |
| WorkBuddy MCP bridge | 17 passed | Real local pipes and stdio relay, current-user access, exclusive instance, cancellation and blocked-output handling |
| Assistant backend session | 10 passed | Switching providers, stale-token rejection and cancellation without re-entry |
| Resource provider credentials | 8 passed | Real provider implementation with local HTTP fixtures; missing keys send no request and user-owned keys replace placeholders |
| Public video provider | 40 passed | Local HTTP protocol fixtures; not a live free-quota guarantee |
| Transcript words | 1 Python test passed | Word-timing conversion |
| Python workspace/media regression | 11 passed | Real PNG export, real one-second audio processing, output preservation and path checks |
| Avatar job regressions | Qt report: 18 passes including setup/cleanup | Real FFmpeg media plus a fake inference worker, output checks, cancellation and package/data path separation; no actual model inference |
| Launcher | 10 path cases passed in Windows PowerShell 5.1 | Custom/default roots, invalid paths and junction rejection; no GUI in this fixture |
| Runtime manifest rejection | 17 passed | Wrong versions, empty or incomplete hashes, invalid paths, duplicate entries and changed files fail validation |
| Official dependency smoke | Passed | Real MLT two-segment render: H.264/AAC, 320×180, 25 fps, 50 frames, 2.005 seconds, nonblank picture and audible signal |
| SoX plugin | Passed after verified Ogg compatibility mapping | Real installed MLT filter registration, no previous missing-library error |

The nine primary Qt suites total **195 actual cases**, excluding their setup/cleanup hooks, with zero failures or skips. The unified runner passed 193 cases; the subsequent cancellation regression added two cases and the complete 10-case session suite passed again. The eight resource-provider cases run separately because they also need KDE Frameworks. These local runs used official Craft Qt 6.12.0. GitHub CI separately uses official Qt 6.10.0; consult the actual [Actions results](https://github.com/John201-del/localcut/actions), not this local count, for remote status.

The Python regressions used an existing local dependency environment. They do not constitute a fresh download/install of every optional AI dependency.

## Complete application acceptance

The final local Windows runtime was built, installed from the packaged overlay, and checked against its application hashes. The following native-editor checks were completed on 2026-11-16 using the bundled MCP relay and a protocol client controlling the real GUI:

| Check | Observed result |
| --- | --- |
| Read state, wait 10 seconds, insert the demo | Insert succeeded in the final installed editor |
| Edit the timeline | Split at frame 50; add a subtitle spanning frames 0–50; apply white text, black outline and font size 24 |
| Native undo and redo | One undo restored one A/V segment and removed the subtitle; redo restored two A/V segments and the subtitle |
| Save and export | Project saved and MP4 exported successfully |
| Exported video | H.264, 640×360, 25 fps, 150 frames, 6.000 seconds |
| Exported audio | AAC, 6.016 seconds; measured RMS level −24.10 dB |

These checks establish the installed editor's native operations and MCP transport. WorkBuddy's own model/client workflow requires separate acceptance.

**Final Codex acceptance passed after a parameter-error fix.** Missing or malformed state fields had been incorrectly classified as `stale_state`, stopping a turn without changing the timeline. The application now returns `invalid_request` with the required field names; genuinely different project IDs or revisions remain strictly rejected. A 38-case check against the real installed editor passed with no failures or skips: 33 malformed requests, four genuine stale contexts, and one unsupported-operation control. Project contents stayed unchanged. The accompanying Catch regression was added but was not run as part of the Qt-only suites.

The corrected source was rebuilt, packaged, installed into an empty runtime directory with Windows PowerShell 5.1, hash-checked and rendered through real MLT again. In a fresh data directory, the normal Codex connection completed one Chinese request to insert the demo, split linked video/audio at two seconds, add the two-second Chinese subtitle, save the project and export MP4. The model corrected an initially incomplete tool request within the same turn. The exported file was byte-identical to the independently exercised native result above (SHA-256 `34ed6379684281895686c9ae2304423286e9a430e4217642d0d057d2e8331c27`). This verifies this account, CLI version and example; it does not guarantee every model-generated edit.

The first remote GitHub CI results will be linked after publication. Local acceptance does not substitute for those remote checks.

## What these results do not cover

- Every video container, GPU, display driver, Windows revision or hardware configuration.
- Linux/macOS validation of the added assistant and local-model environment.
- A new installation and end-to-end inference of every optional large model. Missing model/fixture tests were explicitly skipped.
- Continuous availability or free quotas of remote video, voice or media services.
- Arbitrary Codex versions and accounts. Protocol fixture tests cannot prove account login, quota or model-generated editing decisions.
- Perfect face tracking, photorealistic avatars or a complete 3D character system.

## Reproduce

Run `scripts/test-unit.ps1` as described in [BUILD.md](BUILD.md). For a clean installed runtime, run `scripts/check-runtime.ps1`, then `scripts/smoke-runtime.ps1` with a new empty output directory. These commands fail on native-process errors instead of returning a success marker for missing output.

The release installation workflow checks out the release tag on a clean Windows runner, downloads the exact published application asset and pinned official dependencies, loads the application for a version check, then renders real finite media. It uploads logs and the media report. This supplements desktop acceptance; it is not an automated inspection of the full UI.
