# JSON CLI reference

The complete current user guide, all settings, mode descriptions, examples,
Windows build instructions and limitations are in [README.md](README.md) (English).

FreeCAD exports support settings, sheet outer, per-part rotations, metadata,
and output.resultJson. The three clearances are spacing, partToSheet and
partToHole. An optional CLI-nesting object overrides shared settings and config.
Known legacy settings are ignored; the full list is in the README.

Run `clinesting.exe` with all settings in `input.json`, or use
`clinesting.exe --input path/to/input.json`.

## Current mode contract

- `config.mode: "first"` (default): one compact strategy, then export.
- `config.mode: "timed"`: positive `timeLimitSeconds` required. Restart searches
  until the overall deadline, retaining the best layout across every strategy and restart.
- `config.mode: "continuous"`: restart until Ctrl+C, Ctrl+Break or console close.
  Clear/create `results` **beside the input JSON**, then save only strict improvements.

`continuousRoundSeconds` (default 30) bounds each optimization restart. It is not
a total session limit. In timed mode each restart also respects the remaining global
budget. In continuous mode `timeLimitSeconds` is ignored.
Cancellation is cooperative; in-flight GPU/geometry calls and file output can add time.
Windows console close has an OS-imposed grace period; previous completed files survive.

Legacy inputs without `mode` infer continuous from `continuous:true`, timed from a
positive `timeLimitSeconds`, otherwise first. Conflicting explicit mode/continuous
settings are rejected. Timed mode now runs for the full budget instead of returning
after the initial finite strategies. First mode always uses one strategy.

## Output is configured in JSON

```json
"output": {
  "json": "result.json",
  "dxf": true,
  "svg": true,
  "openPreview": false
}
```

This object is at the root, beside `config` and `parts`.
JSON output is mandatory; `json` is a nonempty path string.
`dxf` and `svg` accept false (disabled), true (result.dxf / result.svg),
or a nonempty path. Both default to false. `openPreview` defaults to false
and requires SVG. On Windows it opens the SVG's associated viewer; continuous mode
opens only the first result. SVG is a visual preview, while JSON/DXF retain the
geometry for integration/CAD.

Relative JSON paths resolve beside the input file. In continuous mode basenames are
ignored: output becomes `results/resultN.json`, plus enabled `.dxf` / `.svg`.
The final JSON is published after companion files and marks a complete result.
The first candidate establishes a baseline; later files require strict quality
improvement. Clear/copy the history intentionally: the next continuous start removes it.

## Geometry and search

Input remains polygon point arrays or point objects in millimetres, with optional holes,
IDs and quantities. A rectangular sheet can use width/height. Angles use `angle`
for one absolute orientation or `allowedAngles` for a permitted list; otherwise
the global rotation grid plus legacy `rotation` offset applies.
GPU is entirely controlled by `config.gpu` (boolean or enabled/device/fallbackToCpu/batchSize
object). `--list-gpus` is an optional device-discovery utility.

The optimization objective is lexicographic: fewer unplaced instances, then smaller
unused material area in used sheets, then smaller unused area in occupied bounding
rectangles. Identical stock/parts have constant physical scrap area; compactness can
still improve. Output reports `usedSheetWasteArea` and `compactWasteArea` separately.

Output points/holes are already transformed; do not rotate/translate them again.
Inspect placed/unplacedCount and unplaced IDs, mode, timeLimitReached, stopReason,
searchIteration, timingMs and GPU diagnostics. Timed GPU counters cover all executed
restarts; continuous snapshots report the candidate strategy. stopReason in a
continuous snapshot describes that candidate, not completion of the entire session.

Optional legacy CLI overrides remain: --output, --dxf, --threads, --trials, --rotations.
Their paths resolve against the working directory. --output layout.dxf writes real
DXF plus layout.json. Input/output collisions, including aliases of existing files,
are rejected.

## Dependencies

Vendored nlohmann/json v3.11.3, commit 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03:
MIT license retained beside the header.
Khronos OpenCL-Headers v2024.10.24, commit 4ea6df132107e3b4b9407f903204b5522fdffcd6:
Apache-2.0 license retained in third_party/opencl/LICENSE.
Clipper2 1.5.4 uses CMake FetchContent.
The driver provides the OpenCL runtime; no CUDA or third-party nesting DLL is needed.
Only Windows has been compiled at this stage. The full runnable example is
[input.json](input.json); see README for every field, alias and valid alternative.

