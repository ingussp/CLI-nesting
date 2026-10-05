# CLI-nesting

C++20 command-line nesting for polygon parts and polygon sheets, with holes,
per-part orientations, independent clearances, OpenCL acceleration, and JSON,
DXF and SVG output. The executable is **clinesting.exe**.

This version targets **Windows x64**. Linux compilation is deferred.

## Build using CMake

Install Visual Studio 2022 with **Desktop development with C++**, MSVC x64,
Windows SDK, CMake and Git. Presets require CMake 3.21 or newer; direct CMake
configuration requires 3.20 or newer. From the repository root:

```text
cmake --preset windows-release
cmake --build --preset windows-release --parallel
build-release\Release\clinesting.exe --input input.json
```

The preset selects Visual Studio 2022, x64 and Release. Equivalent direct commands:

```text
cmake -S . -B build-release -G "Visual Studio 17 2022" -A x64 -DCLINESTING_STATIC_RUNTIME=ON
cmake --build build-release --config Release --parallel
```

`CLINESTING_STATIC_RUNTIME=ON` is the default. OFF selects a shared MSVC runtime
for distributions that require it. The JSON executable is
`build-release/Release/clinesting.exe`. The separate `clinesting_demo.exe`
is a historical demonstration, not the JSON application documented here.

CMake downloads Clipper2 1.5.4 during initial configuration. Its utilities,
examples and tests are disabled. To use an existing offline source checkout:

```text
cmake --preset windows-release -DFETCHCONTENT_SOURCE_DIR_CLIPPER2=C:/deps/Clipper2
cmake --build --preset windows-release --parallel
```

The path must identify the Clipper2 1.5.4 repository root containing `CPP`.
JSON and OpenCL headers are vendored. The GPU runtime comes from the graphics
driver; CUDA and DLLs from other nesting applications are not required.

## Run a job

Edit [input.json](input.json) and run the executable, or use
`clinesting.exe --input C:/jobs/input.json`. Without arguments it reads
`input.json` in the current working directory. `run.cmd` changes to its own
directory and finds the executable there or in `build-release/Release`.

Read the console summary and the saved JSON/DXF/SVG. All relative JSON output
paths are resolved beside the input file, even when the executable is elsewhere.

Save both formats directly from the command line:

```text
clinesting.exe --input input.json --dxf result.dxf --svg result.svg
```

`--svg` enables SVG export without changing `output.svg` in the input JSON.
Command-line output paths use the working directory and override JSON paths.
DXF sheets appear side by side with a 20 mm gap, with their parts, holes and labels
translated together. Continuous mode saves numbered DXF/SVG files in `results`.

Examples: [complete configuration](input.json),
[FreeCAD export](examples/freecad-input.json),
[first layout](examples/mode-first.json),
[ten-minute search](examples/mode-timed.json),
[continuous search](examples/mode-continuous.json),
[0.1-degree rotations](examples/rotation-0.1.json).

## FreeCAD input and configuration precedence

Supported FreeCAD fields include `schema_version: 1`, `settings`, sheet `outer`,
part `rotations`, `output.resultJson`, and `_ip_nesting` metadata. Existing CLI
`config`, `points`, and `output.json` inputs remain supported.

Configuration objects are merged in this order; later values override earlier
values for the same option:

1. `settings`: shared FreeCAD options and supported CLI search settings.
2. `config`: the original CLI configuration object.
3. `CLI-nesting`: an optional dedicated CLI configuration object.

Use one search configuration object in normal jobs. Shared physical options
`units`, `spacing`, `partToSheet`, and `partToHole` can remain in `settings`.
The root input example demonstrates that arrangement.

