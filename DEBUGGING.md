# Debugging log: identifying `SceneVelocity` in a stripped Shipping build

This file documents how the target buffer was actually found — including a
mistake that cost several rounds of analysis, because how that mistake
survived scrutiny for so long is the most useful thing in this project.

Target: UE5.7.1 game ("Oxi"), D3D12, packaged **Shipping** (deliberately —
Shipping strips resource debug names, preserving the "identify render targets
with no names" problem a real commercial title presents).

---

## The headline mistake: one wrong enum digit

The velocity buffer's signature was transcribed from an early RenderDoc
capture as:

```
1212x760, DXGI_FORMAT_R16G16B16A16_FLOAT   // = 10
```

The actual format is:

```
1212x760, DXGI_FORMAT_R16G16B16A16_UNORM   // = 11  (UE5's PF_A16B16G16R16)
```

Every filter in the project used `_FLOAT` for most of its life. Fixing that
one constant took the candidate set from *"4 indistinguishable resources,
avenue exhausted"* to **exactly one uniquely-matching resource**, using log
data that had **already been captured days earlier**. No new instrumentation,
no new risk. The answer had been sitting in the log the whole time, filtered
out by a single digit.

### Why it survived so long

The wrong filter did not fail loudly. It failed *plausibly*:

- `R16G16B16A16_FLOAT` at exactly render resolution is a real, populated
  category in a deferred renderer — it matched a pool of 4 HDR render targets.
- Those 4 behaved **exactly like a velocity buffer should**: cycling
  `RENDER_TARGET → UNORDERED_ACCESS → NON_PIXEL_SHADER_RESOURCE` every frame,
  unconditionally, with zero deviation across 301,362 sampled frames.
- So every result read as *"the search is working, it just needs one more
  discriminator"* rather than *"the search is looking for the wrong thing."*

Three rigorous, individually-correct analyses were performed **on the wrong
candidate set** before the premise itself was questioned.

### The signal that should have triggered re-deriving the premise

Attempt #3 (below) found that all 4 remaining candidates were **byte-identical
in every single descriptor field** — mips, array size, sample count, flags,
layout.

Four *indistinguishable* resources is the signature of a **pool**. The target
was never described as pooled. That was direct evidence the filter was
selecting a *category* rather than a *specific buffer* — and it was read
instead as "need a better tiebreaker."

**Lesson: when a search narrows to N identical things, suspect the search key
before building a better tiebreaker.** And re-verify transcribed constants
against primary sources early — a wrong enum value is indistinguishable from
"needs more analysis" for a very long time.

---

## The narrowing sequence

| # | Method | Result |
|---|--------|--------|
| 0 | Width/height match at render resolution | thousands → 10 |
| 1 | State-transition family (barrier log) | 10 → 4 |
| 2 | Intra-frame CPU timestamp ordering | **negative** — no signal exists |
| 3 | Full `GetDesc()` descriptor metadata | **negative** — closes the avenue |
| 4 | **Corrected format constant** | 10 → **1** ✅ |

### Attempt 1 — state transitions (worked, but for the wrong reason)

Grouping ~1M logged barrier events by transition pattern split the 10 into two
families: 4 cycling `RENDER_TARGET(4) → UNORDERED_ACCESS(8) →
NON_PIXEL_SHADER_RESOURCE(64)`, and 2 that **never** entered
`UNORDERED_ACCESS`. Since RenderDoc showed the velocity buffer is read at
compute slot `cs1`, the latter group was ruled out as "can't be a compute
read."

**That reasoning was wrong**, and the final verified result proves it. The
real `SceneVelocity` cycles `RENDER_TARGET(4) ↔ ALL_SHADER_RESOURCE(192)`
exactly twice per frame and **never enters `UNORDERED_ACCESS` at all** — the
very pattern this step excluded.

The error: "bound at compute slot `cs1`" means the buffer is read by a compute
shader **as an SRV** (a `t` register), not written as a UAV (a `u` register).
A compute-shader *read* requires `NON_PIXEL_SHADER_RESOURCE` state, not
`UNORDERED_ACCESS`. "Compute" was silently conflated with "UAV."

Note also that the real buffer *does* carry `ALLOW_UNORDERED_ACCESS`
(`flags=5`) — it is simply never transitioned into that state in practice.
**A resource flag advertises a capability; it does not imply the capability is
exercised.** Filtering on observed *states* and filtering on declared *flags*
are different tests, and conflating them is what made the exclusion look
sound.

So this step "worked" only in the sense that it narrowed the set — it narrowed
it in the wrong direction, and would have discarded the true target had the
target ever been in the candidate set to begin with (it wasn't, because of the
format bug below). Two independent premise errors were masking each other.

### Attempt 2 — intra-frame timing (clean negative)

Hypothesis: if the 4 are processed in a fixed pipeline order, the relative
order of their first barrier each frame might single one out.

Measured: all 4 fire within the same ~1ms window every frame, in **randomly
jittering order** — no consistent leader or trailer.

Root cause, and why it's informative rather than just a failure: the hook
timestamp records when the **CPU thread called `ResourceBarrier`**, not GPU
execution order. UE5 batches an entire resource group's barriers into one
tight CPU-side loop before submitting the command list, so they land in the
same millisecond *by construction*. **There is no ordering signal to extract
from CPU-side barrier timestamps for resources transitioned in one batch.**

### Attempt 3 — descriptor metadata (definitive negative)

The once-per-resource `GetDesc()` call already fetched the full
`D3D12_RESOURCE_DESC`, but only width/height/format were being logged.
`MipLevels`, `DepthOrArraySize`, `SampleDesc.Count`, `Flags` and `Layout` were
free reads — zero new API calls, same proven-safe one-shot pattern.

Result: all 4 candidates identical (`mips=1, depthOrArray=1, sampleCount=1,
flags=5, layout=0`). This closed the entire "identify by descriptor metadata"
approach rather than leaving it untried — and, as noted above, was the missed
clue pointing at the real problem.

### Attempt 4 — the fix

A later RenderDoc capture showed the actual creation call, with names intact:

```
ID3D12Device::CreatePlacedResource(TransientResourceAllocator Backing Heap,
    { 2212, 1348, 1, DXGI_FORMAT_R16G16B16A16_UNORM })
  Flags        ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS
  InitialState RENDER_TARGET
ID3D12Resource::SetName(SceneVelocity)
```

`10 = _FLOAT` / `11 = _UNORM` verified directly against `dxgiformat.h` (not
from memory — an earlier finding in this project was a pair of wrong
`D3D12_RESOURCE_STATES` values guessed rather than checked).

Re-grepping the **existing** log for `1212x760 fmt=11` returned exactly one
resource, matching RenderDoc's descriptor in every field:

```
2296540017440  1212x760  fmt=11  mips=1  depthOrArray=1  sampleCount=1
               flags=5 (RT|UAV)  layout=0 (UNKNOWN)
```

### Confirmation in a fresh process

Rebuilt with the corrected filter, restarted the game, and re-injected into a
clean process (exactly one `all hooks installed` line — see the double-hook
hazard below). Result:

```
2789178292384  1212x760  fmt=11  mips=1  depthOrArray=1  sampleCount=1
               flags=5 (RT|UAV)  layout=0 (UNKNOWN)
```

**Exactly one match**, descriptor-identical to the previous session's (the
pointer differs because it's a fresh process). Re-checked across all six logged
sessions: **exactly one candidate pointer per session**, never two.

Its observed barrier pattern, taken over one 53,324-frame session rather than
quoted as an average:

```
frames = 53,324        barrier events = 106,648
events-per-frame histogram: {2: 53324}    <- every frame, exactly 2. No exceptions.
(RENDER_TARGET(4) -> ALL_SHADER_RESOURCE(192)): 53,324
(ALL_SHADER_RESOURCE(192) -> RENDER_TARGET(4)): 53,324
state-continuity violations (before != previous after): 0
no UAV barriers at all
```

Stating it as an invariant rather than a mean matters. "63,553 events over
31,830 frames" averages to 1.996 and invites the question of what the variance
was; "53,324 of 53,324 frames had exactly 2, strictly alternating, zero
exceptions" is a universal that had 53,324 opportunities to be falsified. One
3-barrier frame or one continuity violation would have killed the
single-producer/single-consumer hypothesis. None occurred.

That reads exactly as expected for a velocity buffer: written by the velocity
pass as a render target, then read as an SRV by motion blur / TSR, once per
frame, every frame. The runtime behaviour independently corroborates the
static descriptor match — different mechanism, different failure modes. The
format bug below corrupted the static path and could not have corrupted the
dynamic one in the same way.

**Where this evidence stops.** The barrier analysis only ran on resources that
had already passed the descriptor filter (`LogTo("candidates", …)` is gated on
`IsCandidate`). So it shows the selected resource behaves like velocity; it
does not show that no *other* resource in the process shares the signature.
Logging transition patterns for all ~8,000 resources the identify pass saw, and
showing that a strict 2-cycle `RT ↔ ALL_SHADER_RESOURCE` is itself rare, would
turn this into a standalone identification technique that needs no RenderDoc
capture to crib a descriptor from. That is the version that transfers to a
title you don't own, and it is the obvious next experiment.

---

## Corroborating detail from the creation call

**Byte size independently confirms the format.** RenderDoc reported
`Initial Contents (24156160 bytes)` at 2212x1348. R16G16B16A16 is 8 bytes per
pixel; `2212 * 8 = 17,696` bytes/row, aligned up to D3D12's 256-byte row-pitch
requirement = `17,920`; `17,920 * 1348 = 24,156,160` — **exact**. This confirms
all four 16-bit channels are present (ruling out a 2-channel R16G16 velocity
variant) and gives the readback sizing formula.

Note that `rowPitch * height` is the right formula for RenderDoc's "Initial
Contents", which reports the whole padded allocation — but it is *not* the size
`GetCopyableFootprints` reports for a copy, because the last row is not padded.
Mixing the two costs a row of padding.

For the live Shipping resolution 1212x760: `1212*8 = 9,696` → aligned `9,728`,
and the copy size is `9,728 * 759 + 9,696 = ` **7,393,248 bytes** — which is
what `meta.txt` reports and what the dumped `vel_*.bin` files actually are.
(An earlier revision of this document said 7,393,280 here, having applied
`rowPitch * height` three paragraphs after stating why that is wrong. A
32-byte error, but in a document whose whole thesis is "re-derive your
constants from a primary source", so it is recorded rather than quietly
patched.)

**It is a transient / placed resource**, created via `CreatePlacedResource`
from UE5's TransientResourceAllocator backing heap. Two consequences:

1. It is **aliasable** — the same heap memory is recycled for other transient
   resources within a frame, so its contents are only valid inside its own
   live range. A readback must be taken inside that window, not arbitrarily.
2. It explains why creation-time hooks were useless: transient resources are
   *placed* into a pre-allocated heap, so the interesting call is the
   placement, not an allocation.

**UNORM changes the decode math.** Values are in `[0,1]` and require decoding
(roughly `v * 2 - 1`, then scaling) — they are *not* directly signed pixel
offsets the way a FLOAT buffer would be. Assuming FLOAT would have silently
produced a plausible-looking but wrong warp-validation result. This is exactly
the "units and precision" concern the brief calls out.

**Resolution caveat.** The capture above reports 2212x1348 and contains
`SetName("SceneVelocity")` — UE5 compiles resource naming out of Shipping — so
it is near-certainly an Editor/PIE capture. The live Shipping build runs at
1212x760. Format, mip count, array size, sample count, flags and layout are
all resolution-independent and match exactly, so the identification holds, but
**the resolution constant must come from the Shipping build**. An earlier
round of this project failed outright for exactly this reason: an editor-
captured 2212x1348 was used against a Shipping build rendering at 1212x760,
and matched nothing.

---

## Extraction and validation

### The readback

Velocity is copied **inside the `ResourceBarrier` hook**, at the
`RENDER_TARGET -> ALL_SHADER_RESOURCE` edge. That is the only correct moment:
the buffer is a *transient placed* resource, so its backing heap memory is
aliased by other transient resources later in the frame — reading it at
Present would return whatever overwrote it.

The back buffer is copied in the Present hook before chaining through, using
our own command list submitted to the game's own DIRECT queue (captured
opportunistically from `ExecuteCommandLists`), with one fence covering both
copies. Four slots in flight so the CPU never blocks on the GPU, and a
background writer thread does all disk I/O — never the render thread, applying
the lesson from crash (3) above directly.

Subresources are derived, not assumed. The copy loops over
`mips * arraySlices * planeCount` footprints from `GetCopyableFootprints`, with
the plane count queried via `CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO)`,
and transitions them with `D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES`. For the
velocity buffer this degenerates to a single copy (mips=1, arraySize=1,
non-planar), but hardcoding subresource 0 would silently truncate a mipped,
arrayed, or planar resource - a depth/stencil target keeps stencil in plane 1,
so a depth capture would look complete and be missing half its data.

Two details that bite:

- Our copy's barriers must be recorded through the **original**
  `ResourceBarrier` pointer, not the hooked one, or every barrier we add
  recurses back into identification and logging.
- `GetCopyableFootprints` reports total size as `rowPitch*(height-1) +
  width*bpp`, **not** `rowPitch*height` — the last row is not padded.

Velocity is 1212x760 (render resolution, DRS-scaled) while the back buffer is
1707x1067 `R10G10B10A2_UNORM`, so everything downstream has to resample and
bit-unpack rather than assume 8-bit RGBA at a matching size.

### What the buffer actually contains

Visualising the decoded field over the frame shows it landing **exactly on the
weapon viewmodel**, with all static geometry unwritten — about 7.5% of pixels.
That is UE5 working as designed: `SceneVelocity` stores per-object velocity for
*dynamic* geometry, and static-geometry motion is reconstructed from camera
matrices inside the consuming TSR/motion-blur shader.

Two consequences:

1. Unwritten pixels are **0**, and 0 is not "no motion" — zero motion encodes
   near 0.5. Decoding without a written-mask turns most of the screen into a
   large bogus displacement.
2. Warp validation can only legitimately score the written region. Scoring
   static geometry would measure something this buffer never claimed to
   contain; reconstructing that needs the engine-introspection path the brief
   lists as a separate exploration area.

This visual is also strong independent confirmation the *identification* is
right — the field lands on the one genuinely dynamic object and nothing else.

**Cross-checked against the engine's own view.** Running UE5's
`visualizetexture SceneVelocity` in-game shows the same structure from the
opposite direction: an approaching train and background characters lit up as
distinct coloured regions, the weapon viewmodel picked out separately, and all
static geometry black. The engine's own debug visualiser and a buffer pulled
off the GPU by an injected DLL agree on both *which* pixels carry velocity and
*that* static geometry carries none. That is about as good an independent check
on the identification as is available without engine source access, and it
settles the question of whether the sparse coverage was an extraction defect —
it is not, it is the design.

### The validation, and where it stops short

The first warp attempt made error dramatically **worse** under every sign and
scale. Rather than tweak conventions, I measured ground truth — and found the
capture had none: the game had been launched but never given input, so real
motion was 0.1–0.3px. **Before debugging a validation result, check that the
input data contains the phenomenon being validated.** A second capture was
taken during actual movement.

Optical flow was then tried as ground truth and gave weak, inconsistent
correlations. That reference was itself the problem: Farneback flow is
unreliable on a large, smooth, low-texture object (aperture problem). Replacing
it with **brute-force block matching** of the written region — which measures
rigid displacement directly — produced a clean result:

```
 offset    n  corr_dx_ch0  corr_dy_ch1   px/unit x   px/unit y
     -1   18      -0.6448       0.7200      -159.3       206.8
      0   18      -0.8090       0.8536      -193.0       207.4     <-- best
      1   18      -0.7873       0.4878      -190.1       138.7
```

This establishes three things: temporal pairing is correct (offset 0 wins, so
the render-thread-records-ahead concern was unfounded); X and Y live in
channels 0 and 1; and **X is inverted relative to Y**. Channel 2 turned out to
be bimodal — a flag, not a velocity component — and its apparent correlation
with dy was spurious.

