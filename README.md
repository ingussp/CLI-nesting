# CLI-nesting

C++20 command-line nesting for polygon parts and polygon sheets, with holes,
per-part orientations, independent clearances, OpenCL acceleration, and JSON,
DXF and SVG output. The executable is **clinesting.exe** on Windows and
**clinesting** on Linux and macOS.

Build targets: **Windows x64**, **Linux x64**, **Linux ARM64 (aarch64)**, and
**macOS Intel + Apple Silicon**. The macOS Universal 2 executable contains both
architectures. CPU nesting does not require an OpenCL runtime.

The [GitHub Actions workflow](.github/workflows/tests.yml) builds and runs the
regression suite on Windows, both Linux architectures, and both macOS
architectures. macOS builds use Xcode 16.4 and target macOS 13.0 or newer.

## Build on Windows using CMake

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

To run the full Windows regression suite, also install Python 3, then use:

```text
cmake --preset windows-release -DCLINESTING_BUILD_TESTS=ON
cmake --build --preset windows-release --parallel 2
ctest --preset windows-release
```

## Build on Linux x64 or native Linux ARM64

These commands use Ubuntu 22.04 or newer (including Ubuntu in WSL2). Install
GCC 11 or newer with C++20 support, CMake 3.21 or newer for presets, Ninja,
Git and Python 3. Python is needed only for the complete regression suite.
On other distributions, install the equivalent packages with their package
manager.

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git python3

git clone https://github.com/ingussp/CLI-nesting.git
cd CLI-nesting
cmake --version
c++ --version
uname -m

cmake --preset linux-release
cmake --build --preset linux-release --parallel 2
./build-linux-release/clinesting --help
./build-linux-release/clinesting --input examples/holes-first.json
```

The `linux-release` preset uses the native compiler: on an x86_64 machine it
produces Linux x64, and on an aarch64 machine it produces Linux ARM64. It does
not change architectures automatically. `--parallel 2` limits concurrent
compiler processes; increase it if the build machine has enough RAM.
The executable is `build-linux-release/clinesting`. The example above writes
JSON and SVG beside its input in `examples/`.

Equivalent commands without presets (CMake 3.20 or newer):

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel 2
./build-linux/clinesting --input examples/holes-first.json
```

To enable and run every regression test with the native preset:

```bash
cmake --preset linux-release -DCLINESTING_BUILD_TESTS=ON
cmake --build --preset linux-release --parallel 2
ctest --preset linux-release
```

### Build Linux binaries from Windows 11 with WSL2

In an **administrator PowerShell** window, install Ubuntu and restart when
requested. If WSL is already installed, use your existing Ubuntu distribution.

```powershell
wsl --install -d Ubuntu-22.04
wsl -d Ubuntu-22.04
```

The second command opens a Linux shell. Run the Linux dependency installation,
clone and build commands above **inside that shell**, using Linux CMake and
GCC. On a Windows x64 computer this produces Linux x64. For better filesystem
performance, keep a personal WSL checkout in the Linux filesystem rather than
under `/mnt/c`. Windows can access it through `\\wsl.localhost\Ubuntu-22.04`.
An `.exe` built by Visual Studio remains a Windows binary; WSL builds produce
Linux ELF executables.

### Cross-compile Linux ARM64 from Linux x64 or WSL2

Use the supplied Debian/Ubuntu AArch64 toolchain when the build machine is x64:

```bash
sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

cmake --preset linux-arm64-cross-release
cmake --build --preset linux-arm64-cross-release --parallel 2
file build-linux-arm64-cross-release/clinesting
```

The output is `build-linux-arm64-cross-release/clinesting`; `file` should report
an ARM aarch64 ELF executable. Equivalent direct configuration:

```bash
cmake -S . -B build-linux-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64.cmake
cmake --build build-linux-arm64 --parallel 2
```

