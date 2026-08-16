"""Render the captured motion field as a video alongside the gameplay.

Two modes, chosen by what the dump contains rather than by a flag.

SPARSE (any dump). Three panels: the game's own back buffer, the extracted
SceneVelocity field on its own, and the field composited over the frame.
Panel 2 is deliberately shown raw rather than only as an overlay. SceneVelocity
holds velocity for *dynamic and world-position-offset geometry only* - 11-13% of
pixels in the station capture (the train, background characters and signage, and
the weapon viewmodel), 7-9% in the corridor capture where the viewmodel is the
only thing moving, ~21% in the Skyrunner flight captures. Static-geometry motion
is reconstructed from camera matrices inside the consuming TSR/motion-blur
shader and is genuinely not in this buffer, so a mostly-black panel is the
honest picture of its contents rather than a bug in the visualisation.

DENSE (a dump that also carries scene depth and the View uniform buffer).
Three panels: the game frame, SceneVelocity as the engine wrote it, and the
FULL-SCREEN field - the same buffer with every pixel the engine skipped filled
in by reprojecting its depth through View_ClipToPrevClip. That reconstruction is
not an approximation of the engine's: it is a transcription of
ComputeStaticVelocity from TSRDepthVelocityAnalysis.ush, the function TSR itself
uses for exactly those pixels, and reproject.py measures it against the decoded
buffer at slope 1.000 per pixel on static geometry. The dense panel is what a
consumer of this data actually wants; the sparse one stays beside it because the
difference between them is the whole point.

The hue-for-direction colouring here is the optical-flow (Middlebury)
convention, imported by us. Unreal draws nothing like it, so this view has no
engine ground truth to be checked against - the in-game overlay deliberately
shows the raw channel mapping instead. See DEBUGGING.md.

Usage:  python make_video.py <dump_dir> [out.mp4] [--preview] [--still N]
        [--sparse]              force the sparse rendering on a dump that could
                                do better, for comparison
        [--still N]             write one PNG of frame N (or the middle frame if
                                N is omitted) instead of a video - this is what
                                produces the README's lead figure
"""
import os
import sys

import cv2
import numpy as np

import mvtools
from reproject import compute_static_velocity, screen_pos_grid

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]

dump_dir = args[0] if args else os.path.join(os.environ["TEMP"], "mv_dump")
out_path = args[1] if len(args) > 1 else "motion_field.mp4"
preview_only = "--preview" in flags
force_sparse = "--sparse" in flags
still_index = None
still_only = "--still" in sys.argv
if still_only:
    i = sys.argv.index("--still")
    if i + 1 < len(sys.argv) and not sys.argv[i + 1].startswith("--"):
        still_index = int(sys.argv[i + 1])
        if str(still_index) in args:
            args.remove(str(still_index))

meta = mvtools.load_meta(dump_dir)
indices = mvtools.frame_indices(dump_dir)
cw, ch = meta["color_width"], meta["color_height"]
vw, vh = meta["velocity_width"], meta["velocity_height"]

scale = 0.36
pw, ph = int(cw * scale), int(ch * scale)


def dense_available():
    """Can this dump support the reconstruction, or only the sparse buffer?

    Both halves are required and they fail for different reasons, so they are
    reported separately: a dump can have depth and no constant buffer (the View
    buffer was never bound as a root CBV) or a constant buffer and no depth
    (identification never found a depth target). Neither is an error in the
    dump; both mean this falls back rather than producing a field with a hole
    in it.
    """
    if force_sparse:
        return False, "--sparse was passed"
    if not mvtools.has_depth(meta):
        return False, "no meta_depth.txt - this capture has no scene depth in it"
    if mvtools.load_view_cb(dump_dir, indices[0], meta) is None:
        return False, "no viewcb_*.bin - this capture has no View uniform buffer in it"
    return True, ""


def layout_for(index):
    """View_ClipToPrevClip for one frame, derived from that frame's own bytes.

    Per frame and not once for the dump, for the same reason reproject.py does
    it per frame: the offset is a property of the build, but which captured
    candidate buffer is the View buffer is a property of the frame, and pinning
    either would turn a derivation into a constant. Returns None if this frame's
    candidates contain nothing that looks like the View buffer.
    """
    buffers = mvtools.load_view_cb(dump_dir, index, meta)
    if buffers is None:
        return None
    for slot in range(buffers.shape[0]):
        found = mvtools.derive_view_layout(buffers[slot], (vw, vh))
        if found is not None and found["checks_passed"] >= 3:
            return found
    return None