It also exposed why the earlier sweep failed. UE5's documented encode
(`Common.ush`) implies ~2.0 UV per unit; measurement says ~0.15. The true
multiplier was around 0.05, far below the 0.25 minimum the sweep had tested,
so the error curve looked monotonic and I read "no signal" where the real
answer was "your search range excludes the answer."

> **Superseded.** The numbers in this block are from the linear decode and the
> spatial-means fit, both of which are now known to be wrong — see
> [the decode section](#the-decode-three-symptoms-one-cause-and-a-wrong-diagnosis-in-between).
> The `-193 / +207 px/unit` figures are a chord across a square-root curve, not
> a scale, and the "~0.15 vs ~2.0" gap they appear to establish is an artefact
> of the encoding's shape. The temporal-offset result — that offset 0 beats ±1,
> so the frame pairing is correct — does survive, because it depends only on
> which frame correlates best and not on the magnitude of the decode. The
> conclusion that channel 2 is "a flag, not a velocity component" is wrong:
> it is the top half of the float32 `V.z`.

**The result depended on the footage, not the code.** With measured constants,
the first capture gave a weak, high-variance answer:

```
54 pairs (corridor):  improved 44%   median -15.8%   best +50.6%   worst -215%
```

That capture contained exactly one moving thing — a rigid weapon viewmodel
drifting about 1px per frame, largely sub-pixel. There was almost nothing for a
warp to reconstruct, and bilinear resampling error was comparable to the signal.

Recapturing in a scene with real motion (a train crossing at ~11px/frame,
background characters, plus the viewmodel) changed the answer completely, with
**no code or constant changed**:

```
59 pairs (station):   improved 59/59 (100%)   mean error reduction 33.3%
                      MAE 0.04796 -> 0.03198
```

Sweeping a multiplier over the decoded field puts the optimum at 1.0 with clean
falloff either side. That is a convergence check and nothing more — the
constants *are* that sweep's optimum, so it cannot corroborate them. An earlier
draft cited it as evidence; it isn't.

`results/warp_validation.png` shows the pair with the **most motion** in the
capture — the most legible one, not a typical one, since the figure code
selects `max(baseline error)`: MAE 0.0536 -> 0.0272.

Neither number is reproducible from the repo, and both are now also from the
superseded decode. The station capture is gone (dumps are gitignored and
`%TEMP%\mv_dump` was overwritten by a corridor capture) — a mistake worth
naming, since it is the reason a headline result had to be withdrawn rather than
recomputed. The capture directory is now overridable via `MV_DUMP_DIR`, and
`tools/run_validation.py` writes every tool's output verbatim to
`results/validation.txt` so a reader can check numbers without a capture of
their own.

On the capture that *is* on disk, `warp_validate.py` correctly reports 0/118 pairs
improved and prints its own "no multiplier beats the un-warped baseline"
warning — which is the right behaviour on near-static footage, but means the
documented commands do not reproduce the documented numbers.

So the lesson from the idle capture recurred in subtler form. The first time,
the footage had *no* motion and the failure was obvious once measured. The
second time the footage had motion, just not motion this buffer meaningfully
describes — one rigid object moving sub-pixel. **A validation set has to
contain the phenomenon at a magnitude the method can actually resolve**, and
"the test failed" was again a statement about the data rather than the code.

Two bugs surfaced while producing the figures, both found by *looking* rather
than by any test failing:

- The whole image was being warped by the decoded flow, including unwritten
  pixels, where raw 0 decodes to a large bogus displacement and smeared static
  geometry across the frame. Scores were masked so the numbers were unaffected,
  but the visual was wrong until the flow was zeroed outside the written region.

  **This fix was then only half-applied, and the half that was missed is the
  one that draws the picture.** The zeroing went into the two scoring paths in
  `warp_validate.py` and not into step 3, the figure generator — so the
  committed script kept producing a `|warped - N|` panel that was a near-white
  smear across the whole frame, dramatically *worse* than the un-warped
  difference, while every printed number stayed correct. The repo carried that
  broken output as a committed PNG next to the good one from an earlier script
  revision, with no way to tell which was which except by reading the labels.

  The reason it survived is the same shape as the format-enum bug: masking the
  *scores* meant nothing measurable ever regressed. A bug that only shows up in
  an artefact nobody re-generates is invisible to every check that runs. The
  general lesson — if a figure and a metric are produced by different code
  paths, the figure is unvalidated no matter how good the metric looks.
- The captures were being gamma-corrected with `**(1/2.2)` on the assumption
  the back buffer was linear. It is not — the swapchain is what goes to the
  monitor, so it is already display-encoded, and the correction was a *second*
  gamma that visibly washed out an otherwise dark scene. The give-away was
  simply that the captures looked brighter than the game does. Fixing it also
  moved the validation result, since MAE is measured on those images: mean
  error reduction went from 17.1% to **33.3%**, and the best multiplier shifted
  enough to warrant re-deriving the constants (they are now ~20% larger).
  A display-encoding mistake is not cosmetic when your metric is image
  difference.

Evidence for correct identification and extraction, in order of strength:

1. a unique descriptor match against RenderDoc's `SceneVelocity` creation call,
   plus the strict per-frame barrier invariant above;
2. **zero D3D12 debug-layer errors over 90 frames** with GPU-based validation
   enabled, running the real capture path against a placed look-alike resource,
   plus a byte-exact readback check;
3. agreement with UE5's own `visualizetexture SceneVelocity` in-game view;
4. the decode now agreeing with independently block-matched image motion to
   0.25px where the previous decode was off by 1.66px.

Warp-and-difference is deliberately **not** on that list any more. Its passing
numbers came from a capture that no longer exists, under a decode now known to
be wrong, with no controls. It has controls now and it needs a recapture.

Two things previously listed here have been removed as evidence, because they
are not:

- **The scale sweep peaking at the derived constants.** Those constants *are*
  that sweep's optimum. The peak shows the fit converged; it cannot corroborate
  itself. This one was load-bearing in an earlier draft and should not have been.
- **The -0.81 / +0.85 correlation.** It is between ~18 pairs of per-frame
  *spatial means*, not per-pixel or per-object displacement, and it was written
  up in a way that reads as the latter.

### The decode: three symptoms, one cause, and a wrong diagnosis in between

This is the most instructive sequence in the project, so it is worth tracing in
the order it actually happened rather than presenting the answer.

**Symptom 1 (early).** Constants recalled as being "from `Common.ush`" produced
displacements ~13x too large. I abandoned them and fitted my own against
measured motion. The fitted values worked on the station capture and the warp
test passed, so the disagreement was filed as an unexplained curiosity.

**Symptom 2.** On the corridor capture, the fitted constants emitted a steady
~1.7px of confident, consistently-signed motion on a viewmodel that block
matching put at 0.00px. The raw codes showed `ch0` sitting on the documented
zero point (offset −57.7) while `ch1` sat +456.5 codes off it and drifted by
1010 codes across the burst.

**The wrong diagnosis.** I read that as a **zero-point problem, not a scale
problem** — the asymmetry between the two channels looked decisive, and a
constant additive offset is exactly what a wrong bias produces. I wrote it up
that way, listed TAA/TSR jitter and the uninvestigated channels 2/3 as the
leading hypotheses, and called it the project's largest open problem.

It was a scale problem. Specifically it was a *shape* problem.

**The cause.** `EncodeVelocityToTexture` (`Common.ush:2060`) applies a square
root before quantising:

```hlsl
#if VELOCITY_ENCODE_GAMMA                        // 1 for every SM5+ platform
    V.xy = sign(V.xy) * sqrt(abs(V.xy)) * (2.0 / sqrt(2.0));
#endif
EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f;
```

`VELOCITY_ENCODE_GAMMA` is `#define`d to 1 for every feature level at or above
SM5 (`Common.ush:238`). There is no cvar and no project setting — for a D3D12 PC
title it is unconditionally on. Both symptoms fall out of it immediately:

- A square root has no single slope, so "the scale" was never a constant.
  Linearising it around the station capture's dominant motion (a train at
  ~11px/frame across a 1212-wide buffer) predicts an apparent slope of
  `(11/1212) / (sqrt(2*11/1212)*sqrt(2)*0.2495) = 0.191` UV/unit. I had fitted
  −0.1758 and +0.1991. **The fit was not wrong about its own data — it measured
  the tangent of a curve and reported it as the curve.** The "~10x" was the ratio
  between a coefficient and a chord, and it varied with how fast the scene moved.
- A square root is steepest at zero, so decoding it linearly overstates small
  motions worst. The 456-code offset in `ch1` is not a broken bias; it is
  **0.148px of real viewmodel bob**, inflated to 1.05px by a linear decode. `ch0`
  looked "correct" only because the viewmodel barely moves horizontally, so its
  offset was small enough that mis-decoding it didn't show. The asymmetry
  between the channels — the thing that made me confident it was a zero-point
  error — was just the asymmetry of the motion.

Undoing the sqrt drops decoded displacement on that capture from 1.723px to
0.293px against a block-matched 0.063px; mean error against measured motion
falls from 1.660px to 0.251px.

**Channels 2 and 3, which "had never been investigated".** They are not TSR
data and not a flag. Under `VELOCITY_ENCODE_DEPTH` (`Common.ush:2071`) they are
the high and low halves of the float32 bit pattern of `V.z`, the DeviceZ delta,
with the bottom bit of channel 3 carrying `bHasPixelAnimation`. Reassembled they
give a clean float — 100% finite, ~1e-6, sign flipping between frames as the
surface approaches or recedes. Channel 2's "bimodality at ~0.214 / ~0.713",
recorded here for months as evidence of a flag, is the **sign bit** of that
float. Plotting the two halves of a number as if they were two independent
channels is what made one look bimodal and the other look like noise.

**TAA/TSR jitter — the hypothesis I never needed.** `Calculate3DVelocityBase`
(`VelocityCommon.ush:9`) subtracts `View.TemporalAAJitter.xy` from the current
screen position and `.zw` from the previous one *before* differencing. The
stored vector is already de-jittered, so removing it again would have introduced
an error. This was my leading hypothesis and it is answered by four lines.

#### What actually went wrong here

The engine source for the exact version this title is built on — UE 5.7.1 — was
on the same machine the entire time, one directory away from the repo. Every one
of these answers is a `grep` in `Engine/Shaders/Private/`. Instead I spent the
project fitting constants against the buffer's own output.

**A fit against the data can only ever demonstrate internal consistency.** It
cannot tell you the encoding is a square root, because a smooth curve is locally
linear and a linear fit over a narrow range of magnitudes will always look
excellent. The warp test agreed, the correlations were high, and every check I
had was a check against myself. The failure was not "I fitted badly" — the
per-frame-spatial-means procedure *was* also bad, and replacing it was correct —
it was preferring a fit over a primary source at all.

The recalled constants were wrong in a specific, seductive way: they were an
*incomplete quotation*. Bias right, scale right, one `#if` block silently
dropped. That is worse than a wrong number, because the parts that are right
make the whole thing look verified. When "the documented value is 13x off",
the reasonable inference is that the documentation was misquoted, not that the
engine is wrong — and the fix is to open the file, not to fit around it.

This is the same failure as the `_FLOAT`/`_UNORM` enum error earlier in this
document, one level up: both replaced "what does the engine actually do" with
"what is consistent with what I have seen so far", and both failed silently and
plausibly for a long time.

## Coverage is not magnitude: the third capture that still could not validate

Finding #21 was "the capture had no motion". Finding #24 was "the capture had
motion, but not at a magnitude the method could resolve". A third capture,
taken specifically to fix that, failed a third time — and the reason is worth
recording because it is a genuinely different mistake from the first two.

The new capture looks, by every summary statistic I had been using, much better
than the corridor one: **29% of pixels written, up from 8%**. Far more dynamic
objects on screen. It is still unusable for warp validation, because p99 real
motion is **1.68px**. The objects were there; they were not moving.

The mechanism is in this document already, one section down: **UE5 writes
velocity for every skeletal mesh each frame whether or not it is animating.** So
a room full of idle characters produces high coverage and near-zero
displacement. Coverage measures how much of the buffer is populated; it says
nothing about magnitude, and I had been treating a rise in the first as
progress toward the second.

Stated as a rule: **the property a validation set needs is the one the method is
sensitive to, and that is rarely the property that is easiest to measure.**
Coverage is easy to compute and reassuring; magnitude is what warp-and-difference
actually consumes. Three captures in, the check that would have saved all three
is one line of `inspect_velocity.py` output — p95/p99 displacement — read
*before* running the validation rather than after it fails.

The tooling now enforces this rather than relying on me to remember.
`derive_scale.py` refuses to report a slope or an exponent when p99 motion is
below 2px, and `warp_validate.py` explains that a sweep whose optimum sits at
the smallest value tried is reporting "there is nothing to reconstruct" rather
than measuring a scale.

What the capture *did* settle, because these do not need large motion:

- **fence coverage: 120/120 frames verified by command-list identity** (see
  below) — the readback is provably not stale;
- **the barrier survey**: 898 profiled resources narrow to 25 on behaviour
  alone;
- **the decode, on X**: per-pixel regression against block matching gives slope
  0.900, R² 0.834 over 137k pixels, against 0.255 for the old linear decode;
- **frame-adjacency checking works**: `frames.csv` recorded game frames 2671
  onward, and the tools rejected exactly 1 pair of 119 — the boundary between
  the two F8 bursts, which the old burst-modulo heuristic would have missed
  because both bursts landed in one contiguous run of capture indices.

## Turning "it didn't crash" into evidence: the debug-layer harness

Every barrier-correctness claim in this project rested on the game not
crashing. That is close to no evidence at all — a wrong `StateBefore` in a
transition barrier, a copy sized from stale footprints, or a descriptor
overwritten while a command list still references it are all things a driver
will happily execute. The D3D12 debug layer is precisely the tool that catches
them, and it cannot be enabled inside a Shipping build.

The old `testhost` did nothing D3D12-related. It confirmed six vtable patches
landed without crashing a process that had no particular reason to crash, and
that was the whole of it.

It is now a harness that builds a target the debug layer *can* watch: debug
layer plus GPU-based validation, a **placed** resource (not committed — the real
one comes from UE5's `TransientResourceAllocator`, and placed resources have
aliasing rules the layer checks separately) carrying SceneVelocity's exact
descriptor, driven through the same `RENDER_TARGET ↔ ALL_SHADER_RESOURCE` cycle,
with `mv_hook.dll` loaded into the process so the real capture path runs against
it. Every info-queue message is pulled and printed, so the result is an artefact
rather than something you needed a debugger attached to observe.

Result: **0 errors across 90 hooked frames**, with 0 during setup as a control.
The captured bytes were then checked rather than just the absence of complaints
— a buffer cleared to `0.5` reads back as exactly `[0.5 0.5 0.5 0.0]` and
decodes to 0.00000px, which validates the row-pitch unpadding and the decode
end to end.

**It falsified one of my own additions on its first run**, which is the real
argument for building it. Alongside the harness I had added a runtime check for
the fence-coverage assumption: remember which command list the velocity copy was
recorded onto, and confirm the game submits *that* list to the queue we fence on
before we signal. It reported "not submitted" on all 60 frames.

The check was wrong, not the code under test. Under the debug layer the
validation wrapper means the command-list pointer seen by the `ResourceBarrier`
hook and the one passed to `ExecuteCommandLists` are **two different COM
objects**. Switching to a proper COM identity comparison —
`QueryInterface(IID_IUnknown)`, which is the only pointer COM guarantees is
stable per object — did not fix it either: the wrapper is a genuinely separate
object, not an aggregation, so it has its own identity. Interface identity is
simply not observable across layers from inside a hook.

Two things came out of that. The check now reports three grades (verified by
list identity / consistent-but-unproven / unsafe) with an ordering-based
fallback for the middle case, rather than a boolean that is wrong under
instrumentation. And it is a reminder that a tool written to validate something
else will validate your validator first, if you let it.

## A correction: "motion field" is not Unreal's term, and the colour wheel is not Unreal's view

Worth stating plainly because it briefly muddled how this work was being judged.

