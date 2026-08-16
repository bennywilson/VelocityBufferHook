"""Block matching against the captured colour frames.

This is the project's only source of *independent* ground truth. Everything
else - the decode constants, the warp test, the scale fit - is derived from the
velocity buffer itself, so it can only ever show internal consistency. Block
matching measures displacement from the imagery, and knows nothing about the
velocity buffer at all.

Why not optical flow: the subjects here are large, smooth, low-texture objects
(a weapon viewmodel, the flat side of a train). Farneback is unreliable on
those - the aperture problem - and reports near-zero regardless of the truth,
which is a failure mode that looks exactly like a correct "nothing moved"
answer. An earlier version of derive_scale.py was built on Farneback and its
numbers should not be trusted; see DEBUGGING.md. Block matching measures
displacement of a *region* directly and has no such failure mode, at the cost
of assuming the region is locally rigid.

Both matchers here work on the SAD (sum of absolute differences) volume:
for every candidate integer shift, the mean absolute difference between the
current frame and the shifted previous frame, box-filtered over a window. The
integer minimum is then refined to sub-pixel by fitting a parabola through it
and its two neighbours along each axis, independently. That refinement matters:
much of the real motion in these captures is well under a pixel, and an
integer-only matcher would report 0 for all of it and make a sub-pixel decode
look like a phantom.

Sign convention matches the decode: the returned displacement is the offset
from a pixel in frame N back to where that surface was in frame N-1.
"""
import numpy as np

try:
    import cv2
except ImportError:  # pragma: no cover - cv2 is required by every caller
    cv2 = None


def _parabolic1(sm1, s0, sp1):
    """Scalar form: sub-pixel offset of the minimum of a parabola through
    three equally-spaced samples. 0 on a degenerate (flat/non-convex) triple."""
    denom = sm1 - 2.0 * s0 + sp1
    if denom <= 1e-9:
        return 0.0
    return float(np.clip(0.5 * (sm1 - sp1) / denom, -1.0, 1.0))


def _parabolic(sm1, s0, sp1):
    """Array form of _parabolic1, applied elementwise."""
    denom = sm1 - 2.0 * s0 + sp1
    out = np.zeros_like(s0, dtype=np.float32)
    ok = denom > 1e-9
    out[ok] = 0.5 * (sm1[ok] - sp1[ok]) / denom[ok]
    return np.clip(out, -1.0, 1.0)


def _sad_map(cur, padded, search, dx, dy, window):
    """Box-filtered |cur - prev shifted by (dx, dy)|, at full resolution.

    Shift (dy, dx) samples prev at (y+dy, x+dx), so the minimising shift is the
    current-to-previous displacement directly. `padded` is prev edge-replicated
    by `search` on all sides, which makes each shift a plain slice - much
    cheaper than a warp, and exact for integer offsets.
    """
    h, w = cur.shape
    y0, x0 = search + dy, search + dx
    shifted = padded[y0:y0 + h, x0:x0 + w]
    return cv2.boxFilter(np.abs(cur - shifted), -1, (window, window), normalize=True)


