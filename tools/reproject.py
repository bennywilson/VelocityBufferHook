"""An analytical ground truth for SceneVelocity, and the full-screen field.

WHY THIS EXISTS

Every reference this project has used has been an IMAGE-MATCHING reference -
optical flow, then block matching, then wide-range cross-correlation, then
per-region template matching - and all of them have now failed on this footage
in ways that took real work to detect. Optical flow was wrong for smooth
low-texture geometry. Block matching searched +-14px and, above that, returned
plausible small numbers rather than failing: 1.68px against real 27px motion.
Phase correlation reads ~0 on a forward-flying camera because it measures
translation and radial expansion has no net translation. Template matching works
but degrades badly on thin wind-animated foliage. A residual ~4-5% scale
shortfall is left over that two hypotheses failed to explain.

The common factor is that the reference looks at the image.

This one does not. For a pixel on static geometry, the previous frame's screen
position follows from its depth and View_ClipToPrevClip alone. That is not an
approximation of what the engine does - it IS what the engine does, in
TSRDepthVelocityAnalysis.ush:

    float3 ComputeStaticVelocity(float2 ScreenPos, float DeviceZ)
    {
        float3 PosN     = float3(ScreenPos, DeviceZ);
        float4 ThisClip = float4(PosN, 1);
        float4 PrevClip = mul(ThisClip, View.ClipToPrevClip);
        float3 PrevScreen = PrevClip.xyz / PrevClip.w;
        return PosN - PrevScreen;
    }

and TSR uses its result interchangeably with DecodeVelocityFromTexture() for
pixels the velocity buffer did not write (FetchAndComputeScreenVelocity, same
file). So on pixels where BOTH exist and the surface is static, the two must
agree - and a regression of one against the other has no correlation window, no
texture requirement, and no failure mode on foliage.

A correct decode gives slope 1.000.

WHAT IT DOES NOT ASSUME

  * Not the struct offset. View_ClipToPrevClip's byte offset is derived per
    dump, from an anchor found in the buffer's own bytes - see
    mvtools.derive_view_layout - and the derivation is printed so it can be
    disagreed with.
  * Not the jitter convention. ComputeStaticVelocity takes the raw pixel
    ScreenPos and does not subtract View_TemporalAAJitter, because
    View_ClipToPrevClip is built from ComputeInvProjectionNoAAMatrix() and
    ComputeProjectionNoAAMatrix() (SceneView.cpp:2772-2775) - jitter-free at
    both ends. The de-jittered variant is computed too, and reported, because
    "the engine does it this way" is a claim and the difference between the two
    is the size of the thing it would explain.
  * Not that ClipToPrevClip is even the right matrix. If the derived offset is
    wrong the regression falls apart rather than quietly reading 0.95, which is
    the useful failure mode.

Usage:  python reproject.py <dump_dir> [--field <index>] [--out <png>]
"""
import os
import sys

import numpy as np

import mvtools


def screen_pos_grid(width, height, view_rect_min, view_size):
    """ScreenPos for every pixel centre, matching UE's SvPositionToScreenPos.

        ViewportUV = (SvPosition.xy - ViewRectMin) * ViewSizeInv
        ScreenPos  = (2*UV.x - 1, 1 - 2*UV.y)

    the second line being the inverse of ScreenPosToViewportUV (Common.ush:1658),
    which is the function the decode's sign convention already comes from.

    SvPosition is the pixel CENTRE, hence the +0.5. It matters more than it
    looks: getting it wrong offsets the whole field by half a pixel, which is
    invisible next to 27px of motion and exactly the size of the residual this
    is trying to measure.
    """
    rx, ry = float(view_rect_min[0]), float(view_rect_min[1])
    vw, vh = view_size
    xs = (np.arange(width, dtype=np.float64) + 0.5 - rx) / vw
    ys = (np.arange(height, dtype=np.float64) + 0.5 - ry) / vh
    sx = 2.0 * xs - 1.0
    sy = 1.0 - 2.0 * ys
    return np.broadcast_to(sx[None, :], (height, width)), np.broadcast_to(sy[:, None], (height, width))