Unreal has no concept called a "motion field". It has the **velocity buffer** -
`SceneVelocity` - and its only built-in debug view is
`visualizetexture SceneVelocity`, which dumps the stored channels straight to
RGB. That is what produces the engine's characteristic picture: olive for most
geometry, lavender for the viewmodel and HUD, black elsewhere.

"Motion field" is the assignment brief's language. The HSV colour-wheel
rendering - hue for direction, brightness for magnitude - comes from the
**optical-flow literature** (the Middlebury flow colour coding), and was
imported here by me. Unreal never draws anything like it.

That distinction matters for what counts as evidence:

- **Extraction correctness is judged against Unreal.** The raw-channel view
  reproducing the engine's own `visualizetexture` output - including the
  bimodal channel 2 that splits the image into olive and lavender families -
  is a real check that the right buffer is being read the right way, from
  outside the process.
- **The flow-coloured view is a presentation layer we chose.** Its appearance
  has no Unreal ground truth to match. When it looked wrong, that was a tuning
  problem in a convention of our own, not evidence the extracted data was bad.

The practical failure this caused: near-zero vectors have no meaningful
direction, so `atan2(dy, dx)` on them returns noise and the flow view fills
with rainbow speckle. UE5 writes velocity for every skeletal mesh each frame
whether or not it is animating, so idle meshes are exactly this case - which is
why the speckle appeared on characters and not on static world geometry, which
is never written at all. A brightness floor intended to keep slow motion
visible made it worse by guaranteeing the least meaningful pixels were lit.
Measured offsets from the encoded zero are typically 0.001-0.013, decoding to
roughly 0.2-3px, so a large share of written pixels sit near the threshold
where direction stops being reliable.

Terminology in this repo now follows Unreal: the thing being extracted is
**`SceneVelocity`**, the velocity buffer.

**The flow-coloured view was subsequently removed entirely.** It had no ground
truth to be validated against, and it had a failure mode the engine's own
mapping does not: encoding direction as hue means the display is dominated by
`atan2` of two near-zero numbers across most of the frame. Keeping a view whose
worst-case output is indistinguishable from corrupt data, purely because it is
the convention used elsewhere in computer vision, was not worth it. The live
overlay now shows what the engine shows.

## Retargeting at a title I do not own

Everything above was developed against "Oxi", my own UE 5.7.1 project. That
demonstrates the pipeline but dodges the actual problem, because I could always
fall back on knowing how the project was configured. This section is about
making it work on a **third-party UE 5.2 title** — and, more usefully, about
which of the previous conclusions turned out to be facts about *velocity* and
which were facts about *Oxi*.

### The filter was matching a constant against itself

The identification filter was `desc.Width == 1212 && desc.Height == 760 &&
desc.Format == R16G16B16A16_UNORM` — a tuple copied out of a RenderDoc capture.
On any other title that matches nothing, silently, for the whole session.

It has been replaced by two filters that the barrier survey already showed are
individually insufficient and jointly unique, applied in that order:

1. **Structure**, derived from the engine's own rules rather than from one
   observed instance. `FVelocityRendering::GetFormat` (`VelocityRendering.cpp:758`)
   is a closed set of four formats, `GetCreateFlags` (`:774`) is
   `RenderTargetable|UAV|ShaderResource` → D3D12 `ALLOW_RENDER_TARGET|ALLOW_UNORDERED_ACCESS`,
   and `mips=1, arraySize=1, samples=1, layout=UNKNOWN` come with it.
2. **Behaviour**, over a settling window: exactly two barrier events per frame,
   only `RENDER_TARGET ↔ shader-resource`, no UAV barriers, no state-continuity
   violations.

Resolution is no longer matched at all. It is *derived* — the hook logs a
histogram of render-target resolutions and uses the mode only to rank survivors,
never to exclude — and constrained by the back-buffer size, which is the one
resolution fact available without knowing the title.

If more than one resource survives both filters, all survivors are dumped and,
unless the ranking separates them by a clear margin, **the hook refuses to
capture** and tells you which environment overrides would resolve it. A wrong
pick produces a dump that decodes to plausible garbage, which is the most
expensive failure mode this project has had.

### Three things that were Oxi facts wearing engine clothes

Found by re-deriving each load-bearing constant instead of porting it.

**`ALL_SHADER_RESOURCE` is not the state to match.** The capture triggered on
`RENDER_TARGET → ALL_SHADER_RESOURCE` (0xC0) exactly. But UE5's RDG asks for
`SRVMask` on most textures and `SRVGraphics` on a texture only read by a pixel
shader, and the D3D12 RHI maps the latter to `PIXEL_SHADER_RESOURCE` (0x80)
alone. A title whose velocity buffer is consumed only by a pixel shader would
have been rejected — by finding nothing, which is the failure mode that looks
like "this title is different" rather than "my filter is wrong". The match is
now any non-empty subset of the shader-resource bits, and the state the barrier
actually carries is passed through to the copy so it gets *restored* to what the
game believes it is in, rather than to what Oxi happened to use.

**The strict 2-cycle encodes an assumption about the render pipeline, not about
velocity.** A title with `r.BasePassOutputsVelocity=1`, or one that reads
velocity in two passes, transitions the buffer 4 or 6 times a frame while still
being unambiguously the velocity buffer. Identification tries the strict form
first and falls back to a relaxed one (even event count, same purity
requirements), reporting loudly that it did — because the relaxed form is a
weaker claim and should not be quietly substituted for the README's headline.

**A resolution change was a permanent, silent capture failure.** `ValidateCandidate`
correctly dropped the candidate when its descriptor changed, but nothing
re-ran the search, and the "seen" set guarantees a resource is never examined
twice — so after one DRS step the session captured nothing more, with one line
in the log to say so. On a title you do not control, DRS is likely on by
default. Identification now reopens on that event, which also required clearing
the "seen" set: without that the second search walks past every resource in the
process in silence.

### The harness could only ever confirm a constant matched itself

`testhost` placed one decoy with SceneVelocity's exact descriptor, hardcoded to
the same `1212x760` the filter was hardcoded to, and asserted that no
debug-layer errors occurred. That assertion is worth having, but note what it
could not detect: it stayed true throughout the multi-day episode in which the
filter was selecting four unrelated `_FLOAT` render targets. "A capture
happened" is not "the right thing was captured".

It now places a **line-up** at a resolution the hook has never been told about
(1120x630), and the DLL exports the pointer it selected so the harness can check
*which* resource won:

| look-alike | rejected by |
|---|---|
| SceneVelocity (fmt 11, RT\|UAV, strict 2-cycle) | — selected |
| HDR pool target (`R16G16B16A16_FLOAT`, otherwise identical) | structure — a format `GetFormat` cannot return |
| compute-written RG16 (fmt 35, right flags, right size) | behaviour — goes through `UNORDERED_ACCESS`, takes a UAV barrier |
| twin read twice (byte-identical descriptor) | behaviour — 4 RT↔SRV events per frame, not 2 |

Each decoy is rejected by exactly one filter, so neither filter can be quietly
load-bearing on its own. The first entry is the exact shape of this project's
most expensive bug, now a regression test. `--ambiguous` adds a perfect twin of
the real buffer, where the only correct answer is to refuse; that path is
asserted too.

```
[testhost] PASS - selected the velocity buffer out of 4 look-alikes, with no
                  hardcoded resolution or descriptor
[testhost] PASS - identification refused to pick between two indistinguishable
                  candidates                                    (--ambiguous)
[testhost] debug-layer errors during 240 hooked frames: 0       (with GPU-based validation)
```

### What the harness caught: the debug layer doubles every barrier

It failed on its first run, and the reason is worth the space.

Every `ResourceBarrier` call reached the hook **twice**. The validation layer
forwards to the core layer through the same patched code, so one call from the
application arrives as two nested calls. The arithmetic was unambiguous before
any theory was applied: three decoys with three different barrier patterns each
reported exactly double, UAV barriers included.

That is not a cosmetic miscount. The identification signature is literally
"exactly two barrier events per frame", so under instrumentation the real
velocity buffer scores four and is rejected; and the continuity check sees
`RT→SRV` followed by `RT→SRV` and counts a violation that never happened. **Both
behavioural filters are inverted by the presence of a debugging tool.** A
thread-local depth guard, counting only the outermost invocation, fixed it, and
the numbers moved exactly as predicted — 162 events → 81, 162 violations → 0,
`exactly2` 0/80 → 80/80.

This is the same lesson as the fence-coverage check that compared raw COM
pointers, one level down: **observations made from inside a hook are
observations of the instrumented runtime, and the instrumentation is part of
what you are observing.** Twice now, the tool built to validate the capture path
has instead falsified the measurement I was about to trust. Both times the code
under test was fine and the check was wrong.

### Reading semantics into bits of a value whose type you have not established

UE 5.2 and 5.7.1 encode `V.z` differently:

```hlsl
// 5.2   - no mask; all 16 low bits are float32 mantissa
V.z = asfloat((uint(round(EncodedV.z * 65535.0f)) << 16) |  uint(round(EncodedV.w * 65535.0f)));
// 5.7.1 - bottom 1-2 bits stolen for bHasPixelAnimation / temporal responsiveness
V.z = asfloat((uint(round(EncodedV.z * 65535.0f)) << 16) | (uint(round(EncodedV.w * 65535.0f)) & VELOCITY_Z_LOW_MASK));
```

So on a 5.2 title, reporting `channel3 & 1` as `bHasPixelAnimation` would be
presenting the low mantissa bit of a float as a semantic boolean. That is the
*same class of error* this project already made once, when channel 2's
bimodality was written up as "a flag, not a velocity component" — it is the sign
bit of that same float. Same mistake, one bit lower down. The failure mode is
not "I got a bit wrong", it is **reading semantics into bits of a value whose
type you have not established.**

The version is now recorded per capture in `meta.txt` (from `MV_ENGINE_VERSION`)
rather than living in a module-level global in `mvtools`, which was fine for one
title at a time and silently wrong the moment two dumps are analysed in one
session. Only 5.2 and 5.7.1 are verified; anything else warns and falls back
rather than guessing. When no version was recorded, the tools say so instead of
printing a number with nothing behind it.

**A caveat found while writing that table down.** `VELOCITY_Z_LOW_MASK` is not a
constant *within* 5.7.1 either (`Common.ush:2049-2055`): it is `0xFFFC` when
`VELOCITY_ENCODE_TEMPORAL_RESPONSIVENESS` is on and `0xFFFE` otherwise, and that
depends on a per-shader-platform property no dump can reveal. The table uses
`0xFFFE`, deliberately the narrower of the two: if the title actually uses
`0xFFFC` this leaves one extra low mantissa bit in `V.z`, which perturbs it by
~1e-7 relative. Guessing the other way would mask off a real mantissa bit *and*
attribute meaning to a bit that has none. Bit 0 is stolen under both branches,
so `bHasPixelAnimation` is safe either way.

### What is verified, and what is not

Verified against primary sources on this machine, not recalled:

- every DXGI format value, against `dxgiformat.h` (10.0.26100.0) — including
  that `R16G16_UNORM` really is **35**, sitting between `R16G16_FLOAT` (34) and
  `R16G16_UINT` (36);
- `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|ALLOW_UNORDERED_ACCESS == 5` and
  `ALL_SHADER_RESOURCE == 0xC0`, against `d3d12.h`;
- `FVelocityRendering::GetFormat`/`GetCreateFlags`/`NeedVelocityDepth` and the
  `PF_* → DXGI` table, against the UE 5.7.1 tree at `D:\Unreal-Oxi`;
- that `Atomic64Compatible` does **not** add a D3D12 resource flag — it sets
  `FD3D12ResourceDesc::bRequires64BitAtomicSupport` (`D3D12Texture.cpp:352`),
  which is why "flags == 5 exactly" survives as a ranking signal.

**Not verified when this section was first written**, and since answered by
running against the target. The order matters, so both states are kept:

- **The UE 5.2 shader source.** There is no 5.2 tree on this machine and the
  engine repository is not publicly fetchable, so the 5.2 encode/decode came to
  me as pasted text I could not re-derive from a primary source. That is exactly
  the failure mode that cost this project the most (see "one wrong enum digit"),
  so it was flagged rather than assumed. **It is now closed outright**: the encode was read
  out of the shipping game's own compiled shader, which is better evidence than
  the source tree would have been. See "The encode, read out of the shipping
  game's own compiled shader" below.
- **Anything measured on the target title.** Now measured, below.

## Retargeting, part two: what the third-party title actually did

Everything above was written before the hook had run against a game I do not
own. This section is what changed when it did. The target is Skyrunner, a
UE 5.2 title, and nearly every behavioural claim this project had accumulated
turned out to be a claim about Oxi.

### Establishing the engine version without a version string

The executable carries no version resource, and the PDB shipped beside it shows
a **custom engine build from a Perforce workspace**
(`D:\Travail\PerforceWorkspace\PFE2024_Engine_Skyrunner\...`), so even a version
number would not have settled how it encodes velocity. Two checks against that
PDB did:

- `Strata` appears 1575 times, `Substrate` 7. In the UE 5.7.1 tree that ratio is
  inverted — `Substrate` in 94 renderer files, `Strata` in 2 residual ones —
  because Substrate was the 5.4 rename. So this is a **pre-5.4 engine**,
  consistent with 5.2.
- `HasPixelAnimation` appears **zero** times. It exists in 5.7.1's C++, so its
  total absence from a PDB full of renderer symbols is positive evidence that
  this build has **no pixel-animation flag** — which is exactly what the 5.2
  depth-channel rules assume (no mask; all 16 low bits are mantissa).

That is not proof of the label "5.2". It is something more useful: direct
evidence about the two things the decode depends on.

### RenderDoc's global hook was contaminating every measurement

The first run against the target produced a clean-looking barrier profile — 4
events per frame, pure `RENDER_TARGET ↔ shader-resource`, exactly 2 continuity
violations per frame. I was one edit from relaxing the identification filter to
accommodate it when the operator mentioned RenderDoc's global hook was enabled
on that executable.

It was the same signature as the debug-layer double-count above — exactly 2× the
events, exactly 2 violations per frame — because RenderDoc wraps
`ID3D12GraphicsCommandList` the way the validation layer does. With it disabled
the real pattern is **5 events per frame**, nothing like the clean number I had
been about to design around.

That is the same lesson in a third form: an observation made from inside a hook
is an observation of the *instrumented* runtime. What made it dangerous here is
that the contaminated reading was **tidier** than the truth, so it read as
signal rather than noise.

### The barrier signature is not a property of velocity

With RenderDoc gone, the target's SceneVelocity shows:

```
RT->UAV=91   RT->NON_PS_SRV=91   UAV->NON_PS_SRV=91   NON_PS_SRV->RT=182
```

Five events per frame, and it enters `UNORDERED_ACCESS` once per frame. Against
a survey of every resource in each title:

|                                       | Oxi | Skyrunner |
|---------------------------------------|-----|-----------|
| resources profiled (seen ≥10 frames)  | 551 | 729 |
| exactly 2 barrier events every frame  | 221 | **357** — half the process |
| + pure RT↔SRV, no UAV, no violations  | 25  | 54 |
| does SceneVelocity itself pass?       | yes | **no** |

Three properties this repo presented as facts about velocity are facts about
Oxi:

1. **the strict two-events-per-frame cycle** — half of Skyrunner's resources do
   it, and its velocity buffer does not;
2. **`ALL_SHADER_RESOURCE` as the destination state** — Skyrunner uses
   `NON_PIXEL_SHADER_RESOURCE`, because RDG asks for `SRVGraphics` rather than
   `SRVMask` when only one shader stage reads it;
3. **"never enters `UNORDERED_ACCESS` despite declaring it"** — which the README
   called *the discriminator that separates it from the compute-written
   targets*. Skyrunner's does, once per frame.

Any one of them, kept as a rule, rejects the correct buffer on the other title.
They are now scored signals, and the only behavioural **gate** left is the one
that is structural rather than incidental: produced as a render target, read as
a shader resource, each frame. That is what the RDG pass does anywhere.

