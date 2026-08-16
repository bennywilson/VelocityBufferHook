# VelocityBufferTest

Injects a DLL into a running Unreal Engine 5 game, hooks D3D12 via vtable
patching, and locates the engine's `SceneVelocity` (motion vector) buffer.
The target is packaged **Shipping**, which strips resource names, so 
identification has to work from structure and behaviour alone. Once found,
the buffer is copied back to the CPU without stalling the game and decoded
into a per-pixel motion field, which is then checked against independent ground truth rather than just visualised and trusted.

Development ran against two targets: "Oxi", a UE5.7.1 project of my own
and "Skyrunner", a third-party UE5.2 title I do not have source code access to.
The second run is the one that matters most and it falsified several assumptions
the Oxi work made about SceneVelocity.

Full detail is kept in `DEBUGGING.md`.

## Build and run

```
cmake -B build -S .
cmake --build build --config Release --target mv_hook mv_injector mv_testhost
```

One command launches the game and injects, handling the startup ordering:
- env vars must be set before launch to be inherited
- must wait for the real `*-Win64-Shipping` child rather than a launcher stub,
- must not inject on top of an already-loaded copy
- etc

```powershell
.\Invoke-MvCapture.ps1 "D:\Games\Skyrunner\Valfreyja.exe"
.\Invoke-MvCapture.ps1 "D:\Games\Skyrunner\Valfreyja.exe" -DumpDir D:\mv_captures\run7
```

Useful flags: `-DryRun` (do everything except inject), `-DumpDir`,
`-EngineVersion` (inferred for the titles in this repo, required otherwise —
5.2 and 5.7 pack channels 2/3 differently), `-AttachOnly` (inject into an
already-running process).

Key environment variables, if driving it by hand instead:

| variable | effect |
|---|---|
| `MV_DUMP_DIR` | where captures are written |
| `MV_ENGINE_VERSION` | e.g. `5.2` — needed by the offline decode for channels 2/3 |
| `MV_IDENTIFY_FRAMES` | frames to watch barriers before deciding (default 90) |
| `MV_CAPTURE_DEPTH=0` | disable the depth copy for a session |