def compute_static_velocity_3d(screen_x, screen_y, device_z, clip_to_prev_clip, jitter=None):
    """ComputeStaticVelocity, vectorised, returning all three components.

    The z component is what makes the static-geometry test possible. UE's own
    ComputeStaticVelocity returns a float3 for the same reason - the DeviceZ
    delta a pixel would have if its only motion were the camera's.
    """
    m = clip_to_prev_clip
    sx, sy = screen_x, screen_y
    if jitter is not None:
        sx = sx - jitter[0]
        sy = sy - jitter[1]
    px = sx * m[0, 0] + sy * m[1, 0] + device_z * m[2, 0] + m[3, 0]
    py = sx * m[0, 1] + sy * m[1, 1] + device_z * m[2, 1] + m[3, 1]
    pz = sx * m[0, 2] + sy * m[1, 2] + device_z * m[2, 2] + m[3, 2]
    pw = sx * m[0, 3] + sy * m[1, 3] + device_z * m[2, 3] + m[3, 3]
    with np.errstate(divide="ignore", invalid="ignore"):
        prev_x = px / pw
        prev_y = py / pw
        prev_z = pz / pw
    if jitter is not None:
        prev_x = prev_x + jitter[2]
        prev_y = prev_y + jitter[3]
    return sx - prev_x, sy - prev_y, device_z - prev_z


def compute_static_velocity(screen_x, screen_y, device_z, clip_to_prev_clip, jitter=None):
    """ComputeStaticVelocity, vectorised. Returns (V.x, V.y) in clip-space units.

    `jitter` is None for the engine's own convention. Passing
    View_TemporalAAJitter applies the de-jittered variant instead - current
    ScreenPos minus .xy, previous minus .zw, exactly as Calculate3DVelocityBase
    does for the encoder (VelocityCommon.ush:9) - which is the alternative worth
    measuring rather than arguing about.
    """
    m = clip_to_prev_clip
    sx, sy = screen_x, screen_y
    if jitter is not None:
        sx = sx - jitter[0]
        sy = sy - jitter[1]
    # Row-vector times matrix: PrevClip[j] = sum_i ThisClip[i] * M[i][j].
    # ThisClip = (ScreenPos.x, ScreenPos.y, DeviceZ, 1).
    px = sx * m[0, 0] + sy * m[1, 0] + device_z * m[2, 0] + m[3, 0]
    py = sx * m[0, 1] + sy * m[1, 1] + device_z * m[2, 1] + m[3, 1]
    pw = sx * m[0, 3] + sy * m[1, 3] + device_z * m[2, 3] + m[3, 3]
    with np.errstate(divide="ignore", invalid="ignore"):
        prev_x = px / pw
        prev_y = py / pw
    if jitter is not None:
        prev_x = prev_x + jitter[2]
        prev_y = prev_y + jitter[3]
    return sx - prev_x, sy - prev_y


def regress(measured, analytical):
    """Slope through the origin, plus r. Slope 1.000 is a correct decode.

    Through the origin because the two quantities are the same physical thing in
    the same units - an intercept would be a bug, not a degree of freedom.
    """
    denom = float(np.dot(analytical, analytical))
    if denom <= 0:
        return float("nan"), float("nan")
    slope = float(np.dot(analytical, measured)) / denom
    if len(measured) < 2:
        return slope, float("nan")
    r = float(np.corrcoef(measured, analytical)[0, 1])
    return slope, r