Meanwhile the structural filter was *unique on its own* here: of 755 profiled
resources, exactly one had a velocity format with `RT|UAV` at render resolution.
The two filters swapped roles between the titles. So the honest claim is not
"individually insufficient, jointly unique" — itself an Oxi-specific observation
— but that **you cannot know in advance which filter will do the work**, which
is why neither may be allowed to be decisive alone.

### The ground truth was out of range, twice, silently

`check_decode.py` reported `mean block-matched 1.68px` against a decoded
`27.05px`, and I nearly wrote it up as a 16× decode error. Block matching
searches ±14px; above that the minimum sits on the edge of the search box at
every pixel and what comes back is a plausible small number rather than a
failure. Phase correlation agreed at ~0.2px, which felt like corroboration and
was two references failing in two different ways — it measures translation only,
so a camera flying forward (radial expansion, no net translation) reads as zero.

The fix was coarse-to-fine, and then the fix needed a second fix. `dense()` is
now pyramidal — warp by the upsampled flow at each level, search only the
residual; 3, 12, 40 and 75px shifts all recovered to within 0.04px against
synthetic ground truth. And `derive_scale.py` was silently undoing it, because
its clipping filter discarded any match larger than the search window. That test
was correct while the window *was* the whole search, and became wrong the moment
a coarse stage existed.

Even fixed, per-pixel matching does not work on this footage. The written region
is thin wind-animated foliage with background visible through it, and the game
applies heavy motion blur, so a 17px correlation window routinely spans two
surfaces moving differently. It reports slope ~0.7 and **gets worse the harder
the matches are filtered** — 0.73 keeping the top 50% by confidence, 0.39
keeping the top 3%. A reference that degrades as you demand more of it is not
measuring what you think. `region_match.py` exists to ask the weaker question
this footage can answer, and says so in its own docstring.

This is the third time a *reference* rather than the thing under test has been
wrong here, after optical flow on smooth geometry and the multiplier sweep whose
range excluded the answer.

### The encoding question, answered by the game

`region_match.py`, 118 pairs, decoded displacement against cross-correlation of
the game's own presented frames, over the same pixels:

```
                              X: slope   r        Y: slope   r
engine (sqrt, Common.ush)        0.921  +0.995       0.751  +0.935
legacy (linear, fitted)          1.604  +0.937       0.697  +0.881

ratio measured/decoded by displacement band (flat = correct encoding):
   0.0-5.6 px   n=39   engine 0.402   legacy 0.303
   5.6-20  px   n=40   engine 0.953   legacy 0.951
  20-94    px   n=45   engine 0.944   legacy 1.604
```

The band table is the answer. Above 5.6px the engine decode is **flat at ~0.95
from 5.6px to 94px**, while the linear decode diverges 0.95 → 1.60 — which is
how a linear decode of square-root-encoded data must fail: progressively, at the
top end, where the two curves separate. So `VELOCITY_ENCODE_GAMMA` applies on
this UE 5.2 build, established from the shipping binary rather than a source
tree. The lowest band reads ~0.4 for *both* decodes, which identifies it as the
reference running out of precision — template matching resolves whole pixels —
rather than a property of either decode.

The remaining ~5% shortfall is unexplained. Motion blur biasing the match low is
the obvious candidate and I have not shown it.

### Identification confirmed from outside the process

RenderDoc's `CreatePlacedResource` call for the target's velocity texture,
against what the hook derived with no access to it:

| field | RenderDoc | what the hook derived |
|---|---|---|
| Dimension | `TEXTURE2D` | required by the structural gate |
| Width x Height | 1708 x 1068 | 1708 x 1068, from the barrier-time `GetDesc` |
| DepthOrArraySize | 1 | required |
| MipLevels | 1 | required |
| Format | `R16G16B16A16_UNORM` (11) | one of the four `GetFormat` can return |
| Layout | `UNKNOWN` | required |
| Flags | `ALLOW_RENDER_TARGET \| ALLOW_UNORDERED_ACCESS` (5) | required, and scored for being exactly 5 |
| InitialState | `RENDER_TARGET` | implied by the `RT -> shader-resource` edge |
| allocation | **placed**, Heap 2595, offset 0 | assumed transient/placed |

Every field agrees. This is the same kind of evidence the Oxi work rested on -
a RenderDoc creation call matching the descriptor - with one difference that is
the whole point of this phase: on Oxi the descriptor was **copied out of
RenderDoc into the filter**, and here the filter **derived** it and the
RenderDoc call was consulted afterwards to check. Same artefact, opposite
direction of information flow.

It also confirms `placed` rather than committed on this title, which is the
premise the entire capture design rests on: the copy is taken at the barrier
because a transient placed resource's backing memory is not ours to read later.

**A correction, immediately.** Shown a full-resolution `R11G11B10_FLOAT` UAV
texture created at `Heap 2473, offset 458752`, I suggested it might alias the
velocity buffer and that confirming the overlap would strengthen the aliasing
argument. It does not alias it: velocity is in **Heap 2595 at offset 0**, a
different heap entirely. The two cannot overlap.

So the aliasing premise remains an inference - though a better-supported one
since: see "The heap the velocity buffer lives in" below, where the heap turns
out to be a 128 MiB unrestricted pool created `CREATE_NOT_ZEROED` with our
texture at offset 0 and 114 MiB spare. Confirming it outright still needs
another `CreatePlacedResource` into **Heap 2595** at an offset below ~13.9 MiB
(1708 x 1068 x 8 = 14,593,152 bytes), which would provably share memory with the
velocity texture.


### The heap the velocity buffer lives in

```
CreateHeap: 0x08000000 (128 MiB), DEFAULT, CREATE_NOT_ZEROED, no restriction flags
  -> Heap 2595

CreatePlacedResource(Heap 2595, offset 0, 1708x1068 R16G16B16A16_UNORM)
  -> velocity, occupying 0x0 .. 0xDEAC80 (13.92 MiB), leaving 114 MiB spare
```

Two details in there are worth more than they look.

**`CREATE_NOT_ZEROED`.** Asking the driver *not* to zero a fresh allocation is
what you do when you expect the previous tenant's garbage to be there and intend
to overwrite it anyway. It is the allocation pattern of a recycling pool, not of
a dedicated buffer.

**A 128 MiB unrestricted heap holding one 13.92 MiB texture at offset 0.** No
`ALLOW_ONLY_RT_DS_TEXTURES`, so it is a resource-heap-tier-2 pool that can hold
buffers and textures alike, with 114 MiB left for whatever comes next.

Together: this is `TransientResourceAllocator` backing memory, which is the
premise the whole capture design rests on - the copy is taken at the barrier
because the buffer's storage is not ours to read later.

**It is still not proof.** Nothing observed so far actually *shares* that
memory. Closing it needs one more `CreatePlacedResource` into **Heap 2595** with
`HeapOffset < 0xDEAC80` (14,593,152; allow headroom to 16 MiB for tiling and
alignment). Until then the aliasing claim is a strong inference from the
allocation pattern, not an observation, and the write-up says so.

**It also corrected the test harness.** `testhost` was creating an
`ALLOW_ONLY_RT_DS_TEXTURES` heap sized to exactly one texture. That is a
different kind of object with different aliasing rules - and aliasing rules are
exactly what the debug layer checks about placed resources, so the harness was
modelling the wrong thing in the one dimension it exists to validate. It now
creates a tier-2 unrestricted heap with `CREATE_NOT_ZEROED`, matching the
capture, and still passes both pipelines with 0 debug-layer errors under
GPU-based validation.


### The encode, read out of the shipping game's own compiled shader

Everything above about the UE 5.2 encoding was inference: I could not obtain 5.2
source, so the square root was argued statistically (the decode's error is flat
across a 17x range of displacement where a linear decode diverges) and the
depth-channel rules rested on pasted text plus the absence of
`HasPixelAnimation` from the shipped PDB. Good arguments, but arguments.

They are no longer needed. RenderDoc's disassembly of the title's base-pass
pixel shader (SM6.6, DXIL) contains the encode in full:

```
_449 = _441 - _445;               // V.x = ScreenPos.x - PrevScreenPos.x
_450 = _442 - _446;               // V.y
_451 = _447 - _448;               // V.z = DeviceZ delta

_462 = (float)(_452 - _454);      // sign(V.x)  via (V>0) - (V<0)
_464 = abs(_449);
_466 = sqrt(_464);                // <-- VELOCITY_ENCODE_GAMMA
_468 = _466 * 0.352846;
_469 = _468 * _462;
_472 = _469 + 0.499992;           // EncodedV.x

_474 = asint(_451);
_475 = _474 >> 16;                // high half
_479 = sat(_475 * 1.52590e-05 + 1.52590e-06);
_480 = _474 & 65535;              // low half  -- 0xFFFF, NO MASK
_484 = sat(_480 * 1.52590e-05 + 1.52590e-06);
```

Every constant checks out against the engine formula:

| in the binary | derivation | value |
|---|---|---|
| `0.352846` | `(2/sqrt(2)) * (0.499 * 0.5)` | 0.35284628 |
| `0.499992` | `32767 / 65535` | 0.49999237 |
| `1.52590e-05` | `1 / 65535` | 1.5259022e-05 |
| `1.52590e-06` | `0.1 / 65535` | 1.5259022e-06 |

Three things are settled by this, from the shipping binary rather than from any
source tree:

1. **The square root is applied.** `sqrt` on `abs(V.xy)`, multiplied by
   `0.352846` — which is the gamma coefficient `2/sqrt(2)` folded together with
   the `0.499 * 0.5` scale by the shader compiler. This is why the constant to
   search for is `0.352846` and not `1.41421`. The decode this project uses is
   correct, and the statistical argument above was right.
2. **The depth channels are unmasked.** `_474 & 65535` is `& 0xFFFF`. UE 5.7.1
   masks with `VELOCITY_Z_LOW_MASK` (`0xFFFE` or `0xFFFC`) to steal the low bits
   for `bHasPixelAnimation` and temporal responsiveness. This build steals
   nothing, so all 16 low bits are float32 mantissa — the UE 5.2 behaviour, and
   independent confirmation of what the PDB implied.
3. **The jitter is already removed.** `_441 = _439 - _178` and
   `_445 = _443 - _180` subtract `View_TemporalAAJitter.xy` from the current
   screen position and `.zw` from the previous one *before* differencing —
   `Calculate3DVelocityBase`, in the binary. So the stored vector is
   de-jittered and must not be de-jittered again.

**And a live error this caught.** The Skyrunner capture was taken without
`MV_ENGINE_VERSION` set, so `meta.txt` recorded no version and the tools fell
back to their `(5, 7)` default. Every run therefore reported
`bHasPixelAnimation set on ~43% of written pixels` — a flag that **does not
exist in this build**. Those 43% were the low mantissa bit of a float being
read as a boolean: precisely the failure this project already committed once,
when channel 2's bimodality was written up as "a flag, not a velocity
component". It got as far as a committed results file before the disassembly
contradicted it.

The mechanism intended to prevent it worked as designed and was not enough. The
tools printed `decoding as UE 5.7 (NOT recorded in the capture ...)` on every
run, which is how it was caught rather than believed — but a warning that is
printed and skimmed is not a guard. `mvtools.engine_version()` now also accepts
`MV_ENGINE_VERSION` at *analysis* time, so a capture taken without it can still
be decoded correctly instead of defaulting to a wrong answer, and
`results/validation_skyrunner.txt` has been regenerated as UE 5.2.


### Warp-and-difference, finally

The README said this test could not pass at p99 = 1.68px and that what it needed
was footage with several pixels per frame of real motion. That diagnosis was
right, and the fix was footage rather than code. On 120 frames of Skyrunner at
5–94px:

```
             condition    mean MAE    vs baseline   better than baseline
    no warp (baseline)     0.04908          +0.0%            0/103
       per-pixel field     0.03059         +37.7%          102/103
          sign-flipped     0.06959         -41.8%            0/103
     best global shift     0.04892          +0.3%           66/103
      mismatched frame     0.18118        -265.8%            0/98
```

102 of 103 pairs improved, and all three controls rank as they must — the one
that matters most being that the per-pixel field beats the **best possible rigid
translation** by 59.9%, which is what separates "this field carries per-pixel
information" from "the scene moved and any shift would have helped". The
multiplier sweep peaks at exactly **1.0**, the unmodified engine decode, with
nothing fitted to this test.

### Two capture-path failures only a real title could produce

- **Silent ring wedge.** The first burst landed 17 of 60 requested frames and
  then capture stopped dead for 14 minutes with not one line explaining it — no
  dropped writes, no stale-slot reclaims, no completion message. And because the
  F8 hotkey is gated on "no burst in progress", the hotkey was dead for the rest
  of the session too. A capture path that can stop silently *and take its own
  trigger with it* is worse than one that stalls loudly. It now reports slot
  states and fence values when it cannot find a free slot.
- **Throughput.** At 1708×1068, velocity is 14.8MB per frame plus 7.4MB of back
  buffer at ~90fps — roughly 2GB/s. Frames are skipped rather than stalling the
  game, which is the design, and `frames.csv` records the gaps honestly. Oxi at
  1212×760 never came close to exercising this.

### Is the capture taken after the LAST write? Yes - confirmed in RenderDoc

The target writes velocity in **two** places, which is the normal UE5
arrangement and not something the first title exercised:

1. the **base pass**, as GBuffer `SV_Target4` - the shader disassembled below,
   with material textures, DBuffer decals and five render targets;
2. a **dedicated velocity pass** - a `DrawIndexedInstanced` binding only the
   velocity texture and a depth target, with no texture inputs at all. That is
   `VelocityShader.usf` / `MainPixelShader`, for objects the base pass does not
   cover.

The capture triggers on the first `RENDER_TARGET -> shader-resource` transition
of the frame. If the dedicated pass ran *after* that edge, the dump would be
missing whatever it writes - and nothing in the extracted data would look wrong,
because a partially-written velocity buffer decodes to perfectly plausible
velocity.

**The barrier counts predicted we were safe.** Measured per frame:

```
RT->UAV = 1     UAV->SRV = 1     SRV->RT = 2     RT->SRV = 1
```

Entries into each state equal exits, and the only cycle consistent with those
totals is:

```
RT  --UAV-->  SRV  --RT-->  RT  --SRV-->  (next frame)
 |             |             |             |
 base pass     compute       dedicated     final read
 writes        reads         velocity      (TSR / motion blur)
                             pass writes
```

giving exactly one `RT -> shader-resource` edge per frame, as the last
transition to a readable state.

**And RenderDoc confirms it directly.** The velocity texture's resource-usage
list shows the **last `Rendertarget` usage at a lower EID than the transition to
`NON_PIXEL_SHADER_RESOURCE`**. Every write is complete before the barrier the
copy is recorded on. The capture point is correct.

Worth separating the two, though, because they are not the same quality of
evidence and the difference is the recurring theme of this document. The count
argument is arithmetic over aggregates: it constrains the cycle but cannot say
which draw sits between which barriers, and a different arrangement with
identical totals is possible in principle. The usage list is an observation of
the actual ordering. The first was right this time. The 53,324-frame barrier
invariant was also arithmetically airtight and still turned out to describe one
game's frame graph rather than velocity, so "the numbers only balance one way"
earns a check, not a conclusion.


### Chasing the 5% shortfall: two hypotheses, both wrong

`region_match` reports the decoded displacement as consistently ~5% larger than
the measured one (ratio 0.95). Two explanations were proposed, and the data
rejects both.

**Motion blur biasing the match low** was the obvious candidate: a smeared frame
correlates well at slightly-too-small offsets. But blur scales with speed, so the
ratio should degrade as displacement grows. Over 79 pairs:

```
corr(|dx|, ratio) = +0.108
```

Essentially nothing, and the sign is the wrong way. The band table said the same
thing and it was not noticed at the time - the engine ratio is 0.953 in the
5.6-20px band and 0.944 in the 20-94px band, i.e. flat across a 17x range of
speed. Blur is not driving this.

**Foliage-versus-background confusion** was the second: the decoded value is a
median over written pixels (foliage, carrying world-position-offset animation on
top of camera motion) while the template match locks onto whatever has the most
contrast in the crop, which is often the static background. That predicts the
ratio approaching 1.0 as the crop becomes more foliage.

It does the opposite:

