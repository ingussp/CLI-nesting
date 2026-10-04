# Rejection cache, GPU batches and timed search

## Problem and changes

A supplied job has 1000 copies of one concave eight-vertex part, 360 allowed
orientations, one 2000 x 2800 mm sheet, 1 mm raster, 6.1 mm part/hole clearance,
10 mm stock margin, eight CPU workers and an NVIDIA GeForce GTX 1650 Ti.
The timed budget is 120 seconds, with two strategies and 30-second rounds.

The former dense rejection bitmap needed 198.77 MiB for this job, exceeding its
32 MiB cap and disabling rejection reuse altogether. Repeated copies then retried
the same failed contact search. Explicit GPU candidate filtering sent only 64
candidates per synchronous call. The initial fast layout could consume the whole
timed budget, preventing alternative strategies from starting.

Changes:

- Raise the default rejection budget from 32 to 256 MiB; expose `cacheMemoryMiB`
  (1..4096). Allocate touched 4096-position pages rather than the entire domain.
  All concurrent strategies share the same budget. Each page is accounted as
  512 payload bytes plus 128 bytes for map overhead; this is not total process RSS.
- If the budget is full, skip new cache pages and continue ordinary validation.
  Cache destruction releases its budget for subsequent sheets/strategies.
- Reuse a fully failed contact search for identical geometry/rotation policies
  only until the next placement changes occupancy. Deadline-interrupted searches
  are not memoized. `cacheRejects:false` disables both forms of reuse.
- Prefilter ordered contact/shortlist candidates using `gpu.batchSize`, retaining
  64-candidate CPU geometry chunks and selection order. Occupancy stays immutable
  until selection completes. GPU failure still follows the configured fallback.
- Give the initial timed seed at most 20% of the overall budget, also capped by
  `continuousRoundSeconds`. Subsequent rounds use that configured duration, capped
  by remaining global time, instead of an artificial one-second limit.
- Export cache memory, failed-search skips, total strategies started and maximum
  worker counts across timed restarts.

## Sequential measurements (Windows Release build)

The first four rows below are instrumented **first-layout** runs with a 30-second
watchdog, not full timed optimizations. Each step includes the preceding changes.
Machine scheduling/startup varies, so the figures are observations, not guarantees.

| Version | Elapsed | Complete seed | GPU batches | Candidates checked |
| --- | ---: | --- | ---: | ---: |
| Previous vector-optimized build, 32 MiB | 30.026 s | no | 112342 | 7189740 |
| Only 256 MiB dense cache | 30.099 s | no | 4164 | 266457 |
| Also reuse failed contact searches | 10.241 s | yes | 4164 | 266457 |
| Also larger GPU batches | 7.919 s | yes | 95 | 266457 |
| Also sparse shared cache | 6.703 s | yes | 95 | 266457 |

The three completed layouts are identical: 94 placed, 906 unplaced. Failed-search
reuse skips 905 repeated searches. Sparse cache usage peaks at 13,521,280 accounted
bytes (12.90 MiB). Larger batches filter some candidates before they are needed;
GPU candidate count is therefore different from exact candidate checks.

Final executable, ordinary CLI runs in `first` mode:

| GPU | Engine time | Placed | Layout compared with sparse-stage run |
| --- | ---: | ---: | --- |
| on | 7.569 s | 94 | identical |
| off | 6.840 s | 94 | identical |

GPU is working but does not speed up this final first-mode job. Driver/kernel
startup, transfers, serial preparation and refinement remain costs. A configured
pool of eight workers does not mean eight cores stay fully occupied.

The final **120-second timed** run took 120.144 s, started nine strategies in five
search iterations, and retained 94 parts; it did not improve the placement count.
GPU reported 5735 batches and 259,729,187 prefiltered candidates with no fallback.
Peak cache accounting was 32,467,840 bytes (30.96 MiB). Process CPU-time sampling
averaged about 1.38 cores, compared with 1.02 in the prior build. The prior run
started only the seed and made 446,547 GPU calls. Timed mode intentionally consumes
its requested budget; the faster seed leaves that time for competing layouts.

The previous convex 148-vertex regression job also retains its exact 540-part
layout. In this session the new build took 4.331 s versus 5.073 s for the previous
vector-optimized executable; earlier timings on the same machine were lower.

## Validation and reproduction

All four CTest suites pass. New regression checks cover sparse-page boundaries,
shared budget exhaustion and release, occupancy/rotation-policy invalidation,
CPU/GPU layout parity with 256/65536-candidate batches, JSON budget bounds, and
the timed seed leaving room for alternative strategies.

The original exhaustive geometry predicates independently rechecked the final
timed layout and both first-mode layouts: 94 parts, 223 nearby pairs per layout,
no overlap or clearance violations. The older convex fixture passed for 540 parts
and 892 nearby pairs. Clearance values and the 0.001 mm refinement tolerance are
unchanged.

Build Release, run `ctest --test-dir build --output-on-failure`, then use the
provided job unchanged for timed measurement. For seed comparison set `mode` to
`first`, `timeLimitSeconds` to zero, and toggle `gpu.enabled`; keep all geometry,
rotations, clearances and threads fixed. Point each output to a separate directory.
Inspect `totalStartedTrials`, `searchIterations`, `gpu`, `rejectionCache`, and
`vectorRefinement`. Worker counts indicate participation, not utilisation.

This improves reuse and scheduling. It does not prove packing optimality, remove
all serial work, reuse GPU contexts across independent restarts, or guarantee
completion within one second.