An ARM64 executable cannot run directly on an x64 host. Copy it to an ARM64
Linux machine, keep its executable permission (`chmod +x clinesting` if needed),
and run `./clinesting --input /path/to/input.json`. For the full regression
suite, use the native Linux ARM64 build instructions there; CI also tests on a
native ARM64 runner. The cross preset deliberately has no test preset.

### Linux runtime compatibility and optional GPU acceleration

The default build links the system C/C++ runtime dynamically. CI uses Ubuntu
22.04 for both Linux architectures; the binaries require compatible glibc and
libstdc++. They are not universal binaries for every Linux distribution or for
Alpine/musl. Build on the oldest distribution you intend to support, then test
on your target systems. Inspect dependencies with `ldd ./clinesting`.
`CLINESTING_STATIC_RUNTIME` controls MSVC only and does not make Linux binaries
static.

CPU-only jobs work without an OpenCL installation. To use `gpu: true`, install
an OpenCL ICD loader (on Ubuntu: `sudo apt install ocl-icd-libopencl1`) **and**
the appropriate vendor runtime/driver for the actual GPU and CPU architecture.
The loader alone does not provide a GPU driver. Check available devices with
`./build-linux-release/clinesting --list-gpus`. GPU passthrough in WSL depends
on the installed driver/runtime; start with CPU-only jobs there.

## Build on macOS (Intel and Apple Silicon)