```
 written fraction of crop    n    ratio measured/decoded
      0.091 - 0.110         20            0.963
      0.110 - 0.161         20            0.958
      0.161 - 0.275         20            0.962
      0.275 - 0.375         20            0.789

corr(written fraction, ratio) = -0.508
```

The agreement is *worse* the more foliage the crop contains, sharply so in the
top quartile. So the mechanism is not "the matcher tracks the background
instead"; it is that **dense foliage defeats the matcher outright**. Thin,
self-similar, non-rigidly moving structure is the case a rigid template match is
worst at - which is the same limitation already documented for the per-pixel
route, showing up here in the per-region one.

**What that leaves.** Excluding the foliage-dense quartile, the ratio is ~0.96
and flat, so roughly 4% remains unaccounted for. It is not speed-dependent, not
explained by which pixels the matcher favours, and cannot be a decode error in
the arithmetic sense because the encode has since been read directly out of the
compiled shader. The most likely remaining candidate is that every reference
available here is an image-matching reference, and all of them degrade on this
footage in the same direction.

Which is the argument for a different kind of ground truth entirely - see the
handoff note on reprojection from `View_ClipToPrevClip`. An analytical reference
computed from the camera matrices and depth has no correlation window, no
texture requirement and no failure mode on foliage, because it never looks at
the image at all.

Recorded as unresolved. Two hypotheses stated, tested, and rejected - one of
them predicting the wrong sign - is a more useful entry than a number quietly
attributed to "measurement noise".


### What is still not verified

- ~~The **~5% scale shortfall**, and the **Y axis**~~ — closed by the analytical
  reprojection below: 0.997 in X and 1.000 in Y against a reference that does
  not look at the image.
- ~~**Per-pixel** decode correctness on this title~~ — established on static
  geometry, over 2.9M pixels. **Foliage is still open**: the pixels carrying
  world-position-offset animation are excluded by the test's own selector, so
  nothing here measures them.
- The **teardown path in a shipping process**. `testhost` now genuinely
  exercises it — it did not before, see below — but the target was still only
  ever exited by closing the game.

## An analytical ground truth: depth, the View uniform buffer, reprojection

Every reference this project had used looked at the image, and all of them had
failed on this footage in ways that took real work to detect. This section is
the one that does not: for a pixel on static geometry, the previous frame's
screen position follows from its depth and `View_ClipToPrevClip` alone.

It is not an approximation of what the engine does. It is what the engine does,
in `TSRDepthVelocityAnalysis.ush`:

```hlsl
float3 ComputeStaticVelocity(float2 ScreenPos, float DeviceZ)
{
    float3 PosN     = float3(ScreenPos, DeviceZ);
    float4 ThisClip = float4(PosN, 1);
    float4 PrevClip = mul( ThisClip, View.ClipToPrevClip );
    float3 PrevScreen = PrevClip.xyz / PrevClip.w;
    return PosN - PrevScreen;
}
```

and `FetchAndComputeScreenVelocity` in the same file uses its result
*interchangeably* with `DecodeVelocityFromTexture` for pixels the velocity
buffer did not write. So on pixels where both exist and the surface is static,
the two must agree, and a regression of one against the other has no correlation
window, no texture requirement and no failure mode on foliage.

### The result: slope 1.000, per pixel, over 2.9 million pixels

```
 frame     pixels   slope X     r X   slope Y     r Y
     0     250575    1.0002 +1.0000    1.0014 +1.0000
     1     261372    1.0001 +1.0000    0.9995 +1.0000
     2     262678    0.9995 +0.9970    1.0051 +0.9974
     3     265895    0.9263 +0.4776    0.9991 +1.0000
     4     265473    0.9888 +0.8584    0.9988 +0.9997
     5     274417    0.9998 +1.0000    0.9994 +1.0000
     6     284090    1.0002 +1.0000    1.0015 +1.0000
     8     333159    0.9994 +0.9998    1.0004 +0.9997
     9     328389    1.0001 +0.9999    0.9993 +1.0000
    11     398301    0.9999 +0.9999    1.0010 +1.0000

POOLED over 10 frames and 2,924,349 pixels:
  axis X:  decoded = 0.9969 * analytical    r = +0.9984
  axis Y:  decoded = 1.0000 * analytical    r = +1.0000
```

Eight of ten frames land at slope 0.999-1.001 with **r = 1.0000**, in both axes.
So:

- **The ~5% shortfall is a property of image matching on this footage, not of
  the decode.** `region_match` reports 0.921 in X and 0.751 in Y against
  cross-correlated frames; against a reference that never looks at the image,
  the same decode reads 0.997 and 1.000. The two hypotheses tested and rejected
  in the previous phase were both looking in the right place - the reference -
  and the wrong part of it.
- **Y is closed too.** It read 0.751 against image matching and 0.479 in the
  per-pixel regression on the first title, with the honest note that a factor of
  ~2 in a single axis is exactly the shape of a real bug. It is not one. Y is
  the *better* axis here, at 1.0000 with r = 1.0000.
- **Per-pixel decode correctness is established on this title**, which
  `derive_scale` could not do: it reported slope ~0.7 and got worse the harder
  its matches were filtered, because thin foliage and motion blur defeat a 17px
  correlation window. The reference that fixes that is not a better matcher.

The de-jittered variant - subtracting `View_TemporalAAJitter` from both ends as
`Calculate3DVelocityBase` does for the encoder - reads 0.9977 / 0.9978, i.e.
very slightly *worse*. That is the expected ordering and worth having measured
rather than argued: `View_ClipToPrevClip` is built from
`ComputeInvProjectionNoAAMatrix()` and `ComputeProjectionNoAAMatrix()`
(`SceneView.cpp:2772-2775`), jitter-free at both ends, and the engine's own
`ComputeStaticVelocity` passes the raw pixel ScreenPos straight in.

**What this does not establish.** The comparison runs on written pixels whose
depth motion is explained by the camera. That excludes, by construction, the
pixels this reference cannot predict: wind-animated foliage, whose motion comes
from its material's world-position offset rather than from the camera. Those are
also precisely the pixels every image-matching reference failed on. So the
correct form of the claim is **per-pixel decode correctness on static geometry**,
and foliage remains open - not because the decode is suspect there, but because
nothing here measures it.

#### The selector that made the difference, and why it is not circular

The first version of this test selected "static" pixels by a percentile of
|V.z|, and produced frames regressing at 1.0000 next to frames from the same
capture regressing at 0.02, with the bad frames being the ones with more written
pixels - i.e. more foliage. SceneVelocity is written *for* dynamic and WPO
geometry, so "written" selects a population that includes everything the
reference cannot predict, and foliage sways mostly laterally, so a threshold on
|V.z| barely touches it.

The test used instead is per-pixel and uses a **different channel from the one
being regressed**. The reprojection predicts all three components of V; the
encoder stores V.z in channels 2/3. So each pixel is asked: is your *depth*
motion explained by the camera alone? Pixels that pass are not moving towards or
away from the camera under their own power, and their lateral motion is then a
fair question to put to the reference. Selecting on z and regressing on x/y
keeps the two apart.

It is a necessary condition, not a sufficient one - foliage swaying exactly
perpendicular to the view direction has no depth signature and will pass. The
supporting evidence that it selects the right population rather than a
convenient one is that it selects **three times as many pixels** as the
percentile filter did (2.9M against 1.0M) while the agreement improves from
0.63 to 0.997. A filter that was quietly selecting for the answer would narrow,
not widen.

### The full-screen field's own fidelity bug, caught by two tools disagreeing

Making the dense field the visual deliverable meant looking at it closely
enough to notice that `reproject.py`'s coverage line and `make_video.py`'s
independent computation of the same number disagreed: 78.5% "reconstructed"
against 71.5%, on the identical frame of the identical dump.

The 7-point gap was the far plane. UE5's reversed-Z buffer encodes the sky, and
anything the depth pass never wrote, as `DeviceZ == 0` — 7.0% of frame 5 here —
and `ComputeStaticVelocity`'s matrix multiply has no way to know that. Fed a
meaningless depth, it does not fail or return zero; it returns a *finite,
plausible-looking* clip-space delta, because nothing in `float4x4` multiplication
checks whether the input meant anything. Measured on this frame: up to 0.92 in
clip space, a median of **~940px/frame** on the sky pixels alone. `reproject.py`
built its dense field with `np.nan_to_num()` guarding only against non-finite
values, which this is not, so the sky was filled with that number, counted as
"reconstructed" in the printed coverage line, and painted into the right-hand
panel in the same colour vocabulary as the tree and the grass.