The following legacy keys are ignored **only inside settings**:
`placementType`, `simplify`, `useSvgPreProcessor`, `scale`, `endpointTolerance`,
`dxfImportScale`, `dxfExportScale`, `exportWithSheetBoundboarders`,
`exportWithSheetsSpace`, `exportWithSheetsSpaceValue`, `mergeLines`, `timeRatio`,
`populationSize`, `mutationRate`, `useQuantityFromFileName`.
They do not scale geometry, merge cutting lines or configure this optimizer.
Replace/remove them when updating the exporter. Other unknown configuration
keys are rejected, including these legacy keys in config or CLI-nesting.

`autoStart` is informational: invoking the CLI always starts a job. Paths in
`filename` or `_ip_nesting` are metadata and are not opened to import geometry.
`_ip_nesting.result_file` does not override the output settings. `job_id`,
`created_at`, and root/per-object `_ip_nesting` are copied to output when present.
FreeCAD grain and 3D placement metadata do not rotate the supplied XY geometry;
export the intended rotation rule explicitly using angle, allowedAngles or rotations.

## Root fields

JSON names and string values are case-sensitive. Use numbers without quotes,
decimal points, and boolean true/false. Comments and trailing commas are invalid.
Windows paths can use `C:/jobs/result.json` or doubled backslashes.

| Field | Default / requirement | Purpose |
|---|---|---|
| `schema_version` | Optional; only 1 | FreeCAD schema identifier |
| `units` | mm | Only millimetres are supported |
| `settings.units`, `_ip_nesting.units` | mm if absent | Units, when provided, must agree with mm |
| `job_id` | Optional string | Correlates exported jobs and results |
| `created_at` | Optional string | Retained creation timestamp; does not control execution |
| `_ip_nesting` | Optional JSON metadata | Retains FreeCAD mapping information |
| `settings`, `config`, `CLI-nesting` | Optional objects | Configuration layers described above |
| `parts` | Required nonempty array | Part types, point geometry and quantities |
| `sheets` | Required nonempty array unless sheet is used | Available stock definitions |
| `sheet` | Alternative single object | Short form for one stock definition; sheets wins if both are supplied |
| `output` | Optional object | Export formats, filenames and preview behavior |
| `autoStart` | Ignored | Compatibility metadata for interactive exporters |

Additional root/geometry metadata may be ignored. Only documented fields affect
nesting. Unknown search, GPU and output options are errors.

## Parts, sheets and contours

| Field | Default / valid values | Purpose |
|---|---|---|
| `points` | Required for a part unless outer is supplied | Ordered outer boundary |
| `outer` | Alias of points | FreeCAD polygon contour; also accepted for parts and hole objects |
| `holes` | Empty array | Hole contours; each item is a point array or an object with points/outer |
| `width`, `height` | Positive numbers up to 1000000 mm | Rectangle shorthand for sheets without a point contour |
| `x`, `y` | Each 0 | Origin of a rectangle sheet; not used with its points/outer |
| `id` | String or integer | Part type/sheet identifier; output preserves it as source |
| `name` | Optional string | Fallback identifier if id is absent |
| `quantity` | Integer, default 1 | Copies: parts 1..100000; sheets 1..1000 |
| `count` | Part-only quantity alias | Used only when quantity is absent; same limits |
| `filename` | Empty string | Metadata only; does not read a DXF and is not currently exported |
| `_ip_nesting` | Optional metadata | Copied to placed/unplaced copies and sheet results |
| `type` | Informational | FreeCAD usually sends polygon; actual point data controls behavior |
| `rotation` | 0 degrees | Initial part angle offset added to its uniform grid |
| `rotations` | 4 orientations | Part-only integer 1..3600; a uniform orientation grid for this part |
| `angle` | Unset | One absolute permitted part angle |
| `allowedAngles` | Unset | Array of 1..3600 absolute permitted part angles |

Use points or outer. If both exist they must contain identical JSON values.
A contour takes precedence over sheet width/height. Parts require contours;
rectangle shorthand applies only to sheets. Sheet `angle`, `allowedAngles` and
`rotations` are rejected. A legacy sheet rotation is read but does not rotate stock.