def print_layout(layout, index):
    print(f"  frame {index}: View uniform buffer found in candidate slot {layout['slot']}")
    print(f"    anchor  View_ViewSizeAndInvSize at byte {layout['anchor_offset']}"
          f"  = ({layout['view_size'][0]:.0f}, {layout['view_size'][1]:.0f})")
    if len(layout["anchors"]) > 1:
        extra = ", ".join(f"{off} = ({v[0]:.0f}x{v[1]:.0f})" for off, v in layout["anchors"][1:])
        print(f"    other (W,H,1/W,1/H) float4s in the same buffer: {extra}")
        print("      (UE's View buffer also carries BufferSizeAndInvSize; the first is the view rect)")
    print(f"    layout  {layout['layout']}  ->  View_ClipToPrevClip at byte "
          f"{layout['clip_to_prev_clip_offset']}")
    for name, ok, detail in layout["checks"]:
        print(f"      [{'x' if ok else ' '}] {name}: {detail}")
    m = layout["clip_to_prev_clip"]
    print("    View_ClipToPrevClip =")
    for row in m:
        print("      " + "  ".join(f"{v:+11.6f}" for v in row))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    dump_dir = sys.argv[1]
    field_index = None
    out_path = None
    for i, arg in enumerate(sys.argv):
        if arg == "--field" and i + 1 < len(sys.argv):
            field_index = int(sys.argv[i + 1])
        if arg == "--out" and i + 1 < len(sys.argv):
            out_path = sys.argv[i + 1]

    meta = mvtools.load_meta(dump_dir)
    version = mvtools.engine_version(meta)
    print(f"dump:    {dump_dir}")
    print(f"decoding as UE {version[0]}.{version[1]}"
          f"{'' if 'engine_version_major' in meta else '  (NOT recorded in the capture)'}")

    if not mvtools.has_depth(meta):
        print("\nNo depth in this dump (no meta_depth.txt), so there is nothing to reproject from.")
        print("This test needs a capture taken with a build of the hook that identifies and copies")
        print("scene depth, and a session in which it succeeded - see the 'depth:' lines in mv_hook.log.")
        return 1

    indices = mvtools.frame_indices(dump_dir)
    print(f"frames:  {len(indices)}")

    # ---------------------------------------------------------------------
    # 1. Where is View_ClipToPrevClip? Derived per frame, from the bytes.
    # ---------------------------------------------------------------------
    print("\n" + "=" * 72)
    print("Locating View_ClipToPrevClip in the captured constant buffers")
    print("=" * 72)
    print("The hook copies the few most frequently bound root constant buffers per frame")
    print("and does not decide which is the View uniform buffer. That is decided here, from")
    print("the buffer's own contents: a float4 of (W, H, 1/W, 1/H) is View_ViewSizeAndInvSize,")
    print("and the byte distance from there back to View_ClipToPrevClip is read off the member")
    print("ordering in SceneView.h. Nothing below is a constant carried over from another title.")
    print()

    render_extent = (meta["velocity_width"], meta["velocity_height"])
    print(f"Anchors are required to be within a few pixels of the captured velocity texture's")
    print(f"{render_extent[0]}x{render_extent[1]} extent, so a (W, H, 1/W, 1/H) float4 belonging to some")
    print(f"downsampled buffer - or to a constant buffer that is not the View buffer at all -")
    print(f"cannot anchor the layout. That is not a fitted constant: the extent is read out of")
    print(f"this dump's own meta.txt.")
    print()

    layouts = {}
    for index in indices:
        buffers = mvtools.load_view_cb(dump_dir, index, meta)
        if buffers is None:
            continue
        for slot in range(buffers.shape[0]):
            found = mvtools.derive_view_layout(buffers[slot], render_extent)
            if found is not None and found["checks_passed"] >= 3:
                found["slot"] = slot
                layouts[index] = found
                break

    if not layouts:
        print("  FAILED - no captured constant buffer contains a (W, H, 1/W, 1/H) float4.")
        print("  Either the View uniform buffer was not among the most-bound root CBVs, or it")
        print("  was not bound as a root CBV at all on this title. Nothing below can run.")
        return 1

    first = min(layouts)
    print_layout(layouts[first], first)
    offsets = {v["clip_to_prev_clip_offset"] for v in layouts.values()}
    slots = {v["slot"] for v in layouts.values()}
    print(f"\n  Derived on {len(layouts)}/{len(indices)} frames. "
          f"ClipToPrevClip offset(s) seen: {sorted(offsets)}; candidate slot(s): {sorted(slots)}.")
    print("  The handoff note for this phase asserted byte offset 1872 for this title, sourced to")
    print("  a shader disassembly. That disassembly is not in DEBUGGING.md, so the number is")
    print("  treated here as a claim to check rather than as an input:")
    for off in sorted(offsets):
        print(f"    derived {off} vs asserted 1872 -> {'AGREES' if off == 1872 else 'DISAGREES'}")

    # ---------------------------------------------------------------------
    # 2. The regression: decoded velocity against analytical reprojection.
    # ---------------------------------------------------------------------
    print("\n" + "=" * 72)
    print("Decoded SceneVelocity vs analytical reprojection, per pixel")
    print("=" * 72)
    print("Pixels are used only where the velocity buffer was WRITTEN and the surface is")
    print("static in depth - the decoded V.z (channels 2/3, the DeviceZ delta) near zero.")
    print("Those are the pixels whose velocity is pure camera motion, which is the only thing")
    print("the reprojection reconstructs. A correct decode gives slope 1.000.")
    print()

    width, height = meta["velocity_width"], meta["velocity_height"]
    if meta["depth_plane0_width"] != width or meta["depth_plane0_height"] != height:
        print(f"  NOTE: depth is {meta['depth_plane0_width']}x{meta['depth_plane0_height']} and velocity is "
              f"{width}x{height}. They are compared pixel-for-pixel over the overlap only.")
    overlap_w = min(width, meta["depth_plane0_width"])
    overlap_h = min(height, meta["depth_plane0_height"])

    # What the depth capture actually contains, before anything is done with it.
    # A depth buffer that is entirely at the far plane, or entirely at one
    # value, is a plumbing test rather than a scene - and the difference is
    # invisible in a slope that comes out NaN.
    probe_index = next((i for i in indices
                        if os.path.exists(os.path.join(dump_dir, f"depth_{i:05d}.bin"))), None)
    if probe_index is not None:
        probe = mvtools.load_depth(os.path.join(dump_dir, f"depth_{probe_index:05d}.bin"), meta)
        finite_probe = probe[np.isfinite(probe)]
        print(f"  depth (frame {probe_index}, plane 0, format {meta['depth_plane0_format']}): "
              f"min {finite_probe.min():.6f}  median {np.median(finite_probe):.6f}  "
              f"max {finite_probe.max():.6f}")
        print(f"    at the far plane (DeviceZ == 0, reversed-Z): "
              f"{float((finite_probe == 0).mean()) * 100:.1f}% of pixels")
        print()

    rows = []
    degenerate = 0
    pooled_measured_x, pooled_analytic_x = [], []
    pooled_measured_y, pooled_analytic_y = [], []
    pooled_dejitter_x, pooled_dejitter_y = [], []
    for index in indices:
        if index not in layouts:
            continue
        depth_path = os.path.join(dump_dir, f"depth_{index:05d}.bin")
        if not os.path.exists(depth_path):
            continue
        vel_path = os.path.join(dump_dir, f"vel_{index:05d}.bin")
        layout = layouts[index]

        device_z = mvtools.load_depth(depth_path, meta)[:overlap_h, :overlap_w]
        decoded = mvtools.decode_velocity_clip(vel_path, meta)[:overlap_h, :overlap_w]
        written = mvtools.velocity_written_mask(vel_path, meta)[:overlap_h, :overlap_w]
        vz, _ = mvtools.decode_velocity_depth(vel_path, meta, version)
        vz = vz[:overlap_h, :overlap_w]

        sx, sy = screen_pos_grid(overlap_w, overlap_h, layout["view_rect_min"], layout["view_size"])
        ax, ay, az = compute_static_velocity_3d(sx, sy, device_z, layout["clip_to_prev_clip"])
        jx, jy = compute_static_velocity(
            sx, sy, device_z, layout["clip_to_prev_clip"], layout["temporal_aa_jitter"])

        finite = np.isfinite(vz) & np.isfinite(ax) & np.isfinite(ay) & np.isfinite(az) & np.isfinite(device_z)
        # DeviceZ == 0 is the far plane in UE5's reversed-Z buffer: sky, or
        # anything the depth pass never wrote. Reprojecting it divides by a w
        # that carries no geometry.
        usable = written & finite & (device_z > 1e-7)
        if usable.sum() < 100:
            continue

        # Which written pixels are actually on static geometry.
        #
        # This is the whole difficulty of the test, and the first version got it
        # wrong. SceneVelocity is written for dynamic and world-position-offset
        # geometry, so "written" selects, by construction, a population that
        # includes everything this reference cannot predict: the wind-animated
        # foliage whose motion is its material's, not the camera's. Selecting on
        # a percentile of |V.z| barely helps - foliage sways mostly laterally -
        # and the symptom was frames that regressed at 1.0000 while later,
        # more foliage-heavy frames of the same capture regressed at 0.02.
        #
        # The test used instead is per-pixel and uses a DIFFERENT channel from
        # the one being regressed. The reprojection predicts all three
        # components of V; the encoder stores V.z in channels 2/3. So ask of
        # each pixel: is its DEPTH motion explained by the camera alone? If yes,
        # the surface is not moving towards or away from the camera under its
        # own power, and its lateral motion is then a fair question to put to
        # the reference. Selecting on z and regressing on x/y keeps the two
        # apart - this is not the test grading its own homework.
        #
        # It is not a complete filter. Foliage swaying exactly perpendicular to
        # the view direction has no depth signature and will pass. So this is a
        # necessary condition for static geometry, not a sufficient one, and the
        # residual it leaves is stated rather than assumed away.
        z_scale = float(np.percentile(np.abs(az[usable]), 90))
        z_tolerance = max(0.05 * z_scale, 1e-9)
        static = usable & (np.abs(vz - az) <= z_tolerance)
        if static.sum() < 100:
            continue

        # A regression against a constant is not a measurement. It happens for a
        # real reason - a stationary camera gives ClipToPrevClip == identity and
        # an analytical field that is identically zero - and reporting the NaN
        # that comes out of it as a slope would be exactly the kind of
        # plausible-looking number this project keeps having to retract.
        if float(np.max(np.abs(ax[static]))) < 1e-9 and float(np.max(np.abs(ay[static]))) < 1e-9:
            degenerate += 1
            continue

        mx = decoded[:, :, 0][static]
        my = decoded[:, :, 1][static]
        slope_x, r_x = regress(mx, ax[static])
        slope_y, r_y = regress(my, ay[static])
        rows.append((index, int(static.sum()), slope_x, r_x, slope_y, r_y))
        pooled_measured_x.append(mx)
        pooled_analytic_x.append(ax[static])
        pooled_measured_y.append(my)
        pooled_analytic_y.append(ay[static])
        pooled_dejitter_x.append(jx[static])
        pooled_dejitter_y.append(jy[static])

    if degenerate:
        print(f"  {degenerate} frame(s) skipped: View_ClipToPrevClip is the identity there, so the")
        print("  analytical field is identically zero and there is nothing to regress against.")
        print("  That is what a stationary camera looks like - and what the synthetic testhost")
        print("  dump is, by construction.")
        print()

    if not rows:
        print("  No frame had enough written, static, finite pixels to regress. The three")
        print("  things that produce this, in the order worth checking:")
        print("    * the depth buffer is all far plane (see the depth line above) - that is")
        print("      what the synthetic testhost dump looks like, and it is not a scene;")
        print("    * the velocity buffer was never written (a menu, a paused frame);")
        print("    * no written pixel is static in depth, i.e. everything with velocity in")
        print("      this footage is also moving towards or away from the camera.")
        return 1

    print(f"  {'frame':>6}  {'pixels':>9}  {'slope X':>8} {'r X':>7}  {'slope Y':>8} {'r Y':>7}")
    for index, n, sx_, rx_, sy_, ry_ in rows[:12]:
        print(f"  {index:>6}  {n:>9}  {sx_:>8.4f} {rx_:>+7.4f}  {sy_:>8.4f} {ry_:>+7.4f}")
    if len(rows) > 12:
        print(f"  ... {len(rows) - 12} more frames")

    mx = np.concatenate(pooled_measured_x)
    my = np.concatenate(pooled_measured_y)
    ax = np.concatenate(pooled_analytic_x)
    ay = np.concatenate(pooled_analytic_y)
    jx = np.concatenate(pooled_dejitter_x)
    jy = np.concatenate(pooled_dejitter_y)
    slope_x, r_x = regress(mx, ax)
    slope_y, r_y = regress(my, ay)
    dslope_x, dr_x = regress(mx, jx)
    dslope_y, dr_y = regress(my, jy)

    print()
    print(f"  POOLED over {len(rows)} frames and {len(mx):,} pixels:")
    print(f"    engine convention (ComputeStaticVelocity, no jitter subtraction)")
    print(f"      axis X:  decoded = {slope_x:.4f} * analytical    r = {r_x:+.4f}")
    print(f"      axis Y:  decoded = {slope_y:.4f} * analytical    r = {r_y:+.4f}")
    print(f"    de-jittered variant (Calculate3DVelocityBase's convention, for comparison)")
    print(f"      axis X:  decoded = {dslope_x:.4f} * analytical    r = {dr_x:+.4f}")
    print(f"      axis Y:  decoded = {dslope_y:.4f} * analytical    r = {dr_y:+.4f}")
    print()
    print("  Slope 1.000 means the decode is exactly right and the ~5% shortfall the")
    print("  image-matching references report is a property of image matching on this")
    print("  footage. A slope near 0.95 would mean the decode really is off, which would be")
    print("  a surprise given the encode was read out of the game's own compiled shader.")

    # ---------------------------------------------------------------------
    # 3. The full-screen field.
    # ---------------------------------------------------------------------
    print("\n" + "=" * 72)
    print("The full-screen field")
    print("=" * 72)
    print("SceneVelocity carries velocity for dynamic and WPO geometry only. Everything")
    print("else - ground, rocks, sky - is black in it, and correctly so: the consuming")
    print("shader reconstructs those pixels from the camera matrices. Doing the same thing")
    print("to the UNWRITTEN pixels, rather than comparing against the written ones, is what")
    print("turns a sparse buffer into a dense field. It is the same calculation as above.")
    print()

    target = field_index if field_index is not None else (rows[len(rows) // 2][0])
    if target not in layouts:
        target = rows[0][0]
    layout = layouts[target]
    vel_path = os.path.join(dump_dir, f"vel_{target:05d}.bin")
    depth_path = os.path.join(dump_dir, f"depth_{target:05d}.bin")
    device_z = mvtools.load_depth(depth_path, meta)[:overlap_h, :overlap_w]
    decoded = mvtools.decode_velocity_clip(vel_path, meta)[:overlap_h, :overlap_w]
    written = mvtools.velocity_written_mask(vel_path, meta)[:overlap_h, :overlap_w]
    sx, sy = screen_pos_grid(overlap_w, overlap_h, layout["view_rect_min"], layout["view_size"])
    ax2, ay2 = compute_static_velocity(sx, sy, device_z, layout["clip_to_prev_clip"])

    # DeviceZ == 0 is the far plane in UE5's reversed-Z buffer - sky, or
    # anything the depth pass never wrote - and reprojecting it divides by a w
    # that carries no geometry. This used to go through nan_to_num() unguarded,
    # which does not just zero the result: reprojecting DeviceZ=0 on this dump
    # produces a FINITE, plausible-looking clip-space delta (up to 0.92, i.e.
    # median ~940px/frame on the sky pixels of frame 5) because the matrix
    # multiply has no reason to blow up just because the input is meaningless.
    # That is precisely the failure mode this project's own analytical-reference
    # argument (see the module docstring) was supposed to be immune to: a
    # confident number with nothing behind it, this time produced by the
    # reference itself rather than by an image-matching one. It was counted as
    # "reconstructed" in the coverage line and painted into the right-hand
    # panel as if it were data. Caught by comparing this script's own coverage
    # figure against make_video.py's independent computation of the same
    # quantity, which excludes the far plane and disagreed by 7 points.
    reconstructable = (device_z > 1e-7) & np.isfinite(ax2) & np.isfinite(ay2)
    have_data = written | reconstructable

    dense = np.zeros((overlap_h, overlap_w, 2), dtype=np.float32)
    dense[:, :, 0] = np.where(written, decoded[:, :, 0], np.where(reconstructable, ax2, 0.0))
    dense[:, :, 1] = np.where(written, decoded[:, :, 1], np.where(reconstructable, ay2, 0.0))
    coverage = float(written.mean())
    reconstructed_frac = float((reconstructable & ~written).mean())
    no_data_frac = float((~have_data).mean())
    print(f"  frame {target}: {coverage * 100:.1f}% of pixels came from SceneVelocity, "
          f"{reconstructed_frac * 100:.1f}% reconstructed from depth and View_ClipToPrevClip, "
          f"{no_data_frac * 100:.1f}% neither (the far plane - sky - has no depth to reproject).")
    px = -0.5 * dense[:, :, 0] * overlap_w
    py = +0.5 * dense[:, :, 1] * overlap_h
    mag = np.sqrt(px * px + py * py)
    print(f"  displacement, whole screen (pixels with data only): median {np.median(mag[have_data]):.2f} px, "
          f"p99 {np.percentile(mag[have_data], 99):.2f} px, max {mag[have_data].max():.2f} px")
    print(f"  displacement, written region only: median {np.median(mag[written]):.2f} px")
    print(f"  displacement, reconstructed region only: median "
          f"{np.median(mag[reconstructable & ~written]):.2f} px")

    if out_path is None:
        tag = os.environ.get("MV_RESULT_TAG", "")
        suffix = f"_{tag}" if tag else ""
        out_path = os.path.normpath(
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results",
                         f"fullscreen_field{suffix}.png"))
    try:
        import cv2
        uv = np.empty_like(dense)
        uv[:, :, 0] = -0.5 * dense[:, :, 0]
        uv[:, :, 1] = +0.5 * dense[:, :, 1]
        sparse_uv = np.zeros_like(uv)
        sparse_uv[:, :, 0] = np.where(written, -0.5 * decoded[:, :, 0], 0.0)
        sparse_uv[:, :, 1] = np.where(written, +0.5 * decoded[:, :, 1], 0.0)
        scale = max(float(np.percentile(mag[have_data], 99)), 1e-3)

        sparse_rgb = mvtools.flow_to_rgb(sparse_uv, meta, max_pixels=scale)
        # Force the unwritten region to actual black rather than leaving it at
        # flow_to_rgb's brightness floor. The floor exists so slow-moving
        # written pixels do not vanish, but applied to unwritten pixels it
        # paints "no data" in the same colour vocabulary as data - and the point
        # of the left panel is precisely which pixels the engine wrote.
        sparse_rgb[~written] = 0

        # The game's own frame first, so the field can be read against the scene
        # rather than admired on its own. Same order as the existing figures in
        # results/.
        color = mvtools.to_display(mvtools.load_color(
            os.path.join(dump_dir, f"color_{target:05d}.bin"), meta))
        color_bgr = (np.clip(color[:overlap_h, :overlap_w, ::-1], 0, 1) * 255).astype(np.uint8)
        if color_bgr.shape[:2] != sparse_rgb.shape[:2]:
            color_bgr = cv2.resize(color_bgr, (sparse_rgb.shape[1], sparse_rgb.shape[0]))

        dense_rgb = mvtools.flow_to_rgb(uv, meta, max_pixels=scale)
        # Same reasoning as the sparse panel: a pixel with no data (the far
        # plane) is not the same claim as a pixel with a measured zero, and
        # painting it in the same palette as real reconstruction is what this
        # function's docstring is warning against one function up.
        dense_rgb[~have_data] = 0

        panels = [color_bgr, sparse_rgb, dense_rgb]
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        cv2.imwrite(out_path, np.concatenate(panels, axis=1))
        print(f"\n  wrote {out_path}")
        print("  Left:   the game's own back buffer.")
        print("  Middle: SceneVelocity alone - black is where the engine never wrote it.")
        print("  Right:  the same frame with those pixels reconstructed from depth and")
        print("          View_ClipToPrevClip. Black is where neither source has data - the far")
        print("          plane has no depth to reproject. The middle and right panels share a")
        print("          brightness scale, so the two are directly comparable.")
    except ImportError:
        print("\n  (cv2 not available - skipping the figure)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