def colour_wheel(size):
    """Legend: hue = direction of motion, brightness = magnitude."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    cx = cy = (size - 1) / 2.0
    dx, dy = xx - cx, yy - cy
    r = np.sqrt(dx * dx + dy * dy) / cx
    ang = np.arctan2(dy, dx)
    hsv = np.zeros((size, size, 3), np.float32)
    hsv[:, :, 0] = (ang + np.pi) / (2 * np.pi) * 179.0
    hsv[:, :, 1] = 255.0
    hsv[:, :, 2] = np.clip(r, 0, 1) * 255.0
    bgr = cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)
    bgr[r > 1] = 0
    return bgr


def label(img, text, colour=(255, 255, 255)):
    cv2.rectangle(img, (0, 0), (img.shape[1], 30), (0, 0, 0), -1)
    cv2.putText(img, text, (10, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.58, colour, 2)
    return img


def caption_bar(width, text):
    bar = np.zeros((26, width, 3), np.uint8)
    cv2.putText(bar, text, (10, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (185, 185, 185), 1)
    return bar


dense, why_not = dense_available()
print(f"dump:   {dump_dir}  ({len(indices)} frames)")
print(f"mode:   {'DENSE - buffer plus reconstruction' if dense else 'SPARSE - buffer only'}"
      f"{'' if dense else '  (' + why_not + ')'}")

layouts = {}
if dense:
    for index in indices:
        found = layout_for(index)
        if found is not None:
            layouts[index] = found
    if not layouts:
        dense = False
        why_not = "no frame's captured constant buffers contained the View uniform buffer"
        print(f"mode:   falling back to SPARSE - {why_not}")
    else:
        offsets = sorted({v["clip_to_prev_clip_offset"] for v in layouts.values()})
        print(f"        View_ClipToPrevClip derived on {len(layouts)}/{len(indices)} frames, "
              f"at byte offset(s) {offsets} - re-derived per frame, never assumed")


def fields_for(index):
    """(sparse_uv, dense_uv, written_mask, dense_mask) for one frame, in UV units.

    dense_uv is sparse_uv where the engine wrote, and the reprojection
    everywhere else. On a frame whose View buffer could not be derived it is the
    sparse field again, so the clip degrades to "no reconstruction here" rather
    than to a frame of garbage.

    dense_mask says which pixels of dense_uv are real. It is not the whole
    frame: the sky carries no depth to reproject, so those pixels are data
    neither way and are drawn black rather than at the colour wheel's brightness
    floor, which would render "nothing is known here" in the same vocabulary as
    a measured zero.
    """
    vel_path = os.path.join(dump_dir, f"vel_{index:05d}.bin")
    flow = mvtools.decode_velocity(vel_path, meta)
    written = mvtools.velocity_written_mask(vel_path, meta)
    sparse_uv = flow.copy()
    sparse_uv[~written] = 0.0
    if not dense or index not in layouts:
        return sparse_uv, sparse_uv, written, written

    layout = layouts[index]
    depth_path = os.path.join(dump_dir, f"depth_{index:05d}.bin")
    if not os.path.exists(depth_path):
        return sparse_uv, sparse_uv, written, written
    # Depth and velocity are created at the same SceneTexturesConfig::Extent but
    # the readbacks are sized from their own footprints, so compare over the
    # overlap rather than assuming they match to the pixel.
    oh = min(vh, meta["depth_plane0_height"])
    ow = min(vw, meta["depth_plane0_width"])
    device_z = mvtools.load_depth(depth_path, meta)[:oh, :ow]
    sx, sy = screen_pos_grid(ow, oh, layout["view_rect_min"], layout["view_size"])
    ax, ay = compute_static_velocity(sx, sy, device_z, layout["clip_to_prev_clip"])

    # DeviceZ == 0 is the far plane in UE5's reversed-Z buffer: sky, or anything
    # the depth pass never wrote. Reprojecting it divides by a w carrying no
    # geometry, so those pixels stay at zero rather than being filled with a
    # number that came from nowhere - the sky genuinely has no depth to
    # reproject, and painting it would be inventing data.
    valid = np.isfinite(ax) & np.isfinite(ay) & (device_z > 1e-7)
    dense_uv = sparse_uv.copy()
    fill = (~written[:oh, :ow]) & valid
    dense_uv[:oh, :ow, 0][fill] = (-0.5 * ax)[fill]
    dense_uv[:oh, :ow, 1][fill] = (+0.5 * ay)[fill]
    dense_mask = written.copy()
    dense_mask[:oh, :ow] |= fill
    return sparse_uv, dense_uv, written, dense_mask


# One magnitude scale for the whole clip, so brightness is comparable frame to
# frame rather than silently renormalising per frame - but derived from this
# capture instead of hardcoded, because the two Skyrunner dumps differ by more
# than an order of magnitude in displacement (5-90 px/frame against ~144) and a
# constant tuned for one renders the other as a flat white or a flat black.
probe = indices[::max(1, len(indices) // 6)][:6]
mags = []
for index in probe:
    _, dense_uv, _, dense_mask = fields_for(index)
    px = dense_uv[:, :, 0] * vw
    py = dense_uv[:, :, 1] * vh
    m = np.sqrt(px * px + py * py)
    mags.append(np.percentile(m[dense_mask], 99) if dense_mask.any() else 0.0)
MAX_PIXELS = float(max(np.median(mags), 1e-3))
print(f"scale:  brightness saturates at {MAX_PIXELS:.1f} px/frame "
      f"(p99 over {len(probe)} probe frames, fixed for the whole clip)")

wheel = colour_wheel(max(46, ph // 7))

if dense:
    panel_titles = ("game frame", "SceneVelocity, as the engine wrote it", "full-screen field")
    caption = ("right panel: pixels SceneVelocity never wrote are reconstructed from scene depth and "
               "View_ClipToPrevClip - the engine's own ComputeStaticVelocity")
else:
    panel_titles = ("game frame", "extracted motion field", "composited")
    caption = ("SceneVelocity holds dynamic and world-position-offset geometry only; static-geometry "
               "motion is reconstructed by the consuming shader and is not in this buffer")


def render(index):
    color = mvtools.load_color(os.path.join(dump_dir, f"color_{index:05d}.bin"), meta)
    sparse_uv, dense_uv, written, dense_mask = fields_for(index)
    bgr = (mvtools.to_display(color)[:, :, ::-1] * 255).astype(np.uint8)

    sparse_rgb = mvtools.flow_to_rgb(sparse_uv, meta, max_pixels=MAX_PIXELS)
    # Force unwritten pixels to actual black rather than leaving them at
    # flow_to_rgb's brightness floor. The floor exists so slow-moving written
    # pixels do not vanish; applied to unwritten ones it paints "no data" in the
    # same colour vocabulary as data, and which pixels the engine wrote is
    # exactly what this panel is for.
    sparse_rgb[~written] = 0

    # Velocity is at render resolution (DRS); the back buffer is at output
    # resolution - resize up before compositing or laying out.
    sparse_full = cv2.resize(sparse_rgb, (cw, ch), interpolation=cv2.INTER_NEAREST)
    if dense:
        dense_rgb = mvtools.flow_to_rgb(dense_uv, meta, max_pixels=MAX_PIXELS)
        dense_rgb[~dense_mask] = 0
        third = cv2.resize(dense_rgb, (cw, ch), interpolation=cv2.INTER_NEAREST)
    else:
        mask_full = cv2.resize(written.astype(np.uint8), (cw, ch),
                               interpolation=cv2.INTER_NEAREST) > 0
        third = bgr.copy()
        third[mask_full] = sparse_full[mask_full]

    coverage = written.mean() * 100.0
    p1 = label(cv2.resize(bgr, (pw, ph)), panel_titles[0])
    p2 = label(cv2.resize(sparse_full, (pw, ph)), f"{panel_titles[1]} ({coverage:.1f}% written)")
    third_title = panel_titles[2]
    if dense:
        # The reconstructed fraction is measured, not 100 minus coverage: the
        # sky has no depth to reproject and is filled by neither source.
        third_title += f" ({(dense_mask & ~written).mean() * 100.0:.1f}% reconstructed)"
        if index not in layouts:
            third_title = "full-screen field (UNAVAILABLE on this frame)"
    p3 = label(cv2.resize(third, (pw, ph)), third_title)

    # Drop the legend into the panel that carries the most field.
    target = p3 if dense else p2
    s = wheel.shape[0]
    y0, x0 = ph - s - 8, pw - s - 8
    target[y0:y0 + s, x0:x0 + s] = np.maximum(target[y0:y0 + s, x0:x0 + s], wheel)
    cv2.putText(target, "dir", (x0, y0 - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (200, 200, 200), 1)

    frame = np.hstack([p1, p2, p3])
    # Caption on its own bar so it stays readable over bright scene content.
    return np.vstack([frame, caption_bar(frame.shape[1], caption)])


if still_only:
    index = still_index if still_index is not None else indices[len(indices) // 2]
    if index not in indices:
        print(f"frame {index} is not in this dump; frames are {indices[0]}..{indices[-1]}")
        sys.exit(1)
    still_path = out_path if out_path.lower().endswith(".png") else "motion_field_still.png"
    cv2.imwrite(still_path, render(index))
    print(f"wrote {still_path} (frame {index})")
    sys.exit(0)

if preview_only:
    for index in indices[:2]:
        cv2.imwrite(f"preview_{index:05d}.png", render(index))
    print("wrote preview PNGs")
    sys.exit(0)

writer = cv2.VideoWriter(out_path, cv2.VideoWriter_fourcc(*"mp4v"), 30, (pw * 3, ph + 26))
for index in indices:
    writer.write(render(index))
writer.release()
missing = [i for i in indices if dense and i not in layouts]
print(f"wrote {out_path} ({len(indices)} frames)")
if missing:
    print(f"  {len(missing)} frame(s) had no derivable View buffer and show the sparse field "
          f"in the right-hand panel, labelled as such: {missing[:8]}")