Points accept `[x,y]`, `[x,y,z]` or `{"x":x,"y":y}`. Array Z is validated but
ignored; point objects use x/y only. This is 2D nesting: project geometry to XY
and approximate curves with points before exporting. DXF, STEP and FreeCAD
documents are not directly imported.

Each contour needs 3..20000 input points. Consecutive duplicates and an optional
closing copy of the first point are removed. An exactly repeated complete contour
cycle is collapsed to one cycle, including for holes, so bitmap occupancy and
vector area agree. This does not repair arbitrary self-intersections.
At least three points and nonzero
area must remain. Use simple, non-self-intersecting contours in boundary order.
Holes must lie within the outline, must not overlap each other, and must leave
positive material area. One hole level is supported. Winding direction does not
determine holes; their JSON location does.

Numeric values must be finite and have magnitude at most 1000000, subject to
narrower field limits. File size is limited to 64 MiB; expanded totals are at
most 100000 parts and 1000 sheets. Part type IDs must be unique. Missing id/name
becomes part_1, part_2, sheet_1, etc. Each physical copy receives a numeric ID.

### Rotation rules

Angles are degrees counterclockwise about the input origin, applied before the
reported translation. `angle: 45` permits only 45 degrees;
`allowedAngles: [0,90]` permits either orientation. Arbitrary angles such as
13.25 are accepted; negatives are normalized into [0,360), and absolute-list
duplicates are removed.

Part `rotations: 1` permits only its rotation offset, default 0, matching the
FreeCAD example. Part `rotations: 4` permits offset + 0,90,180,270. Without a
part-specific rule, each part defaults to four orientations (0,90,180,270). All
quantity copies share one rule; use separate part records for different copy
orientations.

Do not combine part rotations with angle/allowedAngles; do not combine angle
with allowedAngles; and do not combine absolute angles with nonzero rotation.

## Three independent clearances

All three distances are in millimetres, default to 0, and accept 0..1000000.

| Setting | Measured between | Purpose |
|---|---|---|
| `spacing` | Outer boundaries of different parts | Cutting gap between neighboring parts |
| `partToSheet` | Part outer boundary and sheet outer boundary | Stock margin, including concave edges |
| `partToHole` | Part boundaries and sheet-hole boundaries; also boundary pairs involving holes in another part | Clearance to cutouts and when nesting inside a part hole |

`sheetSpacing` and `holeSpacing` are aliases of partToSheet and partToHole.
Conflicting aliases in one object are rejected. Prefer the FreeCAD spellings.

```json
"settings": {"units":"mm", "spacing":5, "partToSheet":5, "partToHole":2}
```

Spacing 5 requests a 5 mm gap, not a doubled half-offset. The three rules are
independent: all applicable boundary pairs must satisfy their own limits.
Zero permits touching but never overlapping forbidden material. Positive gaps
reject contact. Numerical tolerance near the exact requested distance is at
most 0.0000001 mm.

Bitmap/GPU checks eliminate collisions; exact original-polygon CPU validation
then checks containment, overlap and Euclidean edge distances. Neighbor lookup
includes the gap even when bounding boxes do not touch. Search also proposes
spaced rows/columns. Exported contours are unchanged: these settings do not
enlarge parts, shrink exported sheets or generate compensated toolpaths.
Raster resolution can produce larger achieved gaps or miss narrow feasible spaces.

## All search options

These keys are accepted in settings, config or CLI-nesting.

