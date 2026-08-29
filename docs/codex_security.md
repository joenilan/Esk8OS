# Codex Security for Longboard Display

## Installed setup

Codex Security is installed globally rather than as a dependency of this firmware repository:

* Package: `@openai/codex-security@0.1.1`
* Executable: `E:\AI\npm-global\codex-security.cmd`
* Authentication: existing ChatGPT login, verified by `codex-security login status`
* Official skills: 10 `codex-security-*` skills installed in the global agent skills directory
* User `PATH`: `E:\AI\npm-global` has been added for new terminals and Codex sessions

A new terminal or Codex session may be required before `codex-security` and its skills are discovered without an explicit path.

The scanner is intentionally not installed as a project npm dependency. No repository package manifest or lockfile is changed by this setup.

## Verified behavior and current blocker

The CLI version, environment information, stored authentication, skill installation, and scan dry-run preflight were verified successfully on Windows.

Actual scans reached model analysis, but version 0.1.1 could not seal the final report. Even a one-file `src/main.cpp` scan failed at the save stage with:

```text
scan-manifest.json: expected a regular file inside the scan directory
```

The same failure occurred with both a custom output directory and Codex's default private scan directory. This matches upstream reports:

* <https://github.com/openai/codex-security/issues/51>
* <https://github.com/openai/codex-security/issues/27>
* <https://github.com/openai/codex-security/issues/20>

No completed report or reliable coverage artifact was produced. These attempts therefore provide no basis for saying the firmware is secure or that it has no findings.

Recorded cost estimates for completed attempts were approximately $2.85 in total. An earlier interrupted attempt did not produce a final cost record and may have incurred additional usage. Cost limits are estimates; already-running requests can make the final amount exceed `--max-cost`.

Do not repeat paid scans on version 0.1.1 while this issue is unresolved. First check the installed and current release, the upstream issues, and a tiny dry-run/test scan.

## When to use it

Codex Security is useful for focused review when work changes a trust boundary or before a release. In this repository, prioritize:

1. Companion BLE writes, command authorization, pairing, and settings changes.
2. Daly BLE notification and response parsing, malformed frame handling, reconnect state, and untrusted length/value handling.
3. WiFi bridge ownership, remote endpoints, OTA/update behavior, and web or log export.
4. VESC passthrough and any path capable of writing controller configuration.
5. Serial commands, LittleFS filenames and log access, imported data, credentials, and persisted configuration.
6. Dependency changes that affect networking, parsing, cryptography, storage, or device control.

Routine display layout changes, comments, and ordinary builds do not need a paid security scan unless they accompany a security-sensitive change.

## Safe workflow after the upstream fix

Run these checks from the repository root:

```powershell
codex-security --version
codex-security info
codex-security login status
```

Plan the smallest useful scan before spending budget:

```powershell
codex-security scan . --working-tree --base HEAD --max-cost 1 --dry-run
```

For a focused change, prefer a targeted path:

```powershell
codex-security scan . --path src/transports/DalyBms.cpp --max-cost 1 --dry-run
```

Only remove `--dry-run` after confirming scope, exclusions, expected cost, authentication, and a private output location. Start with narrow paths or the relevant diff and expand only when the first completed report justifies it.

For every completed scan:

* Keep artifacts outside the repository or in Codex's private state.
* Inspect coverage before interpreting the findings.
* Validate high-impact findings against the actual code and device behavior.
* Do not auto-patch battery, motor, bridge, or transport safety logic.
* Build and test any accepted fix normally.
* Report incomplete coverage and scanner failures explicitly.

## Automation policy

No commit hook, CI gate, MCP scan integration, or automatic patching is enabled. Add any of those only after the CLI can complete and save a representative scan, its runtime and cost are understood, and the user explicitly approves the workflow.