def coarse_shift(cur, prev, mask=None, max_shift=400):
    """Integer global displacement, with no small search radius to be blind above.

    THIS EXISTS BECAUSE THE TOOLING WAS THE WEAK LINK, TWICE.

    Everything else in this module refines a displacement within +-`search`
    pixels, which is 14 by default. That is a sensible refinement window and a
    hopeless search window: on a title moving 60px per frame, the minimum sits
    on the edge of the search box on every pixel, and what comes back is not an
    error but a plausible small number. Measured against a capture whose true
    motion was 27px, check_decode.py reported "mean block-matched 1.68px" and
    the decode looked wrong by 16x. It was not; the reference was.

    Phase correlation is not the answer either - it measures translation only,
    so a camera flying forward (radial expansion, no net translation) reads as
    ~0.2px. Two references failing in two different ways looked like agreement.

    So: normalised cross-correlation of a central crop over a wide window, which
    has no radius assumption and reports its own match quality. Returns
    (dx, dy, score); score below ~0.5 means do not trust the answer.
    """
    h, w = cur.shape
    if mask is not None and mask.any():
        # Centre the template on the region being scored, not on the middle of
        # the frame. They are not the same place and they do not move the same
        # way: the written region of a velocity buffer is often foliage at the
        # frame edges, whose world-position-offset animation rides on top of the
        # camera's motion. Centring on the frame middle measures the background,
        # leaves the residual larger than the refinement window, and the answer
        # comes back clipped low - which is the same failure this function was
        # written to fix, one level in.
        ys_, xs_ = np.where(mask)
        cy, cx = int(ys_.mean()), int(xs_.mean())
        hy, hx = int(h * 0.22), int(w * 0.20)
        y0, y1 = max(0, cy - hy), min(h, cy + hy)
        x0, x1 = max(0, cx - hx), min(w, cx + hx)
    else:
        y0, y1 = int(h * 0.30), int(h * 0.75)
        x0, x1 = int(w * 0.30), int(w * 0.70)
    template = cur[y0:y1, x0:x1]
    pad = min(max_shift, x0, y0, w - x1, h - y1)
    if pad < 4 or template.size == 0:
        return 0, 0, 0.0
    ys, ye, xs, xe = y0 - pad, y1 + pad, x0 - pad, x1 + pad
    result = cv2.matchTemplate(prev[ys:ye, xs:xe].astype(np.float32),
                               template.astype(np.float32), cv2.TM_CCOEFF_NORMED)
    _, score, _, loc = cv2.minMaxLoc(result)
    return (xs + loc[0]) - x0, (ys + loc[1]) - y0, float(score)


def global_shift(cur, prev, mask, search=14, max_samples=20000, seed=0, coarse=True):
    """Single rigid displacement of the masked region, to sub-pixel precision.

    Used both as ground truth for a region that really is one rigid object, and
    as the "best global translation" control in warp_validate.py - if a single
    shift explains the frame as well as the per-pixel field does, the per-pixel
    field has not been shown to carry per-pixel information.

    Scores only the masked pixels, not their bounding box. The written region
    of a velocity buffer is rarely box-shaped - a weapon viewmodel fills ~8% of
    the frame but its bounding box is far larger - and averaging over the box
    drowns the moving subject in static background, dragging the match toward
    (0,0) and making any decode look like a phantom.

    Those pixels are then subsampled to `max_samples`. Evaluating every masked
    pixel at every one of (2*search+1)^2 candidate shifts is ~10^9 operations
    per frame pair, which made a full validation run take longer than the
    capture did; 20k samples is far more than enough to localise a mean
    absolute difference and makes it ~100x cheaper. The sample is drawn from a
    fixed seed so results are reproducible.

    Returns (dx, dy) as floats, or None if the mask is too small or too close
    to the border to search.
    """
    h, w = cur.shape
    # Centre the refinement window on a coarse estimate, so `search` bounds the
    # PRECISION of the answer rather than its MAGNITUDE. Without this the whole
    # function silently returns ~0 whenever the true motion exceeds `search`.
    cdx = cdy = 0
    if coarse:
        cdx, cdy, score = coarse_shift(cur, prev, mask)
        if score < 0.5:
            cdx = cdy = 0  # no trustworthy coarse estimate; fall back to centred
    # Exclude pixels whose search window would leave the image.
    margin = search + 1 + max(abs(cdx), abs(cdy))
    if 2 * margin >= min(h, w):
        return None
    inside = np.zeros_like(mask, dtype=bool)
    inside[margin:h - margin, margin:w - margin] = True
    ys, xs = np.where(mask & inside)
    if ys.size < 64:
        return None
    if ys.size > max_samples:
        pick = np.random.default_rng(seed).choice(ys.size, max_samples, replace=False)
        ys, xs = ys[pick], xs[pick]

    ref = cur[ys, xs]
    n = 2 * search + 1
    scores = np.empty((n, n), dtype=np.float64)
    for iy, dy in enumerate(range(-search, search + 1)):
        for ix, dx in enumerate(range(-search, search + 1)):
            scores[iy, ix] = np.abs(ref - prev[ys + dy + cdy, xs + dx + cdx]).mean()

    iy, ix = np.unravel_index(np.argmin(scores), scores.shape)
    dy, dx = iy - search, ix - search
    # Refine, where the minimum is not on the edge of the search window. A
    # minimum sitting on the edge means the true displacement is outside the
    # search radius, and interpolating there would invent precision.
    if 0 < ix < n - 1:
        dx += _parabolic1(scores[iy, ix - 1], scores[iy, ix], scores[iy, ix + 1])
    if 0 < iy < n - 1:
        dy += _parabolic1(scores[iy - 1, ix], scores[iy, ix], scores[iy + 1, ix])
    return float(dx + cdx), float(dy + cdy)