| Field | Default | Valid values and purpose |
|---|---|---|
| `mode` | Inferred, normally first | first, timed, continuous; controls duration and saving behavior below |
| `timeLimitSeconds` | 0 | 0..86400, including fractions; timed requires a positive total budget, first requires 0, continuous ignores it |
| `continuousRoundSeconds` | 30 | 0.01..86400; budget per timed/continuous restart. Too short can spend every restart on preparation |
| `continuous` | false | Legacy boolean; without mode, true selects continuous. With mode it must agree: true only for continuous |
| `threads` | Hardware logical CPU count, at least 1 | Explicit integer 1..256; CPU search worker budget. Driver/OS threads are separate and serial phases cannot use all workers |
| `trials` | 2 | Integer 1..4: compact, then holes_first_rows, large_first, small_first. More strategies cost more time/memory. First mode and jobs with fewer than six copies and no part holes use one |
| `resolution` | 1 mm/pixel | Positive number up to 1000000. Finer pixels increase precision and memory; halving pixel size roughly quadruples raster area. Does not scale geometry |
| `bitmapResolutionMm` | Alias of resolution | Same range; prefer one spelling. If both exist, resolution wins in the current parser |
| `step` | 1 pixel | Integer 1..100000; minimum fine-search translation step. Physical step is step Ãƒâ€” resolution. Larger steps can skip feasible pockets |
| `bitmapSearchStepPx` | Alias of step | Same range; prefer one spelling. If both exist, step wins |
| `curveTolerance` | 0.3 mm | 0..1000000; contact-proposal contour simplification tolerance. Smaller retains more detail and costs more work. Original validation/export points remain intact; this does not import DXF curves |
| `cacheRejects` | true | Boolean; remembers failed raster origins while occupancy grows. False reduces cache memory at the cost of repeated checks |
| `cacheMemoryMiB` | 256 | Integer 1..4096; shared rejection-page budget across concurrent strategies; unused pages are not allocated |
| `spacing` | 0 mm | Outer-boundary gap between parts |
| `partToSheet` | 0 mm | Margin to the stock outline |
| `partToHole` | 0 mm | Margin involving hole edges |
| `gpu` | false | Boolean or OpenCL object described below |

### Search strategies (trials)

`trials` is integer 1..4 (default 2) and applies only to bitmap nesting.
It selects how many independent nesting strategies run per search round. Each
strategy builds a complete layout from scratch with a different tactic, and the
best validated layout is kept. Strategies run in parallel, one worker thread per
strategy (up to `threads`), so more trials cost proportionally more time and
memory.

| # | Strategy | What it does |
|---|---|---|
| 1 | `compact` | Greedy compact search in the input part order. This is the baseline. |
| 2 | `holes_first_rows` | Places parts with large holes first, then tries inserts inside those cavities before repeated pair/row patterns (six or more identical copies) or compact fallback. Every accepted position passes geometry and clearance checks. |
| 3 | `large_first` | Sorts remaining parts by material area descending and places the largest first. |
| 4 | `small_first` | Sorts remaining parts by material area ascending and places the smallest first. |

The winner is chosen by, in order: fewest unplaced copies, then least used-sheet
waste, then the smallest occupied bounding rectangle. The chosen index is
reported as `selectedTrial`, and each strategy's count, duration, completion and
phase timings appear in `strategyResults`.

`trials` takes effect only in `mode: "timed"` and `mode: "continuous"`.
Mode `first` runs the single `first_bottom_left` strategy described below and ignores `trials`.
Jobs with fewer than six part copies and no part holes also use one strategy.
Small jobs containing part holes retain the requested alternatives: even one
frame and one insert can depend on placing the frame first.

### First layout

Mode first runs one `first_bottom_left` strategy across available sheets and
exports its result without optimization restarts. It orders parts by material
area, largest first, and tries permitted orientations inside existing holes and
at contact positions. Free rectangle corners propose additional gap placements.
Each sheet is compacted and refilled before opening the next sheet.

Hole bounds propose positions but never authorize a placement: the whole outline
must fit the actual hole, and normal stock, collision and clearance checks still
apply. Holes in the stock itself remain forbidden. `holePlacements` reports copies
accepted by this cavity search. See [the holes-first example](examples/holes-first.json).

