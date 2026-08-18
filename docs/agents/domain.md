# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- **`CONTEXT-MAP.md`** at the repo root — points at one `CONTEXT.md` per context.
- **Per-context `CONTEXT.md`** at the directory the work targets (`firmware/CONTEXT.md` for firmware, `web/CONTEXT.md` for the web companion).
- **`docs/adr/`** — read ADRs that touch the area you're about to work in. In multi-context repos, also check `docs/adr/` inside the context directory (e.g. `firmware/docs/adr/`).

If any of these files don't exist, **proceed silently**. Don't flag their absence; don't suggest creating them upfront. The `/domain-modeling` skill (reached via `/grill-with-docs` and `/improve-codebase-architecture`) creates them lazily when terms or decisions actually get resolved.

## File structure

Multi-context repo (this repo — firmware + web companion):

```
/
├── CONTEXT-MAP.md
├── docs/adr/                            ← system-wide decisions (shared)
├── firmware/
│   ├── CONTEXT.md                       ← ESP32-S3 firmware context
│   └── docs/adr/                        ← firmware-specific decisions
└── web/
    ├── CONTEXT.md                       ← browser/web companion context
    └── docs/adr/                        ← web-specific decisions
```

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a test name), use the term as defined in the relevant context's `CONTEXT.md`. Don't drift to synonyms the glossary explicitly avoids.

If the concept you need isn't in the glossary yet, that's a signal — either you're inventing language the project doesn't use (reconsider) or there's a real gap (note it for `/domain-modeling`).

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than silently overriding:

> _Contradicts ADR-0007 (event-sourced orders) — but worth reopening because…_
