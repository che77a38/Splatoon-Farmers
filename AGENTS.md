# Agent Skills

This file documents the conventions the engineering skills in this repo follow. Run any of the listed skills; they will read these notes first.

## Agent skills

### Issue tracker

GitHub Issues on `che77a38/Splatoon-Farmers`, accessed via the `gh` CLI. External PRs are not a triage surface. See `docs/agents/issue-tracker.md`.

### Triage labels

Five canonical labels matching role names (`needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`). `wontfix` already exists in the repo; the other four are created on first use. See `docs/agents/triage-labels.md`.

### Domain docs

Multi-context: `firmware/` and `web/`, each with its own `CONTEXT.md` and `docs/adr/`, plus a root `CONTEXT-MAP.md`. System-wide decisions live in `docs/adr/` at the root. See `docs/agents/domain.md`.