This is a fast initial layout, not a guarantee of maximum density or that all
copies fit: inspect `unplacedCount`. Use timed mode to compare this heuristic
with the input-order, large-first and small-first compact strategies and keep the
best layout. `trials` does not change the single-strategy behavior of first mode.

### Timed optimization

```json
"CLI-nesting": {
  "mode":"timed", "timeLimitSeconds":600,
  "continuousRoundSeconds":30, "trials":4, "gpu":true
}
```

600 seconds is ten minutes. Search continues for the whole budget, restarting
with varied part order and angle priorities. The best validated layout is kept
across strategies/restarts and exported at the deadline or on user stop. Worse
or incomplete attempts do not replace it. A tiny budget can leave all parts unplaced.

The deadline is cooperative and shared across sheets/restarts. In-flight GPU
or geometry operations finish safely; input/output, cleanup and preview opening
can add time beyond the configured search budget.

### Continuous optimization

Mode continuous creates or **empties the entire results directory beside the
input JSON** at startup, then searches until stopped. Back up old results before
starting another session. Do not keep the input inside that results directory.

The first validated candidate establishes a baseline, possibly partial. Strict
improvements are saved as results/result1.json, result2.json, etc., with matching
.dxf/.svg when enabled. Equal/worse candidates produce no new files. Search
continues after all parts fit and after rounds with no improvement. There is no
overall deadline. Output basenames are ignored here; the fixed resultN names apply.

Files are written completely under temporary names, then companions are
published before the final JSON. That JSON marks a completed result group.
After forced termination, ignore temporary or companion-only groups. A lock
prevents concurrent sessions from clearing each other's files; use separate input
directories. Symlinks/junctions inside results are rejected before cleanup.

Ctrl+C/Ctrl+Break requests a graceful stop. Console close also requests stopping,
but Windows allows only a limited shutdown interval. Previously published files
remain; force-killing or power loss cannot guarantee the in-flight candidate.
First/timed modes do not automatically clear the results directory.

### Quality objective

Timed/continuous search prefers, in order: fewer unplaced copies, less unused
material in used sheets (usedSheetWasteArea), then less unused area inside occupied
bounding rectangles (compactWasteArea). For identical parts on identical stock,
physical scrap area is constant; rearrangement can still improve compactness and
remnant shape. Clearances remain unused material in the statistics. Search is
heuristic: global optimality and further improvements are not guaranteed.

## OpenCL GPU options

```json
"gpu": {"enabled":true, "device":-1, "fallbackToCpu":true, "batchSize":65536}
```

| Field | Default inside object | Purpose |
|---|---|---|
| `enabled` | true | Boolean; initialize OpenCL. False disables it while retaining settings |
| `device` | -1 | Integer -1..1024; -1 chooses automatically, preferring a discrete GPU. Other values must match an available --list-gpus index |
| `fallbackToCpu` | true | Boolean; continue on CPU and report the reason if GPU startup/execution fails. False turns these failures into errors |
| `batchSize` | 65536 | Integer 256..262144; maximum candidates per GPU batch. Larger batches may reduce launch overhead but increase buffers and latency |

gpu true and an empty object use these defaults; gpu false does not load OpenCL.
`clinesting.exe --list-gpus` prints device indices, names, vendors and memory.
Indices need not match Task Manager numbering. Install a graphics driver with OpenCL.

GPU handles bitmap collision batches. CPU builds geometry/proposals, validates
exact contours and clearances, scores and commits placements. With GPU enabled,
hole, row, contact, grid and refinement candidates all use GPU filtering. Repeated
rows use lookahead batches; cached positive results are rechecked against current
occupancy before placement. CPU helpers use the configured thread budget regardless
of the number of shapes, copies or allowed angles. The budget is shared between
independent strategies and their helpers.
After every candidate in an existing cavity fails for a shape and its angle
policy, later identical copies search only newly added cavities: occupied
material cannot become free as more parts are placed.