def _dense_residual(cur, prev, search, window):
    """Per-pixel displacement field, sub-pixel, from the SAD volume.

    This is what the scale fit needs: a displacement measured *independently at
    each pixel*, so the regression against the decoded field has real per-pixel
    variation on both axes rather than one number per frame.

    `window` is the correlation window in pixels - large enough that a smooth
    surface still has enough gradient inside it to localise, small enough that
    two objects moving differently rarely share one. `search` bounds the
    representable displacement; anything faster is clipped to the window edge
    and flagged via the returned confidence.

    Returns (flow_xy, confidence, clipped):
      flow_xy    (h, w, 2) float32, pixels, current -> previous
      confidence SAD margin between best and second-best distinct shift - low
                 values mean an ambiguous match (flat or repeating region)
      clipped    True where the RESIDUAL landed on the edge of the search
                 window, i.e. the match was pinned rather than measured

    `clipped` is returned rather than left to the caller to infer from the
    magnitude of flow_xy. That inference used to be `abs(flow) < search`, which
    was equivalent while `search` bounded the whole displacement - and became
    wrong the moment a coarse pre-shift was added, because the total then
    legitimately exceeds the window and only the residual can be pinned. The
    old test silently discarded every pixel moving faster than the window,
    which is precisely the population the scale fit needs.
    """
    if cv2 is None:
        raise RuntimeError("blockmatch requires opencv-python")
    cur = np.asarray(cur, dtype=np.float32)
    prev = np.asarray(prev, dtype=np.float32)
    h, w = cur.shape
    padded = cv2.copyMakeBorder(prev, search, search, search, search, cv2.BORDER_REPLICATE)

    # Two streaming passes rather than one materialised SAD volume: at these
    # resolutions a (2*search+1)^2 x h x w float32 stack is gigabytes, which is
    # how the first version of this fell over.
    #
    # Pass 1: running argmin over shifts.
    best = np.full((h, w), np.inf, dtype=np.float32)
    best_dx = np.zeros((h, w), dtype=np.int16)
    best_dy = np.zeros((h, w), dtype=np.int16)
    for dy in range(-search, search + 1):
        for dx in range(-search, search + 1):
            sad = _sad_map(cur, padded, search, dx, dy, window)
            better = sad < best
            best = np.where(better, sad, best)
            best_dx[better] = dx
            best_dy[better] = dy

    # Pass 2: the three SAD samples each pixel needs for its own sub-pixel fit,
    # plus the best score outside a 3x3 neighbourhood of the winner (the
    # ambiguity margin). Both depend on where the winner landed, which is only
    # known after pass 1.
    s_xm = np.full((h, w), np.nan, dtype=np.float32)
    s_xp = np.full((h, w), np.nan, dtype=np.float32)
    s_ym = np.full((h, w), np.nan, dtype=np.float32)
    s_yp = np.full((h, w), np.nan, dtype=np.float32)
    second = np.full((h, w), np.inf, dtype=np.float32)
    for dy in range(-search, search + 1):
        for dx in range(-search, search + 1):
            sad = _sad_map(cur, padded, search, dx, dy, window)
            same_y, same_x = best_dy == dy, best_dx == dx
            np.copyto(s_xm, sad, where=same_y & (best_dx == dx + 1))
            np.copyto(s_xp, sad, where=same_y & (best_dx == dx - 1))
            np.copyto(s_ym, sad, where=same_x & (best_dy == dy + 1))
            np.copyto(s_yp, sad, where=same_x & (best_dy == dy - 1))
            far = (np.abs(best_dy - dy) > 1) | (np.abs(best_dx - dx) > 1)
            second = np.where(far & (sad < second), sad, second)

    # A minimum on the edge of the search window is a clipped match, not a
    # refinable one - its outer neighbour was never evaluated (NaN here).
    ddx = np.where(np.isnan(s_xm) | np.isnan(s_xp), 0.0,
                   _parabolic(np.nan_to_num(s_xm), best, np.nan_to_num(s_xp)))
    ddy = np.where(np.isnan(s_ym) | np.isnan(s_yp), 0.0,
                   _parabolic(np.nan_to_num(s_ym), best, np.nan_to_num(s_yp)))

    flow = np.empty((h, w, 2), dtype=np.float32)
    flow[:, :, 0] = best_dx + ddx
    flow[:, :, 1] = best_dy + ddy
    clipped = (np.abs(best_dx) >= search) | (np.abs(best_dy) >= search)
    return flow, (second - best).astype(np.float32), clipped


