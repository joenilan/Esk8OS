# Codex Security Adoption Prompt

Copy the prompt below into another project when you want its agent to evaluate and adopt the global Codex Security setup safely.

```text
Set up this project to use the already-global OpenAI Codex Security tool when it is useful.

First read every repository instruction file and inspect the working tree. Preserve all existing uncommitted work and do not modify package manifests or lockfiles merely to use a globally installed tool.

Verify, rather than assume:
- whether `codex-security` is on PATH and its exact version
- `codex-security info`
- `codex-security login status`
- whether the official `codex-security-*` agent skills are installed
- the current official documentation and open release-blocking issues

Known historical warning: `@openai/codex-security` 0.1.1 failed while sealing `scan-manifest.json` on Windows and Linux; see openai/codex-security issues #51, #27, and #20. If the installed version is affected or a tiny test cannot save a complete report, stop before spending more scan budget. Do not claim the project is clean from a failed or incomplete scan.

Inspect this repository and identify its actual security boundaries: authentication/authorization, secrets, network and wireless inputs, parsers and serialization, file handling, shell/process execution, update paths, persistence, dependencies, safety-critical control, and any project-specific untrusted inputs.

Add concise repository guidance describing:
- the situations where Codex Security should be considered
- the project-specific high-risk paths and trust boundaries
- dry-run first, then the narrowest useful path/working-tree/diff scope
- explicit cost limits and the fact that in-flight requests can exceed them
- private scan output that is never committed without inspection
- mandatory review of both findings and coverage
- validation before patching and normal tests after any accepted fix
- no hooks, CI gates, exports, automatic patches, or broad paid scans without explicit approval

Do not install a project dependency if the global CLI is usable. Do not enable a commit hook or CI gate during initial adoption.

Test the integration in this order:
1. version, info, and login status
2. a no-cost dry run on the smallest representative scope
3. only with explicit approval, a tightly capped one-file or small-diff scan
4. confirm that the report and coverage artifacts were completely saved
5. validate any important finding against the source; do not auto-fix

Create a short project document recording the installed version, verified behavior, limitations, exact commands, appropriate use cases, artifact/privacy policy, and automation policy. Report every file changed, all test results, scan completeness and coverage, estimated cost, and any unresolved blocker.
```