These settings enable useful work on the selected hardware; they do not guarantee
continuous 100% utilization. Layout commits remain ordered. Driver initialization
and transfers can make short GPU-enabled jobs slower than CPU-only jobs. Empty or
cancelled jobs may finish before any GPU work; failures follow `fallbackToCpu`.

## Output configuration

Output is a root object alongside parts/sheets, not inside settings.

| Field | Default | Purpose |
|---|---|---|
| `resultJson` | result.json | Nonempty string path; mandatory JSON output, FreeCAD spelling |
| `json` | Alias of resultJson | Original spelling; both must agree when supplied together |
| `dxf` | false | false disables CAD output; true uses result.dxf; nonempty string chooses a path |
| `svg` | false | false disables preview export; true uses result.svg; nonempty string chooses a path |
| `openPreview` | false | Boolean; open SVG in the Windows-associated viewer. Requires svg; continuous opens only the first result |

Paths may be absolute or relative to the input directory. Boolean true uses the
default filename independently of the JSON filename. First/timed creates missing
parent directories and overwrites existing outputs. Input and all output formats
must use distinct files, including hard links/path aliases. JSON cannot be disabled.
Continuous uses only the enabled formats, with fixed results/resultN filenames.

SVG shows stock, colored parts, holes and tooltips. JSON/DXF retain full point
geometry. Failure to open a viewer does not delete the output. DXF is an actual
CAD exchange file, not JSON with a changed extension.

### Result JSON fields

| Field | Meaning |
|---|---|
| `schemaVersion`, `schema_version`, `units` | Output schema 1 and millimetres |
| `job_id`, `created_at`, `_ip_nesting` | Input metadata when present |
| `placed`, `unplacedCount`, `unplaced` | Counts and remaining copy IDs/sources; partial results are valid |
| `sheets[].id`, `source`, `points`, `holes` | Used stock identity and geometry |
| `sheets[].parts[]` | Copies with numeric id, source, translation x/y and rotation in degrees |
| Part `points`, `holes` | Already transformed absolute coordinates; do not transform again |
| Object `_ip_nesting` | Original metadata, also on unplaced copies. Original 3D placement is not the nesting result transform |
| `clearances` | Effective spacing, partToSheet and partToHole |
| `mode`, `continuous`, `timeLimitSeconds`, `continuousRoundSeconds` | Effective search controls |
| `timeLimitReached`, `stopReason` | completed, time_limit or user_stop. Continuous snapshot status describes its candidate, not session completion |
| `timingMs`, `searchIteration` | Search/orchestration duration excluding file writing; zero-based restart of the saved candidate |
| `trials`, `startedTrials`, `selectedTrial` | Strategy counts and winner diagnostics |
| `workersUsed`, `proposalWorkersPerTrial` | Strategy worker count and CPU slots per strategy (including its caller) |
| `cpuWorkersUsed` | Total CPU pool size across strategy workers and helpers; does not exceed `threads` |
| `candidateWorkersUsed` | Largest number of CPU slots checking bitmap or exact geometry candidates in one batch of the selected strategy |
| `sheetRefill` | Additional placements and successful refill passes after per-sheet vector compaction |
| `patternPlacements`, `holePlacements`, `rejectedPositionSkips` | Pattern placements, copies inserted in part holes, and failed-position cache diagnostics |
| `strategyResults` | Per-strategy counts, duration, completion and phase timings |
| `gpu.requested`, `used`, `device`, `backend` | Request, actual kernel use, device and OpenCL backend |
| `gpu.batches`, `candidates`, `fallbackReason` | GPU diagnostics. Timed totals cover restarts; continuous snapshots describe the candidate strategy |
| `usedSheetWasteArea` | Unused material in used sheets, mm squared, excluding holes and unused sheets |
| `occupiedBoundsArea`, `compactWasteArea` | Occupied raster bounding-rectangle area and unused area within it |
| `utilisation` | Legacy percentage against processed stock material; not the sole quality criterion |

## Command-line reference