def dense(cur, prev, search=8, window=17, levels=4):
    """Per-pixel displacement field, coarse-to-fine over an image pyramid.

    WHY A PYRAMID, AND WHAT IT REPLACED

    The single-level version searched +-`search` pixels around zero, which made
    `search` bound the representable displacement rather than the precision of
    it. On the title this was developed against that was invisible: motion was
    under a pixel. On a second title moving 15-70px per frame it was fatal, and
    fatal quietly - every fast pixel pinned to the window edge, discarded by the
    caller's clipping filter, and the scale regression left fitting the slowest
    1% of the frame. The reported slope was 0.73 where a correct decode gives
    1.000, which reads exactly like a decode error rather than a measurement
    that never saw the data.

    A single GLOBAL pre-shift is not enough either. It centres every pixel's
    search on the same displacement, so a pixel moving differently from the
    scene as a whole - which is the entire point of a per-pixel field, and is
    what velocity is written for - is still pinned. Only a per-pixel coarse
    estimate fixes that, which is what warping by the upsampled flow at each
    level provides.

    Representable displacement is now roughly search * (2^levels - 1): ~120px
    at the defaults, against 8px before, while level 0 still refines to the same
    sub-pixel precision.

    Returns (flow_xy, confidence, clipped) exactly as the single-level core does;
    `clipped` refers to the residual at the finest level, which is the only
    level where being pinned still means the answer is untrustworthy.
    """
    if cv2 is None:
        raise RuntimeError("blockmatch requires opencv-python")
    cur = np.asarray(cur, dtype=np.float32)
    prev = np.asarray(prev, dtype=np.float32)

    pyr_cur, pyr_prev = [cur], [prev]
    for _ in range(max(0, levels - 1)):
        if min(pyr_cur[-1].shape) < 4 * window:
            break  # below this the correlation window covers the whole image
        pyr_cur.append(cv2.pyrDown(pyr_cur[-1]))
        pyr_prev.append(cv2.pyrDown(pyr_prev[-1]))

    flow = None
    confidence = clipped = None
    for level in range(len(pyr_cur) - 1, -1, -1):
        c, p = pyr_cur[level], pyr_prev[level]
        h, w = c.shape
        if flow is None:
            flow = np.zeros((h, w, 2), dtype=np.float32)
        else:
            # Upsample the coarser level's answer and double it: one pixel of
            # displacement at half resolution is two at full.
            flow = cv2.resize(flow, (w, h), interpolation=cv2.INTER_LINEAR) * 2.0

        # Warp prev by the estimate so far, so the search only has to find what
        # is left over. Sampling at (x + flow) matches _sad_map's convention,
        # where the minimising shift is the current -> previous displacement.
        gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
        warped = cv2.remap(p, gx + flow[:, :, 0], gy + flow[:, :, 1],
                           cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)

        residual, confidence, clipped = _dense_residual(c, warped, search, window)
        flow = flow + residual

    return flow, confidence, clipped
