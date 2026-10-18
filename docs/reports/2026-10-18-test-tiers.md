# Test tiers

Measured 2026-10-18 on Windows, MSVC Release, Vulkan/Slang with the Khronos
validation layer from Vulkan SDK 1.3.290, NVIDIA RTX A5000, in a remote
desktop session. Source base: main `15bc72e`. Renderer behaviour is
unchanged; the change is how CTests are classified and registered, and when
hosted CI builds. The [test policy](../design/TEST_POLICY.md) owns the rules.

## Question

Validation had grown faster than the time spent waiting for it was worth.
Where did the cost actually sit, before SVGF adds its own fixtures?

## Before

Hosted CI, pull request run 39077307871 (`codex/viewport-timing`):

| Job | Seconds | Of which |
| --- | ---: | --- |
| docs | 8 | |
| core | 84 | 69 build and test without a runtime |
| hydra | 604 | 28 Vulkan SDK install, 41 runtime pull and verification, 519 build, test and strict validation |

The hosted runners have no GPU, so every `gpu` check there is an explained
SKIP and the Hydra cell's CTests total about one second. Hosted CI cost is
the Hydra build; SDK and runtime setup is about a minute.

Locally, the CTest cost history of the default core build
(`CTestCostData.txt`, 2026-10-17) records 170 seconds of serial test time,
150 of them (88%) in Renderer Phase 5's reservoir benchmark workload and
proposal variants and the two procedure controls — campaign procedures
whose conclusions the [exit review](2026-10-17-renderer-phase5-exit.md)
already holds.

## Change

- Every CTest carries one tier label, `fast`, `gpu` or `research`, given with
  `lotus_test_tier` (`cmake/LotusTesting.cmake`). `lotus-test-tiers` reads
  `ctest --show-only=json-v1` and fails when a test has no tier or several,
  when research tests are registered without `LOTUS_RESEARCH_TESTS`, or when
  that option registers none. Existing area labels (`hydra`, `viewport`,
  `presentation`, `host-smoke`) are kept; the Hydra directory's tests gain
  `hydra`.
- `LOTUS_RESEARCH_TESTS` (off by default; the `research` intent turns it on)
  registers the 16 reservoir benchmark variants (`-mixed`, `-glossy`,
  `-spheres`, `-multibounce`, `-interreflection`, `-animation`, `-uniform`,
  `-power-uniform` under both integrators) and
  `lotus-renderer-restir-controls-reference` and `-wavefront`. Their
  arguments are unchanged.
- The default suite keeps `lotus-renderer-reservoir` and
  `lotus-renderer-restir-statistics` (`fast`), `lotus-renderer-restir-gpu`
  (initial, temporal and spatial lifecycle and resets), the distant
  `lotus-renderer-restir-benchmark-reference` and `-wavefront` (report,
  every reuse mode, memory accounting, history, the Lambert integral gate,
  CLI preservation), the missing-driver regression, and the Hydra reservoir
  contract and GPU checks.
- `renderer-ci.yml` gains a `changes` job. For a pull request (not a release
  call) whose changed paths are all under `docs/` or top-level Markdown, the
  core and Hydra cells are skipped; every other event builds both.

## Results

Every configuration passes, and `lotus-test-tiers` reports:

| Build | Tests | Research | `ost test` result |
| --- | ---: | ---: | --- |
| default (`core`) | 34 | 0 | 34/34, 230 s wall at `--jobs 8` |
| `research` intent | 52 | 18 | 52/52, 300 s wall at `--jobs 8` |
| `ci-core`, runtime-free | 23 | 0 | 23/23, 2 s |
| `hydra`, `lookdev` | 61 | 0 | 61/61, no SKIP, 367 s wall at `--jobs 8` |

In the research build, the 18 research tests take 1,068 of 1,521 seconds of
serial test time (70%). `ctest -L fast` on the default build runs its 22
`fast` tests in 1.2 seconds.

The path classification was checked against sample change sets: documentation
and top-level Markdown alone skip the builds; any other path, including
`scripts/`, builds. A skipped job counts as passing a required check. This
pull request changes code, so its own run builds both cells; the skip is
first observed on a documentation-only pull request.

## Observations

- Absolute times in this session are several times the cost history's. The
  validation layer dominates: `lotus-restir-gpu-test` took 134 seconds with
  layers and 21 seconds with `VK_LOADER_LAYERS_DISABLE=~all~`, against 10
  seconds in the 2026-10-17 history. The tier split is independent of this,
  but wall time at `--jobs 8` is bounded by the longest test.
- `lotus-renderer-restir-gpu` (134–200 seconds here) is the longest test in
  both core builds and second only to `lotus-renderer-usdview-host` in the
  Hydra build, so moving the research tier shortened the default suite's
  wall time by less than its share of serial time. It loads the validation
  layer about 126 times, once per Vulkan instance. Sharing
  one device across its cases is the next reduction, under the
  [admission rule](../design/TEST_POLICY.md#5-admitting-a-test) rather than
  by dropping coverage.
- `ost test` selects only by name (`--filter`), so tier selection uses
  CTest's `-L` on the build directory or the `research` intent.