| Argument | Purpose |
|---|---|
| `--input path` | Input JSON, default input.json in the working directory |
| `--output path` | Override JSON path; .dxf writes real DXF and a companion .json |
| `--dxf path` | Enable/override DXF output |
| `--threads N` | Override CPU budget, integer 1..256 |
| `--trials N` | Override strategy count, integer 1..4; first still uses one |
| `--list-gpus` | Print devices and exit, without reading input |
| `--help`, `-h` | Print brief usage and exit |

CLI paths resolve against the working directory. JSON is validated before CLI
overrides, so arguments cannot repair contradictory JSON. Mode, GPU, deadlines,
clearances and SVG options belong in JSON. Without mode, continuous true selects
continuous; otherwise positive timeLimitSeconds selects timed; otherwise first.

Exit code 0 means completion or graceful stop, even with unplaced parts. Code 1
means input, calculation or file failure. Forced OS termination may use another code.

## Performance and limitations

- Restrict angles when possible. A 0.1-degree grid substantially increases work
  and does not improve translation resolution.
- Finer rasters use more memory. Raster allocation is capped; pixel caches use
  about 64 MiB per strategy/sheet. Rejected origins use sparse pages with a shared
  default 256 MiB accounting budget (`cacheMemoryMiB`), including an allowance for
  map overhead. Reaching the budget skips new cache entries, not validation. Geometry and
  other state need additional memory. GPU masks are limited to 512 MiB and the
  device allocation limit.
- Large gaps or small stock may prevent every placement. Check unplacedCount;
  exit code 0 is not proof that all copies fit.
- For GPU problems, inspect --list-gpus and fallbackReason or allow fallbackToCpu.
- If the window immediately closes, run from a terminal or use run.cmd.
- Increase continuousRoundSeconds if restarts spend their whole budget preparing.
- Check spelling and object location when an option is rejected.
- There is no toolpath generation, automatic kerf compensation or 3D transformation.

## Source layout

`demo/json_main.cpp` handles CLI modes and exports; `src/json_io.cpp` handles
input/output; `src/bitmap_nesting.cpp` implements raster search and scheduling;
`src/geometry.cpp` implements polygon and clearance checks;
`src/continuous_nesting.cpp` manages restarts; `src/gpu_bitmap.cpp` handles OpenCL;
`demo/continuous_results.hpp` publishes numbered files safely;
`demo/svg_preview.hpp` writes previews; `include/clinesting` contains public interfaces.

## Third-party code and licenses