This is the same class of error the analytical reference was built to be immune
to — a confident number with nothing behind it — except this time it came from
the reference itself rather than from image matching, at the one point where the
reference's own precondition (there must be real depth to reproject) silently
failed to hold. `make_video.py` was written afterwards and independently, gating
the reconstruction on `device_z > 1e-7` because that is the same guard the
regression already uses to define `usable` pixels (see "the selector that made
the difference" above) — and it was that second, independent computation
disagreeing with the first that surfaced the bug, not a review of either script
alone.

Fixed in `reproject.py` by the same guard: pixels with no real depth are neither
`written` nor `reconstructable`, are excluded from the coverage and displacement
statistics, and are drawn black rather than filled. The corrected figure reads
21.5% buffer / 71.5% reconstructed / 7.0% neither, and the two tools now agree
on all three numbers because they now share the precondition.

### Reading a constant buffer that has no resource pointer

`SceneVelocity` and `SceneDepth` arrive as `ID3D12Resource` pointers in a
barrier. The View uniform buffer does not. UE5's D3D12 RHI binds uniform buffers
as **root** constant buffer views —

```cpp
Context.GraphicsCommandList()->SetGraphicsRootConstantBufferView(BaseIndex + SlotIndex, CurrentGPUVirtualAddress);
```

`D3D12DescriptorCache.cpp:746` — so the only thing crossing the API boundary is
a raw `D3D12_GPU_VIRTUAL_ADDRESS`. There is no D3D12 call that maps an address
back to the resource that owns it.

The obvious route is to correlate the address against resources seen at
creation. It does not work here: it needs a `CreateCommittedResource` /
`CreatePlacedResource` hook, catches only resources created *after* injection,
and UE5's constant buffer pool has long since reached steady state by the time a
hook goes in ~10s into a session. It fails by finding nothing.

What does work is not correlating at all. **D3D12 root descriptors take a raw
GPU virtual address with no resource and no descriptor heap**, so a one-line
compute shader bound with `SetComputeRootShaderResourceView(index, address)` can
read that memory into a buffer we own:

```hlsl
ByteAddressBuffer   Src : register(t0);
RWByteAddressBuffer Dst : register(u0);
Dst.Store4(DstOffset + offset, Src.Load4(offset));
```

recorded onto our own command list at Present. Not the game's: setting a compute
root signature and PSO is command-list state, and doing it on a list the game is
still recording into would corrupt whatever it draws next.

**The hazard, and the bound put on it.** A root descriptor is not
bounds-checked. Reading past the end of the resource it points into is undefined
and can fault the GPU, which in a shipping game means a removed device. We do
not know how big the buffer is — that is the whole problem. What we do know is
that D3D12 buffer resources are placed on 64KB-aligned addresses, so an address
known to be inside a valid buffer is inside a 64KB block belonging to that
buffer. The read is clamped to the remainder of that block, and a candidate too
close to the end to hold the fields wanted is skipped rather than truncated.
That is a bound, not a guarantee from the spec, and it is the difference between
reading a few KB past a small constant buffer into the same page and walking off
the end of an allocation.

**Which address is the View buffer is not decided in the process.** The hook
ranks root-CBV addresses by how often they were bound in the frame and copies
the top few; the choice between them is made offline. Deciding it in-process
would mean hardcoding a struct offset for one build of one game, which is
exactly what the previous phase of this project was about.

### The offset, derived rather than asserted

The handoff note for this phase gave `View_ClipToPrevClip` as a `float4x4` at
byte offset **1872**, sourced to "the disassembly quoted in DEBUGGING.md". That
disassembly is not in this document — the listing under "The encode, read out of
the shipping game's own compiled shader" contains the encode arithmetic and no
constant buffer offsets. The number was passed forward as established; the
evidence it cited was not there. So it was treated as a claim to check.

The derivation used instead needs nothing from any other title. Search the
captured buffer for a `float4` whose last two components are the **reciprocals**
of the first two: that is `View_ViewSizeAndInvSize`, and two of four floats
being the reciprocals of the other two is not a coincidence a few KB of struct
will produce by accident. On Skyrunner it lands at byte **2064** reading
`(1707, 1044)` — the view rect — with a second such `float4` 48 bytes later at
`(1708, 1044)`, which is `View_BufferSizeAndInvSize`, the render target rounded
up. Note that the two differ: matching the anchor against the velocity texture's
extent would have rejected the correct answer, so the size is read out rather
than matched.

Stepping back from that anchor by the member ordering in `SceneView.h` —

```
FMatrix44f ClipToPrevClip          64
FMatrix44f ClipToPrevClipWithAA    64
FVector4f  TemporalAAJitter        16
FVector4f  GlobalClippingPlane     16
FVector2f  FieldOfViewWideAngles    8
FVector2f  PrevFieldOfViewWideAngles 8
FVector4f  ViewRectMin             16   = 192 bytes
```

puts `View_ClipToPrevClip` at **1872**, which agrees with the asserted number.
Three independent consistency checks are applied and printed rather than a bare
verdict: `ClipToPrevClip` and `ClipToPrevClipWithAA` are adjacent members
differing only by the jitter, so they must be close but not equal (measured
max difference 0.0016); `TemporalAAJitter` must be a sub-pixel offset (measured
~9e-4); `ViewRectMin` must be a non-negative integer pixel offset (measured
(0, 0)).

UE 5.7.1 inserts two more `FVector2f` members in that run, making the distance
208 rather than 192, so the offset is genuinely version-dependent and both
layouts are tried.

**One thing worth being able to point at.** `mul(v, M)` over these bytes is only
a row-vector product if the matrix is packed row-major, and HLSL packs constant
buffer matrices **column**-major by default. UE compiles all its HLSL with
`D3DCOMPILE_PACK_MATRIX_ROW_MAJOR` (`D3DShaderCompiler.cpp:1343`, and `-Zpr` on
the DXC path), which is what makes the shader's view of the matrix agree with
`FMatrix44f`'s C++ memory layout. Without that flag the shader would see the
transpose and every reprojection would be wrong in a way that still looks like a
plausible motion field.

### SceneDepth and CustomDepth are byte-identical

Depth identification looked like the easy half: the only `ALLOW_DEPTH_STENCIL`
texture at the velocity extent. It is not, and the reason is instructive.

UE5 creates **CustomDepth** in the same place as scene depth, at the same
`SceneTexturesConfig::Extent`, with the same `PF_DepthStencil` format and the
same `TexCreate_DepthStencilTargetable | TexCreate_ShaderResource` pair
(`SceneTextures.cpp`). Their D3D12 descriptors are identical in every field. No
structural test can separate them, and the structural test is the half derived
from engine source and therefore the half worth trusting.

The first session identified depth cleanly and the second refused as ambiguous —
same game, same settings. The first was luck: only one of the two had been
shortlisted by the time the decision ran. That is the same shape as the barrier
signature that survived 53,324 frames and turned out to describe one game's
frame graph, arrived at from the other direction — a filter looking decisive
because the population happened to be smaller.

What separates them is how hard they are worked. Measured on Skyrunner over the
same window:

```
depth:  1708x1044 fmt=19 flags=2   depthWrite->readable=451  (5.011 per frame)   <== SELECTED
depth:  1708x1044 fmt=19 flags=2   depthWrite->readable=51   (1.020 per frame)
```

Scene depth is written and read repeatedly across a frame — prepass, base pass,
and every pass that samples it take turns with it. CustomDepth is rendered at
most once, and only when something in the scene asks for it. A 5x separation.

This is scored evidence with a required margin, not a gate, and both candidates'
numbers go in the log so the choice can be audited. It is deliberately not
phrased as a rule about what depth *is*. The verdict line also changed: when
there is more than one survivor it now says "the top of N structurally
indistinguishable candidates, chosen on per-frame usage. **NOT a unique match**"
rather than the "the only ALLOW_DEPTH_STENCIL texture at the velocity extent"
it printed regardless.

### The pairing bug: right answer, wrong frame

The first real capture produced a result that was obviously right on most frames
and obviously wrong on a few:

```
 frame     pixels   slope X     r X   slope Y     r Y
     1      57622    0.9997 +0.9710    0.9998 +0.9971
     2      63572    0.1263 -0.9324    0.2956 +0.9201
     8      61710    1.0001 +0.9996    0.9991 +1.0000
    11      59466    0.3697 -0.7908    0.9253 +0.9555
    12      57052    1.0001 +1.0000    1.0006 +1.0000
```

Slope 1.0001 with r = 1.0000 on some frames; **anti**-correlation, r = -0.93 and
-0.79, on others. A decode cannot be exactly right on one frame and inverted on
the next, so the fault was in the pairing rather than in the arithmetic —
anti-correlation is what applying a neighbouring frame's `ClipToPrevClip` to a
turning camera looks like.

The cause was where the root-CBV bind counts were taken. Taken at `Present`, the
counting window is "everything since the last Present", which is one frame's
command stream only if the game never begins recording the next frame before
presenting the current one. UE5 does, sometimes. When the window straddles a
boundary the most-bound address can be the *next* frame's View uniform buffer.

The fix is not a filter. The velocity buffer's `RENDER_TARGET ->
shader-resource` edge is a point inside frame N's own command stream, after
every draw that binds the View uniform buffer for that frame, so the snapshot is
taken there instead — tying the constant buffer to the same moment as the
velocity copy by construction. A Present-interval fallback remains for a title
that binds nothing before its velocity pass, and it says so in the log, because
a weaker pairing beats none.

### Three capture-path failures the same session produced

- **45 of 60 frames lost to depth bandwidth.** Skyrunner issues four
  `DEPTH_WRITE -> readable` edges per frame and hit the cap, so "record on every
  edge, last one wins" was copying ~38MB of depth per frame on top of 14MB of
  velocity and 7MB of back buffer. Depth copies now stop at the velocity edge,
  which is *also* exactly the depth the frame's velocity was rendered against;
  edges after it belong to later passes.
- **A burst that could never finish.** `framesRemaining` counts frames
  *submitted*, so when velocity landed intermittently the burst ran for a
  further 5,700 frames — still copying depth on every one — with F8 dead for the
  rest of the session, because the hotkey is gated on "no burst in progress".
  Same silent-wedge class as the ring stall, reached through a completion
  condition that can simply never be met. There is now a Present budget.
- **Partial frames.** Backpressure was applied per blob, so when the write queue
  filled, a frame could land with velocity but no depth purely according to
  which write crossed the threshold — indistinguishable, offline, from "depth
  was never identified". A frame is the unit the tools reason about, so it is
  now the unit that gets dropped, and bursts begin at a frame boundary so the
  first captured frame is one whose recording was watched from the start.

### The harness had been crashing on every run since it was written

Running `mv_testhost` as instructed, its **process exit code** was
`0xC0000409` — `STATUS_STACK_BUFFER_OVERRUN`, the code `abort()` raises.
Reproduced on the unmodified previous revision, so it was not new. It had never
been noticed because it happens after every verdict has printed and after
`main()` has returned its 0: the console output was a complete, correct PASS and
only the exit code disagreed, and nothing was reading the exit code.

Two causes, and the second is the more interesting.

1. A `std::thread` destroyed while still joinable calls `std::terminate()`. The
   hotkey pollers and the writer thread are file-scope objects; on the
   process-exit path `ShutdownCapture` returned early without joining *or*
   detaching them, and their destructors ran a moment later during CRT teardown.

2. **`FreeLibrary` was never unloading the hook at all.** The MSVC CRT takes a
   module reference for every thread whose entry point is inside the DLL and
   releases it only when that thread exits. Both hotkey pollers run for the life
   of the process, so `FreeLibrary` decremented a count that never reached zero,
   `DLL_PROCESS_DETACH` was never delivered, and the entire FreeLibrary teardown
   path — MinHook unpatching, draining the writer — did not run. The harness has
   described itself as exercising that path on every run for its whole life. It
   was calling `FreeLibrary`; the module stayed mapped; nothing checked.

   Caught by asking `GetModuleHandle` after `FreeLibrary` and getting a non-null
   answer back.

There was an `MvPrepareUnload` export that stopped the hook's own threads so
the reference count could reach zero, and the harness asserted the module
really unloaded rather than that `FreeLibrary` returned. That export and the
harness's `FreeLibrary` call were both removed in a later pass: a game never
unloads the hook either, so once the fix was verified there was nothing left
that needed exercising on every run. `MvFlushCapture` (drains the writer
queue, leaves the module loaded) replaced it - see dllmain.cpp.

**And what the harness caught next.** With depth copies stopping at the velocity
edge, the harness reported PASS on both identification verdicts and zero
debug-layer errors while producing a dump with **no depth in it at all** —
because in the harness the depth edges came *after* the velocity edge. Nothing
was checking the files. "Identification picked the right resource" and "the
bytes reached disk" are different claims. The harness now asserts that every
captured frame carries velocity, depth and a View constant buffer, models both
frame orderings, and the hook carries a fallback for the ordering it does not
prefer.

### Resolved: velocity stopped being captured because the game stopped rendering it

The capture that produced the result above landed **12 of 60 requested frames**,
and they are 12 *consecutive* game frames (10 adjacent pairs verified against
`frames.csv`). After that, for the remaining 468 Presents of the burst, the
depth barrier fired every frame and the velocity barrier did not. The previous
session called the reason unknown, ruled out five candidates, and left a
standing suspicion that the extra per-frame work this build records onto the
game's command list was to blame.

**The suspicion was wrong, and one of the five things ruled out was the answer.**

The session's own `mv_candidates.log` records every barrier the identified
velocity resource took, and grouping it into contiguous runs of frames settles
it outright:

```
frames with a candidate barrier: 9142     contiguous runs: 18

      91..8559    len 8469    then 11 frames with none
    8571..8579    len    9    then  6 frames with none
    ...            (a ~500-frame stretch of intermittency)
    9015..9017    len    3    then 27 frames with none
    9045..9049    len    5    then  5 frames with none     <- F8 was at 9042
    9055..9061    len    7    then 271,123 frames with none
  280185..280472  len  288    then 92 frames with none
  280565..280645  len   81
```

The edge fired on **every one of frames 91–8559**, went intermittent for about
500 frames, stopped completely at 9062 — and came back at 280185, on **the same
resource pointer**, and then fired every frame again. So the buffer was never
replaced, never re-pooled and never mis-identified: it stopped being *produced*,
and later started again. The degradation also begins around frame 8560, roughly
**483 frames and 4.5 seconds before F8 was pressed** at frame 9042, while the
hook was doing nothing but watching — no burst was active, so no depth copy and
no constant buffer read had been recorded onto anything. The "our extra work
costs frames" hypothesis is falsified by the game's own barrier record, not by
argument.

**And the heartbeat was saying so the whole time.** The ruled-out table above
reads "the game stopped rendering a scene (menu, alt-tab) — heartbeat shows 418
barriers/frame, steady, across the whole period". That is the evidence *for* the
proposition, entered against it. The heartbeat logs cumulative counters, so a
rate has to be differenced out of consecutive lines; doing that:

| frames | fps | barriers/frame | OMSetRenderTargets/frame |
|---|---|---|---|
| startup | 71 | 596.0 | 137.0 |
| 8100 (edge still every frame) | 95 | 535.9 | 108.6 |
| 9000 (burst; edge intermittent) | 104 | 430.1 | 76.8 |
| 12000 → 270000 (silence) | 97–101 | **418.4** | **74.0** |
| 280500 (edge is back) | 94 | 420.1 | 76.8 |
| end of session | 78 | 593.3 | 136.1 |

418.4 barriers and 74.0 render-target binds per frame, constant **to one decimal
place across 258,000 frames and 45 minutes**. Gameplay does not do that; the
figures before and after it wander between 420 and 596. A fixed command stream
replayed every frame does — a menu, a results screen, a paused run, which this
title still presents at ~98fps. The steadiness of 418 was never evidence that
the game was still rendering a scene. It was the signature of a screen that had
stopped changing, and it was read as the opposite because a steady number looks
reassuring.

So the sequence is: the scene had already been intermittent for 4.5 seconds
when F8 was pressed (frame 9042), the edge fired 19 more times and then stopped
for good 0.3 seconds later, and the burst spent its remaining 468 Presents on
whatever came next — a menu, a results screen — at ~100fps. Nothing was wrong
with the ring, the write queue, identification or the copies. **The capture
path did exactly what it should; the operator pressed F8 under a second too
late for a clean 60-frame burst.**

Two things came out of it in code:

- The hook now says this. `VelocityPassIsSilent` reports once when the
  identified resource has not taken its produce-then-consume edge for 300
  presented frames while the game carries on presenting, and again when it
  resumes; a burst in progress ends on that condition instead of grinding
  through its whole Present budget. One line in the log would have replaced this
  entire investigation.
- It deliberately does **not** reopen identification. That was the first thing
  written, and the run-length data above is what argued it back out: the
  resource came back, so discarding a correct answer and re-searching against a
  scene that is not being drawn would have been a fix for a cause that does not
  exist here.

None of this affects the reprojection result — the 12 frames are complete,
consecutive, fence-verified and internally consistent, and 2.9M pixels is not a
small sample.

**What is still not explained.** From around frame 8000 the velocity resource
began taking its `RENDER_TARGET -> shader-resource` edge **twice** per frame
rather than once, and kept doing so through the burst (`4->64 x2, 64->4 x2` on
every logged frame from 8000 on, against `x1` before it). That is a change in
the game's own frame graph — a second view, a scene capture, or the beginning of
whatever transition ended the run — and nothing here identifies which. It is
harmless to the capture (the second edge is deduplicated by `hasVelocity`) and
it is recorded because it is the one thing in this episode still unaccounted
for.

**A silent cliff found on the way.** Every identification log line in that
session — shortlists, near-misses, both searches — stopped at t+96s and nothing
was printed again for at least the remaining 47 minutes the session ran.
`g_identifyBudget` caps how many
distinct resources are ever `GetDesc()`-ed at 8000, and it ran out without
saying so; 3,135 had been profiled within the first 24 seconds. That did not
cause this failure, but it is indistinguishable from "the game stopped creating
resources" and it made the pooled-resource hypothesis impossible to rule out
from the log. Worse, `ReopenIdentification` clears the seen-set and would then
have been rejected one layer down for want of budget — reopening after
exhaustion was a guaranteed no-op that would have searched forever and reported
that it had reopened. Both are fixed: the budget lives next to the seen-set,
says so when it is spent, and is refilled when the search reopens.

### A moderate-speed capture, and the extreme-speed degradation resolved

Getting a 60-frame capture with depth and the View buffer at a speed slow
enough to read as a video took four F8 presses on the running game, each
ending in the same shape of failure this phase had just fixed detection for:
`VelocityPassIsSilent` correctly reported the game had stopped rendering (a
death, a menu) seconds before or after the key was pressed, and the burst
ended cleanly instead of wedging. That is the fix working as designed, not a
new bug - it just took several tries to line the press up with a clean
stretch of gameplay.

The fourth attempt landed 60/60 frames, fence-verified, at 0.1-61px/frame
median - no saturation, nothing extreme. Regressed alone (isolating capture
indices 120-179 from the raw session, which also contains two earlier, much
faster bursts), it reproduces the analytical result independently:

```
skyrunner_video (60 frames, moderate speed, separate capture session):
  POOLED over 59 frames and 18,076,704 pixels
    axis X:  decoded = 1.0000 * analytical    r = +1.0000
    axis Y:  decoded = 0.9997 * analytical    r = +0.9998
```

Slope 1.0000/0.9997 against the original 0.9969/1.0000, on a completely
independent capture, over 6x the pixels. This is a second, unrelated
confirmation of the slope-1.000 result, not a re-measurement of the same data.

**Resolved: the extreme-speed degradation is the velocity buffer's own encoding
running out of range, confirmed exactly rather than assumed.** Pooling the
reprojection over the RAW session's earlier, much faster bursts (mouse rotation
up to 982px/frame, one frame's channel 0 fully saturated at 1.0000) gives:

```
  axis X:  decoded = 0.5912 * analytical    r = +0.8567
  axis Y:  decoded = 0.6286 * analytical    r = +0.8719
```

against slope 1.0000 on the moderate-speed frames from the identical session,
same identified resources, same `View_ClipToPrevClip` derivation. The two
populations differ only in how fast the camera was moving, so the drop is real
and tied to speed, not noise.

Three hypotheses were tested live against RenderDoc and the raw dump before
landing on the right one:

1. **Downstream motion-blur clamp.** RenderDoc showed the engine's own
   `VelocityFlatten` compute pass (`VelocityTexture` + `DepthTexture` ->
   `OutVelocityFlatTexture` + a tiled `OutVelocityTile`) pinning one channel at
   an identical raw value (`42.5`) across three different screen positions in
   the reconstructed background - a genuine clamp, and a second, separate
   channel showing the exact half-float bit pattern for negative infinity
   (`0xFC00` = 64512). Both real, both ruled out as the cause here: that pass
   only *reads* `SceneVelocity`, and our own decode never touches its output.
2. **A numerical blow-up in our own reprojection math**, motivated by the
   engine's own -infinity sighting above - the obvious candidate being the
   perspective-divide denominator (`pw` in `compute_static_velocity`) going
   through zero at extreme rotation. Checked directly against the fast frames:
   `pw` stayed bounded between 0.4 and 1.5 on every one of them. Ruled out.
3. **The velocity buffer's own encoded range.** Per-frame regression (not
   pooled) shows the actual shape: most fast frames still regress at slope
   ~1.000, and a handful (6, 13, 17, 23, 41, 44) show `decoded`'s per-frame max
   pinned at exactly **2.008** clip-space units while the analytical prediction
   for the same pixels runs far past it - up to 18.6 on frame 44. Confirmed at
   the byte level: on frame 44, **32.2% of all written pixels** have raw
   channel 0 sitting at `0.999863-1.000000` - the hard ceiling of
   `R16G16B16A16_UNORM` - decoding to the identical capped value every time.
   This is the same "±2 in clip space" ceiling `skyrunner_fast`'s README entry
   already documented against block matching; this measures it against the
   analytical reference instead, and pins the exact value rather than an
   order-of-magnitude estimate. A hard per-pixel clamp, not a smooth bias, is
   why pooling a handful of over-range frames into a regression dominated by
   the largest-magnitude points drags the fitted slope down so far even though
   most individual frames are unaffected.

**A capture lost to the exact mistake this project's captures README already
warns about.** The first successful moderate-speed take (9 frames, slope
1.0006/1.0005) was overwritten mid-session when the game crashed, a second
process was injected, and its OWN capture-index counter restarted at 0 -
straight into the same `MV_DUMP_DIR` as the first process's frames. The
capture-index counter is per-process (`g_captureCounter`, a plain `atomic<int>`
initialised at DLL load), so nothing about it survives a reinjection, and
nothing in the capture path checks whether a `vel_00000.bin` already exists
before overwriting it - by design, since a session is expected to build up one
consistent numbering. The fix in the moment was operational, not code: point
any future reinjection at a fresh directory, not just a fresh session. Whether
the capture path should also refuse to overwrite an existing indexed file is
an open question - it would have saved this specific loss, at the cost of a
capture stalling entirely if a stale directory is reused on purpose.

## Other findings worth recording

**Three crashes, all diagnosed from evidence rather than guesswork.** UE5
packaged builds write their own crash reports to
`%LOCALAPPDATA%\<Project>\Saved\Crashes\UECC-Windows-*\CrashContext.runtime-xml`
— far more useful than Windows Event Viewer, which mostly surfaces generic
WER/telemetry noise. `<ErrorMessage>` gives the exception type/address and
`<PCallStack>` the faulting callstack with module+offset.

1. **Injecting immediately at process creation** crashes the game. A ~5–6s
   stabilization delay is reliable; racing the game's own startup is not worth
   it.
2. **Calling `GetDesc()` inside a `CreateRenderTargetView` hook** crashes with
   `EXCEPTION_ACCESS_VIOLATION`. D3D12 view creation does **not** take a
   reference on the resource — a view is just a descriptor-table entry — so
   touching the pointer after the original call returns races the engine's own
   reference handling.
3. **Unbatched logging** (~6,500 flushes/sec) crashed the game — but notably
   `mv_hook` appeared **nowhere** in that crash's callstack, unlike (1) and
   (2). Diagnosis: the I/O volume stalled the render thread badly enough to
   expose a latent timing bug elsewhere in the game, rather than corrupting
   anything directly. Fixed by buffering and flushing every 500 lines; the
   identical workload then ran for over 1,000,000 log lines with zero crashes,
   confirming the diagnosis.

The distinguishing detail in (3) — *our module is absent from the callstack* —
is what separated "our code is broken" from "our code is too slow," which are
very different fixes.

**Operational hazard: don't inject a rebuilt DLL from a different path.**
Injecting `hook/Release/mv_hook.dll` while `hook/Debug/mv_hook.dll` was still
loaded gives Windows two *different paths*, so it maps two separate modules —
each with its own globals and its own statically-linked MinHook, both patching
the same vtable targets, the second chaining onto the first's trampoline. The
tell was the heartbeat log showing two independently-growing counter sets
interleaved, plus two `InstallD3D12Hooks: all hooks installed` lines in one
process. Always restart the target before injecting a rebuild, and keep one
canonical output path so `LoadLibrary` refcounts an already-loaded module
instead of mapping a second copy.

## The overlay verified, and external teardown found broken rather than confirmed

Three unverified claims going into this phase: the live overlay (fixed blind,
never watched running), teardown against a real game (never attempted, only
`testhost`'s in-process synthetic unload), and WPO/foliage pixels (unmeasured
by construction). All three were exercised. Two closed clean. The third closed
with a real, previously-unknown problem instead of a green check — which is
the more valuable outcome of the two, and the reason this project keeps this
log instead of just a pass/fail badge.

### A regression test that was not actually testing what it claimed

Running the full `mv_testhost` regression before touching a live game (the
point of having one) surfaced a real bug on first run: `--pipeline skyrunner
--ambiguous` failed identification outright instead of refusing.

```
[testhost]   perfect twin           000001B0F6102F00 <== SELECTED
[testhost] FAIL - identification picked one of two indistinguishable candidates instead of
                  reporting the ambiguity
```

The hook's own log showed why: the two fmt=11 survivors scored **16 and 11**,
not tied — a comfortable 5-point margin, so identification's own ranking
logic correctly (by its own rules) picked a winner rather than refusing.
The "perfect twin" decoy in `testhost/src/main.cpp` was hardcoded to
`Behaviour::Velocity` (a clean 2-cycle resource) regardless of pipeline, but
`--pipeline skyrunner`'s own SceneVelocity uses `Behaviour::VelocityComputeAssisted`
(5 transitions, through `UNORDERED_ACCESS` — the shape measured on the
third-party title). So for oxi the twin really was a perfect twin (both
`Behaviour::Velocity`) and the test passed; for skyrunner it was a *cleaner*
decoy standing next to a messier real resource, and identification correctly
preferred the cleaner-looking one — the decoy, not the real buffer. The fixture
had silently stopped constructing the ambiguity it claimed to test the moment
`--pipeline` was combined with `--ambiguous`, which as far as this session's
git history shows had never actually been run together before.

Fix: the twin now takes `lineUp.front().behaviour` — whatever the real
SceneVelocity for the active pipeline actually is — instead of a hardcoded
constant, so "same behaviour" means what the comment above it always claimed.
All four combinations (`oxi`/`skyrunner` × plain/`--ambiguous`) pass clean
after the fix, exit code 0 each.

This is worth sitting with beyond the one-line fix: a test that always
constructs its own ambiguity relative to whichever behaviour the "real"
resource happens to have that run is more honest than one with a fixed decoy,
precisely because it will not silently stop testing anything the way this one
did when a second pipeline shape was added beside it.

### The WPO synthetic encode/decode round-trip: built, and it passes

`testhost/src/wpo_encode_test.cpp` and `tools/wpo_synthetic_test.py` (see the
header comments in both for the full design) render a known, deterministic
`(Vx, Vy, Vz)` grid — swept to `[-2, 2]` clip units in X/Y (just inside the
documented 2.008 ceiling, so the sweep covers the encoding's nonlinear span
without reaching saturation) and `~1e-5` in Z — through the real
`EncodeVelocityToTexture()`/`VELOCITY_ENCODE_DEPTH` path transcribed exactly
from `Common.ush`, dump it in the same `meta.txt`/`*.bin` shape a real capture
uses, and check the unmodified, already-shipping `mvtools.py` decode functions
invert it correctly:

```
mv_testhost.exe --wpo-encode-test <dir>
  wrote 131072 bytes, rowPitch=2048, 0 debug-layer error(s)

python tools/wpo_synthetic_test.py <dir>
  decoded vs decode(round_to_unorm16(encode(true))) - the pass/fail gate:
    X:  max err 1.211e-04 (0 pixel(s) over their own tolerance)
    Y:  max err 9.174e-05 (0 pixel(s) over their own tolerance)
    Vz: max err 1.819e-12, mean 1.487e-13
    bHasPixelAnimation: 0 / 16384 pixels mismatched
  PASS
```

This does not mean WPO pixels in a real capture decode to physically correct
velocities — nothing here simulates WPO vertex displacement, only the
encode/decode format, which is source-agnostic by construction (see the file's
own header for why that is enough to answer the actual open question). What it
does mean: nothing about this project's decode logic is specific to
static-camera-predictable motion. The analytical reprojection's exclusion of
WPO pixels remains a property of the *reference*, not evidence the decode
mishandles them — the two failure modes the previous phase left conflated are
now separated, and it is the reference, not the decode, that cannot see WPO
pixels.

Note for whoever reads this file next: `wpo_encode_test.cpp` did not compile
the first time it was built this session — missing `#include <dxgi1_4.h>`
(needed for `CreateDXGIFactory2`/`IDXGIFactory4`, present in `main.cpp` but not
copied into this newer file) and two wrong enum names
(`D3D12_TEXTURE_COPY_LOCATION_{SUBRESOURCE_INDEX,PLACED_FOOTPRINT}`, which do
not exist — the real names are `D3D12_TEXTURE_COPY_TYPE_*`). Both were fixed
before the PASS above. The file had apparently never been built successfully
before this session, which is exactly the "unverified claim" category this
project's own ethic warns about — a test that cannot compile is a stronger
kind of untested than a test that merely has not been run.

### The overlay, watched running for the first time

Injected the rebuilt hook (RTV-descriptor-per-in-flight-frame fix, transient
velocity-copy-lifetime fix — both previously fixed blind) into a live,
running Skyrunner (`MV_ENGINE_VERSION=5.2`, fresh `MV_DUMP_DIR`). Identification
succeeded cleanly at the real resolution (1708x1068), score=13, survivors=1 —
unrelated to this phase, but confirms the rebuild didn't regress the path
everything else depends on.

F7 cycled through both non-Off modes; both were **visually confirmed correct
by direct observation of the running game**, not inferred from logs:

- **Mode 1 (`SceneVelocity`, raw channels)**: confirmed correct. Log:
  `overlay: mode -> SceneVelocity (raw channels, engine view)`, `overlay:
  initialised`, `overlay: velocity copy texture created 1708x1068 fmt=11` — no
  errors.
- **Mode 2 (composited over gameplay)**: confirmed correct. Log: `overlay:
  mode -> SceneVelocity over gameplay`.

Both switches were clean in the log (no debug-layer messages, no gap in the
per-Present heartbeat counter) and the game stayed alive and rendering
throughout — but the thing that actually closes this claim is a person looking
at the screen and saying so, which is what happened here, not an absence of
errors. The RTV-lifetime and texture-lifetime fixes that were "fixed blind"
going into this phase are now watched-running claims instead.

**F8 during the overlay:** triggered a capture burst while mode 2 was active,
to test the header comment's claim of "no interaction with the F8 recording
path." The burst reported 60/60 frames identified, but the write queue
dropped **25 of them** to backpressure (30MB/frame at this resolution with
depth + View buffer). Rather than conclude the overlay caused that, a second,
control burst was taken immediately after with the overlay back at Off: it
dropped **26 of 60** — the same rate. Dump directory confirms: 69 `vel_*.bin`
files landed out of 120 requested across both bursts (57.5%, matching the
cumulative log count). **Conclusion: the overlay does not measurably worsen
the F8 capture path's drop rate** — the header comment's claim holds, at least
for this concern.

The baseline drop rate itself (~42%) is a separate, open observation: the
previous `skyrunner_video_final` capture (also 60 frames, also with depth and
the View buffer) landed 60/60 clean. Nothing here identifies why this session's
bursts dropped as heavily — this machine was simultaneously running a full
build, several `mv_testhost` regression passes, an editor, and this session's
own automation, any of which could be competing for the same disk I/O the
capture writer thread needs. Recorded rather than diagnosed: worth a clean-machine
repeat before treating it as a property of the capture path itself.

**Other false starts.** Hook diagnostics initially gated on
`IDXGISwapChain::Present` alone showed zero activity — UE5 presents via
`Present1`. And `D3D12CreateDevice(nullptr, ...)` can silently select the
integrated GPU on a hybrid-graphics laptop, installing valid hooks into a
driver the game never calls: no crash, no activity, nothing obviously wrong.
Fixed by explicitly selecting via
`IDXGIFactory6::EnumAdapterByGpuPreference(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE)`.

## Something double-injects mv_hook.dll into Skyrunner, unprompted

Found as a side effect of a since-removed investigation (external `--unload`
teardown, see NEXT.md and the note on scope below), but the finding itself is
independent of that feature and still relevant: `mv_testhost` was run clean
(`--hook <mv_hook.dll> --pipeline {oxi,skyrunner} × {plain,--ambiguous}`, 4/4,
exit code 0 each), then Skyrunner was launched fresh (confirmed via `tasklist`
— no Skyrunner or Valfreyja process existed beforehand) and this worktree's
`mv_hook.dll` was injected once.

A few minutes later, `mv_hook.log` showed something wrong: **two independent,
interleaved heartbeat counters**, each incrementing steadily but at different
rates and different absolute values, both stamped with real, current
timestamps:

```
[...] heartbeat: frames=8100 ... [worktree copy, series A]
[...] heartbeat: frames=3300 ... [a second copy, series B]
[...] heartbeat: frames=8400 ...
[...] heartbeat: frames=3600 ...
```

`Get-Process -Id <pid> | Select Modules` confirmed it directly: **two
`mv_hook.dll` modules, at two different base addresses, loaded from two
different full paths** —

```
mv_hook.dll  D:\hook\ue5-motion-vectors\build\hook\Release\mv_hook.dll                                (main checkout)
mv_hook.dll  D:\hook\.claude\worktrees\agent-ada9292fd073e08cd\ue5-motion-vectors\build\hook\Release\mv_hook.dll  (this worktree)
```

This session never touched the main-checkout path. A controlled follow-up
test made the mechanism unambiguous: kill everything, relaunch Skyrunner, and
**inject nothing at all**. Within 20 seconds, with zero action from this
session, the main-checkout copy was loaded anyway. `AppInit_DLLs` is off
(`LoadAppInit_DLLs=0`, empty value) so it isn't a system-wide injection
mechanism — the most likely explanation, given the task brief's own warning
that the live game process is shared, unpartitioned OS-level state across
worktrees, is the user's own concurrent activity on the machine (the task
brief said a manual review pass was happening in parallel; a stray screenshot
taken during this investigation, deleted immediately without inspecting its
contents further, confirmed another active VS Code/terminal session was live
on this machine at the time). Whatever the exact source, **something injects
the main-checkout `mv_hook.dll` into every freshly-launched Skyrunner process
within roughly 15-70 seconds, without being asked to.** It means two copies of
the hook can end up loaded any time a second session also injects into the
same machine's Skyrunner, unless the target is restarted immediately before
every injection and used immediately after. See NEXT.md, "Things that will
bite."

