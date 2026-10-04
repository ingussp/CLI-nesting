# Indexed vector refinement

Bitmap placement is followed by continuous-coordinate refinement at a 0.001 mm
search tolerance. The previous implementation performed every contour-edge pair
comparison repeatedly, which dominated the runtime for finely tessellated parts.

The refinement now uses three complementary geometry checks:

- A per-contour edge BVH selects nearby edge pairs and retains the exact segment
  distance predicate. Only moved parts rebuild their index.
- Convex parts without holes use cached Minkowski differences (convex NFPs) for
  repeated shape/orientation pairs. Bit-exact shape identities and a conservative
  numerical band avoid merging different geometry or approximating the final gap.
  Unsupported shapes and ambiguous queries fall back to the indexed predicates.
- Rectangular sheets without holes use bounding extents for their margin test.
  Queries near the boundary and all other stock shapes retain the full checks.

Caches belong to one sheet refinement. The convex cache is bounded to 128 shapes
and 256 pairs, accepts contours up to 400 vertices, and uses the general index
outside these limits. Zero or tiny margins also use the general predicates.

The search order, three refinement passes, exact exported contours, rotation
rules and clearance settings are unchanged. This accelerates the serial work;
it does not move dependent parts concurrently or add GPU dependence.

## Measured job

Release MSVC build, first mode, GPU disabled, 12 requested CPU workers, 1 mm bitmap,
6.1 mm part clearance and 10 mm sheet clearance. The job requests 1000 copies of
one convex 148-vertex part with 0/180 degree orientations on one 2000 x 2800 mm
sheet. Baseline commit: `6675a45fb1f6a6d924fd5792293608ab4d4141dc`.

Final measurements were run sequentially on the same machine after compilation:

| Refinement variant | Total engine time | Vector refinement |
| --- | ---: | ---: |
| Original | 44.268 s | 43.002 s |
| Edge index | 3.392 s | 2.026 s |
| Edge index + convex pair cache | 2.780 s | 1.526 s |
| Above + rectangular stock shortcut | 1.536 s | 0.272 s |
| Final variant, repeated | 1.687 s | 0.279 s |
| Final variant, one CPU worker | 2.384 s | 0.278 s |

All variants returned identical positions, rotations and unplaced entries:
540 placed, 460 unplaced, 40,468 vector probes and 2,032 committed moves.
The final result was also checked with the original exhaustive clearance
predicates: 540 sheet checks and 892 nearby part pairs, no clearance violations.
The supplied job geometry is not included in the repository. These timings are
case-specific, especially the convex-cache improvement.

## Validation

Build with `CLINESTING_BUILD_TESTS=ON` and run `ctest --output-on-failure`.
The existing regression and workbench protocol suites are retained. Added suites
compare the fast checks against the original predicates across concavity, holes,
rotations, translated coordinates and fractional margin thresholds:

- 9,642 edge-index comparisons.
- 3,000 rectangular-stock comparisons.
- 8,003 convex-cache answers plus four boundary-band fallbacks.

`validate_indexed_layout input.json result.json` independently rechecks an
exported layout using the original exhaustive predicates, without the new index
or NFP fast paths.