| Dependency | Version and purpose | License |
|---|---|---|
| [Clipper2](https://github.com/AngusJohnson/Clipper2) | 1.5.4, polygon Boolean operations; CMake FetchContent | [Boost Software License 1.0](https://github.com/AngusJohnson/Clipper2/blob/Clipper2_1.5.4/LICENSE) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3, vendored JSON parser/writer | [MIT upstream](https://github.com/nlohmann/json/blob/v3.11.3/LICENSE.MIT), [included license](third_party/nlohmann/LICENSE.MIT) |
| [Khronos OpenCL-Headers](https://github.com/KhronosGroup/OpenCL-Headers) | v2024.10.24, vendored OpenCL declarations | [Apache-2.0 upstream](https://github.com/KhronosGroup/OpenCL-Headers/blob/v2024.10.24/LICENSE), [included license](third_party/opencl/LICENSE) |

The OpenCL runtime is supplied by the GPU vendor's driver, not bundled. Retain
third-party notices when distributing source/packages. Upstream headers retain
their original comments and license notices.

## Regression tests

Configure with `-DCLINESTING_BUILD_TESTS=ON`, build, then run:

```text
ctest --test-dir build-release -C Release --output-on-failure
```

The tests cover repeated contours, scanline rasterization against pixel-center
sampling, hole-first ordering, rotated and concave holes, independent clearances,
pattern spacing, timed strategy comparison, cancellation and GPU failure policy.
They also compare serial/parallel layouts for 1000 copies plus a singleton with one
allowed angle. When an OpenCL GPU is available, hardware tests check fast-path use,
CPU/GPU layout agreement and bitmap word-boundary filtering. Otherwise those
hardware checks print a skip reason; GPU failure-policy tests still run.


### Search modes and subpixel clearance

`mode: "first"` automatically builds one large-first, bottom-left bitmap layout.
Its initial pass tries contact positions, free rectangle corners and existing
hole pockets. A per-sheet refill pass additionally uses a coarse grid and local
refinement, without exhaustive fine-grid fallback. No additional bitmapSearch
input option is needed. This heuristic can still miss feasible pockets.

`mode: "timed"` first retains the same fast layout, then compares different part
orders, permitted orientations and contact/compact strategies until the shared
time limit. `continuous` keeps searching until cancellation. Search rounds after
the initial layout use `continuousRoundSeconds` as their budget. Ranking
prioritizes fewer unplaced parts, then less used-sheet waste, then compactness.
Only improvements replace the incumbent. A finite time budget does not enumerate
all permutations or prove an optimum. `searchIterations` reports attempted
iterations including the initial seed in timed/continuous mode.

After placing parts on each sheet, bounded vector refinement moves
parts down/left using continuous coordinates and exact polygon clearance checks.
It keeps original geometry and rotations, checks sheet margins, stock cutouts,
part outlines and holes, and never reuses stale bitmap occupancy after moving.
The refinement performs at most three sweeps and narrows blocked translations to
0.001 mm; this is a search tolerance, not a CAD manufacturing accuracy guarantee.
The gap is a minimum: not every pair of parts can have exactly that gap.
At 1 mm bitmap resolution, contacting straight edges can therefore approach
6.5 mm clearance instead of retaining a grid-rounded 7 mm gap. No 0.1 mm raster
or NFP backend is needed for this refinement.

Time limits and cancellation also apply during refinement. An interrupted result
retains only already validated moves. Result JSON reports `vectorRefinement`
(checks, moves, elapsed milliseconds, tolerance and completion) and `bitmapSearch`
(`bottom-left` for first; `portfolio` for timed/continuous). Refinement keeps the
original geometry and permitted orientations. The engine then rebuilds occupancy
on the sheet grid, discards stale rejection caches, and tries the remaining copies
on that sheet again. Successful refill/compaction passes repeat until no copy is
added or cancellation/deadline ends the search. `sheetRefill` reports added copies
and successful passes. Only then does the strategy open the next sheet.

Repeated-part NFP and pair-pattern proposals include the configured part spacing;
raw touching-contour vertices are insufficient when that spacing is positive.
Exact geometry remains authoritative. Timed/continuous restarts cycle through
permitted anchor orientations as well as perturbing the part order. With
`rotations: 360`, the permitted orientations are one degree apart; this does not
enumerate every combination of angles, orders and positions recursively.

CPU candidate batches can include multiple rotations while pinning their masks
within the lazy raster cache budget. `threads: 12` supplies at most 12 execution
slots, not a guarantee of constant 12-core utilization: placement commits, ranking
and vector compaction still contain serial work.

`quality_examples` uses `tests/fixtures/{zvaigzne,plaukts,aplis}.json`, checks the
regressed upper strip and panel channels, and compares deterministic results at
1 and 12 threads. The ring fixture is discretized from `aplisArCaurumu.FCStd`
(outer radius 44 mm, inner radius 31 mm, 0.05 mm contour tolerance) with eight
15 x 15 mm inserts. Output is checked by `validate_indexed_layout` using the
exhaustive polygon predicates. Run all checks after configuring tests:

```text
cmake --preset windows-release -DCLINESTING_BUILD_TESTS=ON
cmake --build --preset windows-release --parallel
ctest --test-dir build-release -C Release --output-on-failure
```

Python 3.8 or newer is needed for the protocol and quality-example tests.