Build on a Mac or use the macOS GitHub Actions runners. A normal Windows or
Linux CMake installation does not supply the Apple SDK/toolchain.
Install Xcode Command Line Tools, then CMake, Ninja and Python 3 with
[Homebrew](https://brew.sh/). Xcode 16.4/its Command Line Tools are the CI
toolchain; a newer compatible Apple Clang toolchain can also be used.
The project uses C++20 but does not require the newer libc++ `std::jthread`
implementation or experimental standard-library flags.

```bash
xcode-select --install
brew install cmake ninja python

git clone https://github.com/ingussp/CLI-nesting.git
cd CLI-nesting
xcrun --find clang++
cmake --version

cmake --preset macos-universal-release
cmake --build --preset macos-universal-release --parallel 2
lipo build-macos-universal-release/clinesting -verify_arch arm64 x86_64
lipo -info build-macos-universal-release/clinesting
./build-macos-universal-release/clinesting --input examples/holes-first.json
```

Complete the Command Line Tools installation dialog before running the build.
If using full Xcode instead, select its developer directory with
`sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer`.

The executable is `build-macos-universal-release/clinesting`. It contains
**arm64 and x86_64** slices and runs natively on both processor families, with
no Rosetta requirement. The bundled Clipper2 dependency is compiled for both
architectures in the same build. The default minimum deployment target is
**macOS 13.0**; compilation on a newer macOS version does not itself test older
systems. Test on the oldest macOS version you plan to distribute to.

Equivalent commands without presets (quote the architecture list so the shell
does not treat the semicolon as a command separator):

```bash
cmake -S . -B build-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build build-macos --parallel 2
lipo -info build-macos/clinesting
```

To build a single architecture, use `macos-arm64-release` or
`macos-x64-release` for both configure and build. Their executables live in
`build-macos-arm64-release/clinesting` and `build-macos-x64-release/clinesting`.
Use separate build directories/presets when changing architectures. You can
override the minimum macOS version at initial configuration, for example
`cmake --preset macos-universal-release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0`.

Enable and run the regression suite:

```bash
cmake --preset macos-universal-release -DCLINESTING_BUILD_TESTS=ON
cmake --build --preset macos-universal-release --parallel 2
ctest --preset macos-universal-release
```

Tests execute the slice matching the current Mac. CI builds Universal 2 and
runs the suite on both an Apple Silicon runner and an Intel runner, then checks
that the CLI contains both slices. Inspect runtime libraries with
`otool -L build-macos-universal-release/clinesting`.

For CPU-only use, leave `gpu` false (the default). GPU mode loads the system
OpenCL framework at runtime. Apple has deprecated OpenCL; device availability
and kernel support must be tested on the actual Mac. `--list-gpus` reports the
available devices. CPU fallback remains available unless `fallbackToCpu` is
false. Building/testing the CPU executable does not certify GPU support.

On Linux/macOS, `output.openPreview` prints the saved SVG path. Open the SVG
manually, for example `open examples/result.svg` on macOS or
`xdg-open examples/result.svg` on a Linux desktop.

## Download all platform builds using GitHub Actions

From Windows, you can obtain every platform build without installing its local
toolchain:

1. Push to `main`, push a `v*` version tag, or open a pull request. The workflow
   runs automatically.
   After the workflow is on the default branch, **Actions → Cross-platform
   builds and regression tests → Run workflow** also starts a manual build.
2. Open the workflow run and wait for all five platform jobs to pass.
3. Download the four artifacts: `clinesting-windows-x86_64`,
   `clinesting-linux-x86_64`, `clinesting-linux-aarch64`, and
   `clinesting-macos-universal2`.
4. Extract the artifact ZIP, then its `.tar.gz` archive. Each archive contains
   the CLI, examples, documentation and third-party license notices. On Windows
   11, extract it with `tar -xzf clinesting-windows-x86_64.tar.gz`; on Linux/macOS,
   use the same command with the corresponding archive name. The tar archive
   preserves the Unix executable permission.

The single macOS artifact is produced on Apple Silicon; the Intel job also
builds and tests Universal 2. CI uses CPU regressions, with GPU checks running
only when an eligible GPU is present. Artifacts are build outputs, not automatic
GitHub Releases, and the workflow does not sign/notarize macOS distributions.

## Run a job

The CLI reads polygon geometry from JSON. All coordinates and clearances are in
millimetres. All search settings belong in the root `config` object.

```text
clinesting --input input.json
clinesting --input input.json --dxf result.dxf --svg result.svg
clinesting --list-gpus
```

On Windows, use `clinesting.exe` or `run.cmd`. On Linux/macOS, use the executable
path from the build instructions above. Without `--input`, the CLI reads
`input.json` in the working directory.

Examples: [complete input](input.json), [holes](examples/holes-first.json),
[first layout](examples/mode-first.json), [continuous with a deadline](examples/continuous-limited.json),
[continuous search](examples/mode-continuous.json),
[0.1-degree rotations](examples/rotation-0.1.json).

## Input JSON

```json
{
  "units": "mm",
  "config": {"mode": "first", "spacing": 2, "partToSheet": 2},
  "sheets": [{"id": "stock", "width": 100, "height": 100}],
  "parts": [{
    "id": "panel", "quantity": 4, "rotations": 1,
    "points": [[0,0],[20,0],[20,10],[0,10]]
  }],
  "output": {"json": "result.json", "dxf": true, "svg": true}
}
```

JSON keys and string values are case-sensitive. Use numbers without quotes,
decimal points, and `true`/`false`. Comments and trailing commas are invalid.
Unknown root, geometry, config, GPU and output fields are rejected.

| Root field | Requirement/default | Purpose |
|---|---|---|
| `parts` | Required nonempty array | Part contours, orientation rules and quantities |
| `sheets` | Required nonempty array, or use `sheet` | Available stock contours/dimensions and quantities |
| `sheet` | Single-sheet alternative | One stock object; `sheets` takes precedence if both are supplied |
| `units` | `mm` | Only millimetres are supported |
| `config` | Engine defaults | Search, precision, clearance and GPU settings |
| `output` | JSON to `result.json` | Result paths, export formats and stop marker |
| `job_id` | String, omitted by default | Copied unchanged to every result and written to the progress log |
| `created_at` | String, omitted by default | Copied unchanged to every result |

### Geometry

| Part/sheet field | Requirement/default | Purpose |
|---|---|---|
| `points` | Required for parts; sheets may use dimensions | Ordered outer contour |
| `outer` | Alternative to `points` | Same contour format; if both are supplied, their values must match |
| `holes` | Empty array | Hole contours, each a point array or an object with `points`/`outer` |
| `width`, `height` | Positive numbers | Rectangle shorthand for stock without a contour |
| `x`, `y` | 0 | Origin for rectangular stock |
| `id` | String/integer or generated | Part/sheet identity retained as `source` in results |
| `name` | Used when `id` is absent | Fallback part/sheet identity |
| `quantity` | 1 | Part copies: 1..100000; sheet copies: 1..1000 |
| `count` | Alternative to part `quantity` | Used only when `quantity` is absent |
| `filename` | Empty string | Source file name; geometry still comes from supplied points |
| `rotations` | 4 | Part orientations on a uniform grid, integer 1..3600 |
| `rotation` | 0 degrees | Part offset added to that uniform grid |
| `angle` | Unset | One absolute permitted part orientation |
| `allowedAngles` | Unset | 1..3600 absolute permitted part orientations |

Points are `[x,y]` arrays or `{"x":x,"y":y}` objects. Input is strictly 2D.
Each contour requires 3..20000 points with nonzero area. Use simple,
non-self-intersecting contours. Consecutive duplicates, a closing copy of the
first point and exactly repeated complete contour cycles are normalized.
Holes must lie within their outline, must not overlap, and must leave positive
material area. One hole level is supported. Contour winding does not define holes.

Coordinates must be finite and within +/-1000000 mm. Input files are limited
to 64 MiB; expanded totals are limited to 100000 parts and 1000 sheets.
Part type IDs must be unique. Without `id` or `name`, identities become `part_1`, `sheet_1`, etc.
Each physical copy receives a numeric result ID.

Angles are counterclockwise degrees. `rotations: 4` permits offset + 0/90/180/270;
`rotations: 1` permits the offset only. `angle: 45` permits exactly 45 degrees;
`allowedAngles: [0,90]` permits those two orientations. Absolute angles are
normalized to [0,360), and duplicates are removed. Do not combine `rotations`
with absolute angles, `angle` with `allowedAngles`, or absolute angles with a
nonzero `rotation`. Sheets have fixed orientation and reject rotation fields.

Curves must be approximated by points before running the CLI. `filename` does
not import a file. There is no DXF/STEP import, 3D transformation, automatic kerf
compensation or toolpath generation.

## Search settings

Every field below belongs inside `config`.

| Field | Default | Values and purpose |
|---|---|---|
| `mode` | Inferred, normally `first` | `first`, `continuous` |
| `timeLimitSeconds` | 0 | 0..86400; continuous total budget in seconds, 0 = unlimited; first requires 0 |
| `continuousRoundSeconds` | 30 | 0.01..86400; initial-layout snapshot interval; placement continues between snapshots, bounded only by cancellation or the total time limit |
| `threads` | Logical CPU count | Integer 1..256; shared CPU worker budget |
| `resolution` | 1 mm/pixel | Positive value up to 1000000; smaller values increase raster detail and memory |
| `step` | 1 pixel | Integer 1..100000; translation step is `step * resolution` |
| `curveTolerance` | 0.3 mm | 0..1000000; simplify contact proposals; original contours remain unchanged |
| `cacheRejects` | true | Reuse rejected origins and failed contact searches |
| `cacheMemoryMiB` | 256 | Integer 1..4096; shared rejection-cache budget |
| `spacing` | 0 mm | Gap between outer boundaries of parts |
| `partToSheet` | 0 mm | Margin from parts to the stock outline |
| `partToHole` | 0 mm | Clearance to stock cutouts and between boundaries involving part holes |
| `gpu` | false | Boolean or OpenCL configuration object |
| `reusableOffcut` | Enabled, minimum width 5 mm | Boolean or `{ "enabled":true, "minWidthMm":5 }`; report reusable material before continuous result publication |

The three clearances are independent. Zero allows touching, not overlap.
Positive values reject contact. Original contours are exported unchanged.
`sheetSpacing` and `holeSpacing` are accepted alternatives to `partToSheet` and
`partToHole`; conflicting values are errors. `bitmapResolutionMm` and
`bitmapSearchStepPx` are alternatives to `resolution` and `step`; use one spelling.

### Modes

- `first`: build one fast initial layout, compact/refill stock and export once.
- `continuous`: search until Ctrl+C, a cancellation request or the total
  `timeLimitSeconds` budget expires. A limit of `0` means unlimited.
  Clear/create `results/` beside the input,
  then save only improving `result1.json`, `result2.json`, etc., with enabled
  DXF/SVG companions. Also replace `output.json` with the latest complete snapshot.

Without `mode`, a positive time budget selects continuous search; otherwise the
default is first. `continuous: true` also selects continuous when mode is absent;
when both are supplied, they must agree.

Before starting continuous mode, back up old results and keep the input, latest
snapshot and cancel marker outside `results/`. A lock prevents simultaneous
sessions from clearing the same directory. A numbered JSON is published after
its companions and marks a complete snapshot. Temporary or companion-only
files are incomplete.

Ctrl+C requests a graceful stop; Linux/macOS also accept SIGTERM, and Windows
accepts Ctrl+Break. Windows console close allows only a limited shutdown period.
The continuous budget includes the initial layout, preparation and all CPU/GPU
workers. If the finite search space is exhausted early, the session waits for
its deadline or a cancellation request.

Cancellation and deadlines are cooperative: active GPU/geometry calls and output
can add time. Completed snapshots remain available after a stop.

The initial pass retains its masks, occupancy and remaining-copy cursor across
`continuousRoundSeconds` snapshots. A snapshot can be partial: `placementInProgress`
and `stopReason: "in_progress"` distinguish this from a finished pass. Only the
total deadline or cancellation interrupts that pass. Missing copies receive a
non-destructive refill attempt before neighbourhood compaction/repair.

Offcut evaluation and file publication run on one reporting thread outside the
incumbent lock. At most one newest pending improvement is retained, so reporting
can skip superseded intermediate candidates without blocking placement. Saved
layouts remain ordered improvements and the final pending result is drained on
shutdown. Reporting and active geometry calls can extend shutdown beyond the
search deadline. The reporting thread is additional to the search worker budget.

The GPU keeps initial-layout occupancy resident and updates it on-device after
each accepted placement. `gpuOccupancy.uploadBytes` and `deviceUpdates` report
these transfers/updates for the selected bitmap strategy. Exact CPU validation
remains authoritative; recursive validation uses a removable neighbour index
with clearance-expanded queries, including during backtracking.

The objective prioritizes fewer unplaced copies, then less unused material in
used sheets, then smaller occupied bounds and less placement spread. Search is
heuristic; check `unplacedCount` even when the process exits successfully.
Continuous offcut measurement reports the largest connected usable remnant
after excluding clearance bands and applying its minimum-width filter. It does
not change the placement objective. `minWidthMm` must be positive and <=1000000.

### OpenCL GPU

```json
"gpu": {"enabled":true, "device":-1, "fallbackToCpu":true, "batchSize":65536}
```

| Field | Default inside object | Purpose |
|---|---|---|
| `enabled` | true | Enable OpenCL filtering |
| `device` | -1 | -1 selects automatically; otherwise use an index from `--list-gpus` (0..1024) |
| `fallbackToCpu` | true | Continue on CPU if GPU initialization/execution fails |
| `batchSize` | 65536 | Integer 256..262144; maximum candidates in one GPU batch |

`gpu: true` uses these defaults. CPU mode requires no OpenCL runtime. GPU mode
requires the appropriate loader and vendor driver; see the platform build
instructions. GPU filters bitmap collisions; CPU handles proposals, exact
geometry validation and commits. Short jobs may be slower on GPU due to startup
and transfer costs. Inspect result `gpu.used` and `gpu.fallbackReason`.

## Output

`output` is a root object alongside `config`, `parts` and `sheets`.

| Field | Default | Purpose |
|---|---|---|
| `json` | `result.json` | Nonempty JSON result path; JSON output cannot be disabled |
| `dxf` | false | false disables; true uses `result.dxf`; a string specifies the path |
| `svg` | false | false disables; true uses `result.svg`; a string specifies the path |
| `openPreview` | false | Requires SVG; open the Windows viewer or print the SVG path on Linux/macOS |
| `cancelFile` | Unset | Nonempty stop-marker path; creating the file requests cooperative cancellation |

JSON-configured paths resolve beside the input file. Input, outputs, temporary
publication files and the stop marker must refer to distinct files. First
creates parent directories and overwrites existing outputs. Continuous uses
fixed numbered history names and maintains the configured latest JSON path.
Choose a fresh cancel-marker path for each run and remove an old marker before
starting. Creating it works even when the process has no interactive console.

DXF is a CAD exchange file. Its sheets are arranged horizontally with 20 mm
gaps, with part contours, holes and labels translated together. JSON retains the
original stock coordinates. SVG is a colored preview.

### Result fields

| Field | Meaning |
|---|---|
| `schemaVersion`, `units` | Result format version 1, millimetres |
| `job_id`, `created_at` | Supplied values copied unchanged |
| `placed`, `unplacedCount`, `unplaced` | Copy counts and unplaced IDs/sources |
| `sheets[].id`, `source`, `points`, `holes` | Used stock identities and contours |
| `sheets[].parts[]` | Copy ID/source, translation `x`/`y`, angle `rotation`, transformed points/holes |
| `mode`, `timeLimitReached`, `stopReason` | Search mode and candidate stop status |
| `timingMs`, `searchIteration`, `searchIterations` | Timing and search progress |
| `clearances` | Applied gaps and margins |
| `utilisation`, `usedSheetWasteArea`, `occupiedBoundsArea`, `compactWasteArea` | Stock use and compactness |
| `reusableOffcut` | Usable remnant area/contour and measurement status |
| `gpu`, worker/cache/strategy counters | Runtime diagnostics |

Result points and holes are already transformed; do not apply the reported
translation/rotation again. Numbered continuous snapshots describe improving
candidates. At shutdown, the
latest `output.json` is updated with final counters and `stopReason: time_limit`
or `user_stop`; numbered history remains unchanged. `timeLimitReached` is true
only when the total deadline caused the stop. Monitor the process separately.

## Command-line arguments

| Argument | Purpose |
|---|---|
| `--input path` | Input JSON, default `input.json` |
| `--output path` | Override JSON path; a `.dxf` path enables DXF and a companion JSON |
| `--dxf path` | Enable/override DXF export |
| `--svg path` | Enable/override SVG export |
| `--threads N` | Override CPU budget, integer 1..256 |
| `--list-gpus` | Print OpenCL devices and exit |
| `-v`, `-version`, `--version` | Print program version and exit without reading input |
| `--help`, `-h` | Print usage and exit |

CLI paths resolve against the working directory and override JSON paths.
JSON is validated before overrides. Exit code 0 means completion or graceful
stop, including partial layouts; code 1 means an input/runtime error.

Check the installed program version on Windows with `clinesting.exe -v` or
`clinesting.exe -version`. Version 1 prints `clinesting 1`. On Linux/macOS, use
`clinesting -v` or `clinesting -version`. The version is set by `project(... VERSION ...)`
in `CMakeLists.txt` and embedded in the compiled CLI.

## Dependencies and licenses

| Dependency | Version/purpose | License |
|---|---|---|
| [Clipper2](https://github.com/AngusJohnson/Clipper2) | 1.5.4, polygon operations | [Boost Software License 1.0](https://github.com/AngusJohnson/Clipper2/blob/Clipper2_1.5.4/LICENSE) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3, JSON parser/writer | [Included MIT license](third_party/nlohmann/LICENSE.MIT) |
| [OpenCL-Headers](https://github.com/KhronosGroup/OpenCL-Headers) | v2024.10.24, OpenCL declarations | [Included Apache-2.0 license](third_party/opencl/LICENSE) |

Retain these notices when distributing the source or executables. The GPU
runtime is supplied separately by the driver. Test commands for each platform
are included in the build instructions above.
