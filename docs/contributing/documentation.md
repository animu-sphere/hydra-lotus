# Documentation guidelines

Documentation is part of the implementation contract. A change is incomplete
if it changes a public boundary, implemented architecture or delivery status
without updating the page that owns it.

**One concept, one owning repository, one canonical document.** Everything
below follows from that. These rules follow the sibling repositories'
([`hydra-toon`](https://github.com/animu-sphere/hydra-toon/blob/main/docs/contributing/documentation.md),
[`usd-vrm-plugins`](https://github.com/animu-sphere/usd-vrm-plugins/blob/main/docs/contributing/documentation.md)),
so a reader moving between them finds the same shape.

## Category ownership

| Category | Put this here | Not this |
| --- | --- | --- |
| root `README.md` | What this repository is, what it owns and does not, a small diagram, a component table, links, a minimal build entry point, the licence | See [root README](#root-readme) |
| `docs/README.md` | Which document owns which subject | Content of its own |
| `design/` | Intended contracts, their rationale, open questions | Claims that something is implemented |
| `architecture/` | Target identities, directories, dependency edges, packaging — the binding structural contract | Rationale; plans |
| `reference/` | Facts about the current tree: capabilities, measured configurations | Plans; another repository's status |
| `roadmap/` | Incomplete work this repository owns, and which release carries it | Completed work; rationale; a sibling's roadmap |
| `guides/` | How to accomplish a task, with commands that have been run | Commands nobody has run |
| `releases/` | One immutable record per released version | Work in progress |
| `reports/` | Dated measurements and observations: builds, benchmarks, convergence and correctness runs, `ost` dogfooding | Current-state claims |
| `archive/` | Plans and documents that were once authoritative and no longer are | Anything a reader should act on |
| `contributing/` | How to maintain this repository | End-user tasks |

The same fact is not maintained independently in two categories.

## Cross-repository contracts

`hydra-lotus` consumes OpenUSD, MaterialX and OpenPBR, and is built with
OpenStrata ([integration scope §3](../design/INTEGRATION_SCOPE_POLICY.md#3-what-it-consumes-and-from-whom)).

> A repository may describe how it consumes another project's contract,
> but must not redefine that contract.
>
> Link to the owner instead of copying its API semantics, capability status,
> roadmap, or implementation state.

Link to a sibling's **canonical** document, never to one it has archived or
superseded. A discrepancy found in another repository's documents is recorded
in [integration scope §6](../design/INTEGRATION_SCOPE_POLICY.md#6-cross-repository-observations)
and raised with the owner, not corrected by restating.

## Root README

The root README is an entry point. It follows the shared shape — **Scope**,
**Architecture**, **Components**, **Documentation**, **Build**, **License** —
and stays short. It does not carry release-by-release history, version status
prose, status columns in the component table, detailed dependency graphs,
contract definitions, or another repository's contracts or status.

## Reports and the archive

A **report** is dated, append-only evidence: what was measured, where and
when. It is never authoritative for current status. A later finding gets a
new report and a one-line forward note on the old one; the only edit an
existing report receives is a link repair.

A claim about image quality, convergence, bias or performance cites a report,
and the report names the scene, the sample count, the seed and the build it
was measured with ([design policy §26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode)).

The **archive** holds intent that has been done or replaced. Every archived
document opens with a *Historical only* banner. A superseded **design**
document is not archived; it stays at its path as a short stub — status,
former purpose, current owner, links to the replacement.

## Metadata

A design document, a superseded stub and an archived document carry YAML
front matter:

```yaml
---
status: accepted       # proposed | accepted | binding | superseded | rejected | historical
owner: hydra-lotus     # the repository that owns the subject
canonical: X.md        # superseded only: the replacement, relative
---
```

## Naming

- Phase identifiers are always qualified by their sequence: Renderer Phase 1,
  Renderer Phase 1.5. A sibling's sequence carries the sibling's name
  ("`hydra-toon` Renderer Phase 1").
- Section numbers in design documents are stable, so they can be cited.
  [DESIGN_POLICY.md](../design/DESIGN_POLICY.md) keeps the implementation
  direction's §1–§51 numbering, and
  [ROADMAP_POLICY.md](../design/ROADMAP_POLICY.md) the roadmap policy's
  §1–§10. A superseded section keeps its heading and points to its
  replacement.
- "The reference path tracer" is the Renderer Phase 1 brute-force path
  tracer; "the NEE / MIS renderer" is Renderer Phase 3.

## Language and form

- Repository documents are in English.
- Relative links for everything in the repository; code spans for commands,
  paths, targets, types and attribute names.
- Keep each category index (`docs/README.md`, `roadmap/README.md`,
  `archive/README.md`, `releases/README.md`, `reports/README.md`,
  `reports/ost/README.md`) in sync with its files.
- Never commit machine-local paths, or a scene, texture or HDRI whose terms
  do not allow redistribution.

## Change checklist

1. Planned behaviour is not presented as implemented.
2. Every new page appears in its category index.
3. Relative links resolve.
4. Implementation changes update `architecture/` and `reference/`.
5. Completed work leaves `roadmap/`; a completed plan moves to `archive/`.
6. Another project's contract is linked, not restated.