With the game running, **F8** captures a 60-frame burst to `%TEMP%\mv_dump\`
(or `MV_DUMP_DIR`). Wait for `capture: write queue drained` in
`%TEMP%\mv_hook.log` before quitting — `capture: burst recorded` does not mean
flushed. Then run the offline validation:

```
cd tools
python run_validation.py %TEMP%\mv_dump <title>   # everything -> results/validation_<title>.txt
python make_video.py %TEMP%\mv_dump motion_field.mp4
python barrier_signature.py                        # needs a >=2000-frame session
```

To exercise the capture path under the D3D12 debug layer + GPU-based
validation instead of a real game (requires the Windows **Graphics Tools**
optional feature):

```
set MV_AUTOCAPTURE=1
set MV_DUMP_DIR=%TEMP%\mv_dump_testhost
build\testhost\Release\mv_testhost.exe --hook <abs path>\build\hook\Release\mv_hook.dll --frames 240
```

Practical notes: inject into `*-Win64-Shipping.exe`, not the launcher stub;
wait ~5s after process start before injecting; restart the game rather than
re-injecting a rebuilt DLL over a loaded one; capture the shipping build in
RenderDoc, not PIE/editor (resolutions differ between them).

## How it works

**Identification.** Neither a pure descriptor match nor pure runtime behaviour
transfers between titles, so the two are used as separate tools. Structure is
a hard gate derived from engine rules rather than a copied tuple: the formats
`FVelocityRendering::GetFormat` can return, the create flags
`GetCreateFlags` implies, `mips=1/arraySize=1/samples=1/layout=UNKNOWN`, and a
resolution ranked (not matched) from an observed histogram, bounded above by
the back buffer. Behaviour — barrier transition patterns over a settling
window — is scored evidence layered on top, logged per candidate with a reason
for the winner. On Oxi, behaviour was decisive and format broke a tie; on
Skyrunner, structure alone was already unique and most of the "invariants"
measured on Oxi (exactly 2 barrier events/frame, never entering
`UNORDERED_ACCESS`, destination state `ALL_SHADER_RESOURCE`) turned out false
of Skyrunner's own velocity buffer. Where the ranking can't separate
candidates by a clear margin, the hook refuses to capture rather than
guessing.

**Extraction.** The buffer is a *transient placed* resource whose heap memory
gets aliased by other resources later in the frame, so it can only be safely
read at the `RENDER_TARGET -> shader-resource` barrier — after the last
render-target write (confirmed against RenderDoc's own event ordering) and
before the memory is reused. The copy is recorded on the game's own command
list; a 4-slot async ring means the CPU never waits on the GPU (a full ring
skips the frame rather than stalling); a single fence, checked at runtime by
command-list identity rather than assumed, covers the copy; and all disk I/O
happens on a background writer thread with a bounded, drop-and-log queue.
Subresource/plane counts and footprints are read from the resource desc every
frame rather than cached, so a swapchain resize or a byte-identical
reallocation to a new address (both observed live) don't silently invalidate
the readback.

**Units and precision.** The single biggest correctness bug in the project was
treating the encoding as linear. `Common.ush` applies
`sign(V) * sqrt(abs(V))` before packing (`VELOCITY_ENCODE_GAMMA`, unconditional
on SM5+ D3D12), so a linear decode is wrong everywhere and worst near zero,
where the curve is steepest. This was found by reading the engine source
(and later, on Skyrunner where no matching source tree exists locally,
by reading the shipping binary's own disassembled pixel shader — the encode
is legible directly in the compiled HLSL) rather than by fitting constants to
captured data. A prior attempt to fit constants against a single capture
produced numbers that worked on that one clip and were physically meaningless
elsewhere. Channels 2/3 turned out to be the two halves of one packed float32
(`V.z`, the DeviceZ delta) rather than two independent channels — viewed
separately, one half looks bimodal (it's the sign bit) and the other looks
like noise, which is why it took months to read as a single value.

**Validation** runs several independent tests, each weaker or stronger for
different reasons:
- *Warp-and-difference*: warp frame N-1 by the decoded field, score against
  frame N, with three controls (sign-flipped field, best rigid global shift,
  mismatched frame) that must rank worse than the real field. This only means
  something on footage with several pixels/frame of true motion — on
  near-static Oxi captures it correctly reports that no warp beats not
  warping, which says nothing about the decode; on a moderate-speed Skyrunner
  capture it passes at 102/103 pairs improved, beating the best rigid shift by
  60%.
- *Block matching* (pyramidal, sub-pixel) as an independent, non-buffer ground
  truth for per-pixel and per-region comparison against the decode.
- *Analytical reprojection*, the strongest reference because it never looks at
  the image at all: for static geometry, the previous frame's screen position
  follows purely from current depth and `View_ClipToPrevClip`, which is a
  transcription of the engine's own `ComputeStaticVelocity`. The hook reads
  the View uniform buffer out of the process by GPU virtual address (no
  resource pointer), and the byte offset of `View_ClipToPrevClip` is derived
  per-dump from a reciprocal-`float4` anchor plus `SceneView.h`'s member
  ordering, never hardcoded. This measured slope 1.0000/0.9997 (r =
  1.0000/0.9998) against the decode over 18M pixels on Skyrunner, and is also
  what fills in the full-screen field (~57% of a frame reconstructed from
  depth, ~23% from the buffer itself, the far plane left as no-data rather
  than a fabricated number).

**Debugging ethic.** Two rules run through the whole log: re-derive constants
from primary sources instead of trusting recalled or fitted values, and leave
wrong turns in the record instead of editing them out once the right answer is
known. Two concrete cases:
- The velocity buffer's format was transcribed early on as
  `R16G16B16A16_FLOAT` (DXGI enum 10) instead of the actual `_UNORM` (11). The
  wrong filter didn't fail loudly — it matched a real pool of 4 HDR render
  targets that cycled states exactly like a velocity buffer would, so three
  separate rounds of otherwise-correct analysis ran against the wrong
  candidate set before anyone re-checked the constant against `dxgiformat.h`.
  The tell, in hindsight, was that all 4 survivors were descriptor-identical —
  the signature of a pool, not a specific resource.
- Porting to Skyrunner falsified properties the Oxi barrier survey had
  written up as facts about `SceneVelocity` itself: Oxi's velocity buffer
  showed exactly 2 barrier events/frame with zero exceptions over 53,324
  frames and never entered `UNORDERED_ACCESS`; Skyrunner's shows 5
  events/frame, transitions to `NON_PIXEL_SHADER_RESOURCE` instead of
  `ALL_SHADER_RESOURCE`, and does enter `UNORDERED_ACCESS`. Every one of those,
  kept as a hard rule, would reject the correct buffer on the other title —
  which is why identification now treats structure as the only hard gate and
  behaviour as scored evidence rather than a second set of invariants.

**Where AI helped, and where it was overridden.** Useful for well-specified
boilerplate — the D3D12 bootstrap to obtain vtable pointers, readback/fence
plumbing, the ring-buffer/writer-thread structure, Python decode/plot code —
and for bulk log analysis (grouping ~1M barrier events by transition pattern).
Unreliable, and overridden, on anything that was a specific numeric claim
about the engine: recalled `D3D12_RESOURCE_STATES` values that were simply
wrong and caught only by checking `d3d12.h`; a recollection of the velocity
decode constants that quietly omitted the `sqrt` term, discovered by reading
the actual shader source rather than trusting the recollection; and a fence
"coverage" check that compared raw COM interface pointers, which the debug
layer falsified on its first run because it wraps interfaces at a different
address per hook site. The pattern was consistent: reliable for structure and
throughput, unreliable for specific facts, and those are exactly the things
that fail silently and plausibly rather than loudly.