**Scope note.** This finding surfaced while testing an external
`mv_injector --unload` path (tear down an already-injected, already-running
process from outside it, without closing the game). That capability was
root-caused, fixed, and verified live in a later phase — a stacked
`LoadLibrary` reference count from double-injecting the same path, not a race
- but it was never something the brief asked for (the brief's teardown-adjacent
requirement is about read/mutate safety and detecting visual impact on the
player, not a clean-unload mechanism), and it was removed in a later pass to
keep the submission scoped to the brief. `MvPrepareUnload` and the harness's
`FreeLibrary` call were cut in that same pass, for the same reason: a game
never unloads the hook either, so neither the external tool nor an in-process
`FreeLibrary` teardown was ever something the brief needed. `MvFlushCapture`
(drain the writer queue, leave the module loaded - see dllmain.cpp) replaced
it, and is what the harness actually exercises now.

## The overlay shows a stale velocity frame whenever the velocity pass idles

Reported from live play: with the overlay in "SceneVelocity over gameplay"
mode, rotating the view desynchronises the overlay from the scene — the
coloured silhouettes stay put while the game keeps animating, so trees and the
gun drift out of alignment. Facing forward it looks correct; past a certain
rotation it does not.

### First: the data is not the problem

Before touching the overlay, the same question was asked of a real capture,
where velocity and colour are taken under one fence and can be compared
offline. `skyrunner_video_final`, the frame with the most written pixels
(`vel_00162.bin`, 23.7% written), with the written mask resampled onto the
colour frame exactly the way the overlay shader samples it — `uv` across the
whole velocity texture, point sampled:

```
velocity 1708x1068   colour 1707x1067   ratio 1.00059 x 1.00094
written bbox in velocity space: x 0..1706 of 0..1707, y 0..1066 of 0..1067
```

The mask lands on the trees, the grass and the gun **pixel for pixel** — every
branch, every blade. So the decode, the copy point (the
`RENDER_TARGET -> shader-resource` barrier), and the full-texture UV mapping
are all correct. Worth recording the one real mapping error this turned up,
which is far too small to be the reported symptom: UE renders a 1707x1067
sub-rect into a 1708x1068 allocation, and the shader maps the whole allocation
across the screen, so the overlay is off by one pixel in each axis.

### The mechanism: no copy this frame, but a composite anyway

`OverlayCaptureVelocity` (overlay.cpp) runs only from the
`RENDER_TARGET -> shader-resource` barrier on the identified resource.
`OverlayOnPresent` composites `g_velocityTex` **unconditionally, every
present**, and nothing records which frame that texture's contents came from.
So on any frame where that barrier does not occur, the overlay draws the last
copy — however old — over the current back buffer. There is no staleness
check, no frame stamp, and nothing on screen says the data is not current.

That is the whole defect, and it does not depend on knowing why the barrier
stops.

### The barrier does stop, constantly, and the existing explanation is wrong

`identify:` already reports this, 13 times in one session, in stretches of
300-500 presented frames:

```
identify: the velocity pass has STOPPED. The identified resource has not taken a
RENDER_TARGET -> shader-resource edge for 426 presented frames (last on frame
12196, now at 12622), while the game carries on presenting. That is what a menu,
a pause, or the end of a run looks like from in here - the scene is no longer
being drawn.
```

The candidates log confirms the resource goes *completely* dead — not a changed
barrier pattern, no barriers at all (control: a known-good second has 323
records):

```
ts 1786809692 : 160 records
ts 1786809693 : 252 records
ts 1786809694 : 0        <- silence begins
ts 1786809695 : 0
...through 1786809700 : 0
```

But the log line's stated diagnosis — *"the scene is no longer being drawn"* —
is contradicted by the hook's own heartbeat counters. Per-frame GPU work across
that window, from the same session:

```
frames   barriers/frame   ECL/frame
 12000        421.4          15.0
 12300        421.4          15.0   <- inside the silent window
 12600        421.5          15.0   <- inside the silent window
 12900        421.5          15.0
```

