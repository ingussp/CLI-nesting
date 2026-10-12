# CLI JSON quick reference

Build instructions and the complete input/output reference are in
[README.md](README.md). The runnable full example is [input.json](input.json).

## Minimum input

```json
{
  "parts": [{"points": [[0,0],[20,0],[20,10],[0,10]], "quantity": 4}],
  "sheets": [{"width": 100, "height": 100}]
}
```

Run `clinesting --input input.json` (`clinesting.exe` on Windows). Geometry is
2D, in millimetres. A part requires a point contour; stock can use a contour or
positive width/height. Points are `[x,y]` or `{"x":x,"y":y}`. Holes are separate
contours in `holes`. Unknown fields are errors.

## Search

Use one root `config` object:

```json
"config": {
  "mode": "continuous", "timeLimitSeconds": 60,
  "threads": 4, "resolution": 1,
  "spacing": 2, "partToSheet": 2, "partToHole": 2,
  "gpu": false
}
```

- `first`: produce one initial layout and export.
- `continuous`: search until Ctrl+C, cancellation or `timeLimitSeconds`;
  `0` means unlimited. Clear `results/` beside the input, then
  publish improving numbered snapshots and update the latest result JSON.

Part `rotations` selects a uniform orientation count; default 4. Use `angle` or
`allowedAngles` for absolute permitted orientations. Stock orientation is fixed.
The three clearance settings are independent and do not modify exported contours.

## Output and cancellation

```json
"output": {
  "json": "result.json", "dxf": true, "svg": true,
  "cancelFile": ".cancel-current-run"
}
```

JSON output is always enabled. Relative output/cancel paths resolve beside the
input. Creating the cancel marker requests a graceful stop; remove old markers
before starting. Ctrl+C also stops the process. Keep input, latest output and
cancel marker outside the continuous `results/` history directory.

CLI overrides `--output`, `--dxf`, `--svg` and `--threads` take
precedence; their paths resolve against the working directory. `--list-gpus`
prints devices without reading input. GPU mode requires an OpenCL driver;
CPU fallback is enabled by default.

`job_id` and `created_at` strings are retained unchanged in every result.
Part/sheet `filename` stores the source file name; the CLI uses supplied points
and does not import that file.

Check `placed`, `unplacedCount`, `unplaced`, `sheets`, `mode`, `stopReason`,
`timeLimitReached` and `gpu` in the result. Points/holes are already transformed.
Exit code 0 can include unplaced parts. Numbered continuous JSON files are completed
snapshots. The latest JSON receives the final stop reason (`time_limit` or
`user_stop`) at shutdown without adding a duplicate history entry.