Flat to within 0.1%. A menu or a pause collapses that number; a full scene
render does not. **The scene was still being drawn at an identical work rate
while the velocity resource received nothing.** The pass resumes later on the
same resource (`identify: ... has RESUMED ... same resource`), so this is not a
reallocation either.

One hypothesis was raised and **killed immediately**, which is worth recording
so it does not get re-proposed: that UE skips the pass on frames whose visible
set contains nothing with non-camera motion (velocity is only written for such
pixels; camera-only motion is reconstructed from depth). That would have tied
the silence to view direction, matching the "facing forward it's mostly okay
until I rotate past a certain point" report. It is wrong: the screenshot that
came with the report has WPO trees filling half the frame while the overlay is
desynchronised. Whatever stops the pass, it is not an absence of things to
write.

A second attempt — measuring copy-edge coverage per frame out of
`mv_candidates.log` — produced numbers that looked damning (87% / 43% / 26% /
16% coverage in four different 1000-frame windows, gaps of 5742 and 4846
frames) and **should not be trusted**. That log is written through the same
batched, flush-every-500-lines path that exists because unbatched logging once
crashed the game, at ~5 records per frame; the wildly varying "coverage" is far
more consistent with dropped log lines than with a game that renders velocity
on 16% of its frames. Independent contradiction: gaps of 5742 and 4846 frames
would each have tripped the 300-frame silence detector, and only one
STOPPED/RESUMED pair was logged in that session. `mv_candidates.log` cannot
answer this question; do not re-derive coverage from it without first
establishing whether it drops records.

So the trigger is **not established**. What is established is the 426-frame
outage above — logged by the hook itself, independently visible as zero
candidate records across seven consecutive seconds, and concurrent with an
unchanged scene-rendering work rate.

### Confirmed, and the stalls were hiding under the detector's threshold

The fix went in (frame-stamp the copy, withhold it when stale, draw a red
border so the overlay is visibly declining rather than apparently frozen) along
with a freshness counter that counts in the same place the draw decision is
made. Reproduced live: **the border appears exactly where the desynchronisation
used to be.** The overlay was drawing stale velocity over live frames, and that
is the whole of the reported bug.

The counter then answered the question the logs could not:

```
overlay: velocity freshness over 300 presents -  82 drawn, 218 withheld, longest stale run 217
overlay: velocity freshness over 300 presents - 121 drawn, 179 withheld, longest stale run 272
overlay: velocity freshness over 300 presents - 300 drawn,   0 withheld, longest stale run   0
overlay: velocity freshness over 300 presents - 153 drawn, 147 withheld, longest stale run  82
overlay: velocity freshness over 300 presents -  72 drawn, 228 withheld, longest stale run 157
overlay: velocity freshness over 300 presents - 221 drawn,  79 withheld, longest stale run  84
```

Two things fall out of that. First, it alternates: clean 300/300 windows sit
between windows that are up to 76% stale, so this is a recurring state the game
enters and leaves, not a one-off. Second, and the reason none of it was ever
visible: **every stale run is shorter than 300 frames**, and
`kQuietFramesBeforeSilent` is 300. The silence detector reported nothing during
this entire reproduction - its last `STOPPED` was over 1,700 seconds earlier.
Stalls of 82-272 frames are 1-3 seconds at this frame rate, exactly the "it
freezes, then catches up" the report described, and the detector was built to
ignore them. The 426-frame outage documented above was not a special event; it
was one instance of this that happened to run long enough to trip the
threshold.

### The likely mechanism, and the line that points at it

A log line that appeared in the same session is the strongest lead:

```
resource-tracking: identification budget of 8000 distinct resources is SPENT. No
resource created from here on is examined, so a velocity or depth texture the
engine allocates now can never be found ... what is lost is the ability to notice
a REPLACEMENT.
```

Together with the identification histogram, which found **two** velocity-shaped
render-target populations rather than one -
`1708x1068=30 ... 1707x1067=6` - the hypothesis is that the engine has more
than one velocity target and moves between them (dynamic resolution is the
obvious candidate: raising GPU load by whipping the camera around is precisely
what makes DRS change resolution, and "rotate past a certain point" is how the
bug was first described). The hook identified one of them, cannot see the other,
and cannot even notice the substitution now that the tracking budget is spent.
The pass "resuming on the same resource" fits: the engine switches back.

### Caught in the act - and the size hypothesis was wrong

A diagnostic was added for exactly this: on any `RENDER_TARGET ->
shader-resource` edge from a resource that is *not* the identified one, if the
identified one has been stalled three or more frames, compare formats and log
it. It reads the barrier directly rather than going through resource tracking,
because tracking is what the spent budget blinds.

It fired immediately, and hit its 40-line cap:

```
identify: RIVAL velocity-format edge while the identified resource is stalled -
ptr=1912348214624 1708x1068 fmt=11 took RENDER_TARGET -> shader-resource on
frame 2090, 3 frames into the stall. The identified resource is 1708x1068.
```

**The rival is the same extent and the same format — 1708x1068 fmt=11 — at a
different address.** So the dynamic-resolution guess above is wrong: this is not
the engine switching to a differently-sized target. It is a straight
**reallocation of SceneVelocity to a new address with a byte-identical
descriptor**, which is a considerably worse case, because the hook's only
trigger for reopening the search is `ValidateCandidate` detecting that the
selected resource's *descriptor* changed. An identical-descriptor replacement
cannot trip that check, by construction. The old pointer simply stops being
used, forever, and nothing notices.

This time it did not come back. The freshness counter shows the overlay never
recovering:

```
0 drawn, 300 withheld, longest stale run 1503 frames, max copy age 1504
0 drawn, 300 withheld, longest stale run 1803 frames, max copy age 1804
0 drawn, 300 withheld, longest stale run 2403 frames, max copy age 2404
0 drawn, 300 withheld, longest stale run 3003 frames, max copy age 3004
```

Monotonic, no recovery. So the earlier episodes that *did* resume on the same
resource were a milder, separate phenomenon; a replacement is terminal. The
whole chain:

1. UE reallocates SceneVelocity to a new address, identical descriptor.
2. Nothing reopens the search: the descriptor check cannot see it, and
   `resource-tracking` stopped examining new resources when its 8000-entry
   budget was spent, so the replacement was never a candidate anyway.
3. `VelocityPassIsSilent` notices the quiet and **explicitly declines to
   reidentify** - correct for the menu case it was written for, wrong here.
4. The overlay is permanently stale (now visibly withheld rather than
   silently misaligned) and F8 captures return nothing at all.

### That fix was wrong too, and the reason is the actual root cause

The obvious next step - promote the diagnostic to a trigger, reopen the search
when a rival holds the edge - was built, gated on eight consecutive frames, and
rate-limited. It made things **worse**, visibly, within two seconds of play:

```
identify: velocity target REPLACED ... on 8 consecutive frames
identify: REOPENING the search - ...
identify: SELECTED 2089945739632 1708x1068 fmt=11 | score=13 | survivors=1
identify: velocity target REPLACED ... on 8 consecutive frames
identify: REOPENING the search - ...
identify: AMBIGUOUS - the top two survivors score 13 and 13 ...
```

Reopen, re-find, immediately lose it again, land in ambiguity. Two further
observations finished the diagnosis. First, in the state where the overlay
never recovered at all, **no rival was reported** - nothing of the identified
format was taking the edge. Second, that state was reproducible by facing a
particular direction.

The answer was already written down in this repo, in the comment above
`kVelocityFormats` in `velocity_identify.cpp`:

```
//     if (IsOpenGLPlatform(ShaderPlatform))
//         return bNeedVelocityDepth ? PF_R16G16B16A16_UINT : PF_R16G16_UINT;
//     else
//         return bNeedVelocityDepth ? PF_A16B16G16R16      : PF_G16R16;
//
// bNeedVelocityDepth is NeedVelocityDepth() (VelocityRendering.cpp:120):
// distance fields + Lumen GI, or ray tracing. It is a project/platform property
// we cannot read from outside the process, so BOTH widths stay in the filter.
```

`PF_A16B16G16R16` is `DXGI_FORMAT_R16G16B16A16_UNORM` (fmt=11, four channels);
`PF_G16R16` is `DXGI_FORMAT_R16G16_UNORM` (fmt=35, two). **SceneVelocity has two
widths, and this title switches between them at runtime, several times a
minute, with view direction** - the log fills with freshly created
`1708x1068 fmt=35` textures during exactly the stalls where no fmt=11 rival
exists.

The sentence *"It is a project/platform property we cannot read from outside
the process"* is the defect. The conclusion drawn from it - keep both widths in
the structural filter - was right. The premise was not: it is not a fixed
property of the build, it is re-decided per view. Identification hedged
correctly at the filter and then latched a single resource of a single width
anyway, so when the engine switched, the tracked pointer was not stalled, not
replaced, just **the wrong width** - and stayed wrong until the engine happened
to switch back.

This is the same shape as the mistake at the top of this file, and as "Three
things that were Oxi facts wearing engine clothes": a property observed to be
constant was recorded as constant, rather than as observed-constant-so-far.

### The fix: track the pass, not the resource

`NoteRivalVelocityEdge` now judges a rival by the **same structural filter
identification uses** rather than by "same format as the one already picked" -
which by construction could never see a width change - plus the same render
extent, since both widths are allocated at the same
`SceneTexturesConfig::Extent`. A texture passing that and holding the edge for
eight consecutive frames while the selected one takes none is **adopted as a
second velocity target**, and the hook captures from whichever takes the edge.
Adoption is bounded at eight, because an adoption mechanism with no limit
cannot be distinguished from one adopting the wrong things.

Reopening is *not* the answer here and the earlier attempt was removed: while
both targets are live there genuinely are two velocity resources, so re-running
the search finds two equally good survivors and correctly refuses - which is why
that version produced `AMBIGUOUS 13 and 13` and a permanently blank overlay.
Nothing was wrong with the scorer. There were simply two right answers, and the
code above it was built to hold one.

### Correction: it works, and the width story was wrong

The fix was then verified live, and the verification corrected the diagnosis
that motivated it. First, that it works, unambiguously - the freshness counter
across the moment of adoption:

```
0 drawn, 300 withheld, longest stale run 17600 frames
299 drawn,   1 withheld, longest stale run 1        <- adoption fires here
282 drawn,  18 withheld, longest stale run 9
300 drawn,   0 withheld, longest stale run 0        <- and stays, 10+ windows
```

A 17,600-frame stale run becomes 100% drawn and stays there, with no reopen and
no `AMBIGUOUS`. The user confirms the red border now appears only briefly at
startup and not afterwards.

But **both adopted targets are `fmt=11`** - the same width as the one already
selected - and across every session logged, **no `fmt=35` texture has ever been
observed taking the velocity edge.** So this is not `NeedVelocityDepth()`
flipping. It is a plain **reallocation of SceneVelocity to a new address with a
byte-identical descriptor**, which is the hypothesis two sections above, the one
abandoned when the reopen fix built on it failed.

That is the mistake worth recording. The reopen attempt failing was taken as
evidence that the *diagnosis* was wrong. It was not: the diagnosis was right and
the *response* was wrong. Reopening lands in `AMBIGUOUS` precisely because both
the orphaned and the new target are structurally valid velocity textures -
which is confirmation of the replacement story, not a refutation of it. Adoption
is the correct response to the same diagnosis.

The `fmt=35` evidence was misread. Freshly created `1708x1068 fmt=35` textures
do appear in the log during stalls, and that was taken as the pass having moved
to the two-channel width. Creation is not use: none of them ever took the
produce-then-consume edge. The inference was drawn from the wrong signal, and a
check that was already available - "does anything of that width take the edge?"
- would have killed it immediately.

What survives: `FVelocityRendering::GetFormat` really does have two widths, the
structural filter really should accept both, and the note calling that "a
project/platform property" really is unverified as stated. But nothing here
demonstrates this title switching between them, and the sections above should be
read with that correction in mind. The adoption fix covers a width switch by
construction, since it judges rivals with the full structural filter rather than
by matching the selected format - it simply has not been observed doing so.

### What to fix

The overlay must not present data it did not refresh. Stamp `g_velocityTex`
with the present-frame index at each copy, and in `OverlayOnPresent` compare it
against the current frame: if it is stale, either composite nothing (show the
scene untouched) or mark it visibly, rather than drawing an old frame as
though it were this one. The one-pixel sub-rect mapping error above is worth
correcting in the same pass, by scaling the sampled UV by the written extent
rather than the allocation.

The `velocity pass has STOPPED` message should also stop asserting that the
scene is not being drawn. It has the counters on hand to tell the two apart:
if per-frame barrier and ExecuteCommandLists rates are unchanged, the scene is
still being drawn and the correct statement is "the velocity pass specifically
is idle", which is a different fact with different consequences.

## Leaving fullscreen abandons SceneVelocity too, and the first fix was silent

Reported separately from the reallocation bug above, but the same shape: a
swapchain resize (leaving fullscreen, or anything else that changes
back-buffer size) makes UE rebuild its scene textures at a new
`SceneTexturesConfig::Extent`, abandoning the selected velocity resource. The
descriptor check cannot see it - the old texture's own descriptor never
changes - and adoption cannot cover it either, because `NoteRivalVelocityEdge`
deliberately requires the same render extent as the resource it might
replace, which is exactly the one thing a resize changes.

### First attempt: silent, because a comment was still true when read

`NoteBackBufferSize` was made to return `true` on a size change, and the
Present hook reopened both searches on that signal. It did not work, and the
log gave no clue why - no `back buffer resized` line at all, ever.

The cause was `NoteBackBufferSizeIfNeeded` itself:

```cpp
// Read only until identification has an answer - GetDesc on the swapchain is
// cheap but not free, and after that nothing uses it.
void NoteBackBufferSizeIfNeeded(IDXGISwapChain* swapChain) {
    if (mv::IdentifiedVelocityResource() != nullptr || swapChain == nullptr) {
        return;
    }
    ...
```

`"after that nothing uses it"` was true the day it was written and became
false the moment a resize check was added downstream of it: the function
stops calling `GetDesc`/`NoteBackBufferSize` entirely once identification
succeeds, which is precisely the state the game is in almost all the time.
Not reading the back buffer size is indistinguishable, from the log, from
nothing having happened - it cost a full round of "the fix does not work"
before the early return itself became the suspect.

### Fix: poll instead of stop

Read every frame until identification has an answer, since the search itself
needs it; polled every 30 frames after, since the resize check does. A resize
is a human-scale event, so a few checks a second is plenty, and it keeps the
original cost argument (`GetDesc` is cheap but not free) intact rather than
discarding it.

### Verified live

Built, injected into the running game, overlay on, fullscreen toggled
repeatedly - including a burst of rapid resizes from window-edge snapping.
Filtering the log to this injection's window (from its `InitThread running`
line onward) gives exact counts, not estimates:

```
identify: back buffer resized : 6
identify: REOPENING           : 6
depth:    REOPENING           : 6
identify: SELECTED            : 7   (1 at injection + 6 re-selections)
depth:    SELECTED            : 6
identify: AMBIGUOUS           : 0
depth:    AMBIGUOUS           : 0
```

Every one of the 6 resize events reopened both searches and reached a new
`SELECTED` within roughly a second, with no `AMBIGUOUS` at any point in the
run. The rapid window-snap burst - 18 separate resize events on the overlay's
own (unrelated, every-frame) resize check in under two seconds - produced
exactly one `identify: back buffer resized`, because the 30-frame poll
samples the settled state rather than chasing every transient; it caught up
once the resizing stopped rather than thrashing through it. Freshness
recovered to `300 drawn, 0 withheld` within a window or two of every toggle
and held there once toggling stopped.

The log's other 21 `AMBIGUOUS` lines all predate this injection - they are
the reopen-on-rival dead end recorded above under "That fix was wrong too",
not a recurrence of it here.

This is the same diagnosis as the reallocation case, confirmed rather than
revised: what failed on the first attempt was an implementation bug in the
trigger plumbing (a stale early return), not the idea that a resize abandons
the resource. Unlike the dual-width story earlier in this file, there was
never a wrong turn in what caused the bug - only in getting the fix to
actually run.
