"""Decoding helpers for the raw buffers dumped by the hook.

The hook writes untouched GPU bytes plus a meta.txt describing them, so all
format knowledge lives here rather than in the injected DLL - keeping the
in-process code as small and as low-risk as possible.
"""
import os
import numpy as np

# DXGI formats we actually encounter, verified against dxgiformat.h in the
# installed Windows SDK (10.0.26100.0), not recalled. The neighbours matter:
# R16G16B16A16_FLOAT is 10 and R16G16B16A16_UNORM is 11, and confusing those two
# is the error that cost this project the most (see DEBUGGING.md). R16G16_UNORM
# sits at 35, between R16G16_FLOAT (34) and R16G16_UINT (36).
DXGI_R16G16B16A16_UNORM = 11
DXGI_R16G16B16A16_UINT = 12
DXGI_R10G10B10A2_UNORM = 24
DXGI_R8G8B8A8_UNORM = 28
DXGI_R16G16_UNORM = 35
DXGI_R16G16_UINT = 36
DXGI_B8G8R8A8_UNORM = 87

# The four formats FVelocityRendering::GetFormat can return (VelocityRendering.cpp:758),
# crossed with the D3D12 RHI's PF_* -> DXGI table (D3D12RHI.cpp:139,144):
#
#     bNeedVelocityDepth ? PF_A16B16G16R16 : PF_G16R16              (desktop)
#     bNeedVelocityDepth ? PF_R16G16B16A16_UINT : PF_R16G16_UINT    (OpenGL/GLES)
#
# -> (channels, bytes per pixel). The UINT variants store the same 0..65535
# integers the UNORM ones do - the GLES path just writes them explicitly
# (`return uint4(EncodedV * 65535.0 + 0.5f)`, Common.ush:2081) - so normalising
# by 65535 recovers the identical [0,1] value and the decode below is unchanged.
VELOCITY_FORMATS = {
    DXGI_R16G16B16A16_UNORM: (4, 8),
    DXGI_R16G16B16A16_UINT: (4, 8),
    DXGI_R16G16_UNORM: (2, 4),
    DXGI_R16G16_UINT: (2, 4),
}


def load_meta(dump_dir):
    """Everything the hook recorded about the dump's layout.

    Three files, merged. meta.txt is written once, when the colour readback
    buffer is created; meta_depth.txt and meta_viewcb.txt are written later,
    when (and only if) those parts of the capture were identified. They are
    separate on purpose - depth identification cannot start until velocity
    identification has produced an extent to test against, so waiting for it
    before writing meta.txt would risk never writing meta.txt at all on a
    session where depth is never found. A dump missing meta_depth.txt is a dump
    with no depth in it, which is what it looks like here too.
    """
    meta = {}
    for name in ("meta.txt", "meta_depth.txt", "meta_viewcb.txt"):
        path = os.path.join(dump_dir, name)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                meta[k] = int(v)
    return meta


def _rows(path, width, height, row_pitch, bytes_per_pixel):
    """Un-pad a GPU row-pitched buffer into a tight (height, width*bpp) array.

    D3D12 pads each row up to a 256-byte multiple, and GetCopyableFootprints
    reports a total size where the *last* row is not padded - so the file is
    row_pitch*(height-1) + width*bpp bytes, not row_pitch*height.
    """
    raw = np.fromfile(path, dtype=np.uint8)
    useful = width * bytes_per_pixel
    out = np.empty((height, useful), dtype=np.uint8)
    for y in range(height):
        start = y * row_pitch
        out[y] = raw[start:start + useful]
    return out


def velocity_channels(meta):
    """(channels, bytes per pixel) for the captured velocity format.

    Read from meta.txt rather than asserted. The previous version hardcoded
    4 channels and 8 bytes per pixel and asserted the format was
    R16G16B16A16_UNORM, which is what the one title this was developed against
    happens to use - a title without Lumen, distance fields or ray tracing gets
    PF_G16R16 (2 channels, 4 bytes) from the same engine code, and would have
    failed here with an AssertionError naming a number, at the very end of the
    pipeline, long after the expensive part.
    """
    fmt = meta["velocity_format"]
    if fmt not in VELOCITY_FORMATS:
        raise ValueError(
            f"velocity format {fmt} is not one FVelocityRendering::GetFormat can return "
            f"({sorted(VELOCITY_FORMATS)}). Either the identification filter selected the wrong "
            f"resource, or this engine version uses a format this tool has not been told about - "
            f"check FVelocityRendering::GetFormat for the version in question before adding it.")
    return VELOCITY_FORMATS[fmt]


def load_velocity_raw(path, meta):
    """Returns raw velocity as float (h, w, channels) in [0,1].

    Channel count depends on the format: 4 for PF_A16B16G16R16 (the
    NeedVelocityDepth case), 2 for PF_G16R16. Callers that need channels 2/3
    must check - decode_velocity_depth() does.
    """
    channels, bpp = velocity_channels(meta)
    w, h = meta["velocity_width"], meta["velocity_height"]
    rows = _rows(path, w, h, meta["velocity_row_pitch"], bpp)
    u16 = rows.view(np.uint16).reshape(h, w, channels)
    return u16.astype(np.float32) / 65535.0


# ---------------------------------------------------------------------------
# The SceneVelocity encoding.
#
# These are no longer fitted. They are transcribed from the encode/decode pair
# in Engine/Shaders/Private/Common.ush of the engine this title is built on
# (UE 5.7.1, on disk), and every constant below can be pointed at a line there.
# The previous fitted constants (-0.1758 / +0.1991 UV per UNORM unit) are gone;
# what they actually were is explained at the bottom of this comment, because
# the reason they looked plausible is the interesting part.
#
# EncodeVelocityToTexture(), Common.ush:2060 --
#
#     #if VELOCITY_ENCODE_GAMMA                          // 1 whenever SM5+
#         V.xy = sign(V.xy) * sqrt(abs(V.xy)) * (2.0 / sqrt(2.0));
#     #endif
#     EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f;
#
# So the stored value is a SQUARE-ROOT encoding of the clip-space delta, not a
# linear one. VELOCITY_ENCODE_GAMMA is `#define`d to 1 for every feature level
# at or above SM5 (Common.ush:238), i.e. unconditionally for a D3D12 PC title -
# there is no cvar and no per-project setting to check.
#
# The inverse, DecodeVelocityFromTexture() at Common.ush:2107:
#
#     V.xy = EncodedV.xy * InvDiv - 32767.0f/65535.0f * InvDiv;   // InvDiv = 1/(0.499*0.5)
#     #if VELOCITY_ENCODE_GAMMA
#         V.xy = (V.xy * abs(V.xy)) * 0.5;
#     #endif
#
# V is then a CLIP-space delta, current minus previous, with x right and y up.
# Two more engine facts fix the rest of the convention:
#
#   * `PrevScreenPos = ScreenPos - DecodeVelocityFromTexture(...).xy`
#     (PostProcessAmbientOcclusion.usf:1203, VisualizeMotionVectors.usf:78, and
#     every other consumer) - so V points forward, and stepping *back* to the
#     previous frame subtracts it.
#   * `ScreenPosToViewportUV(S) = float2(0.5 + 0.5*S.x, 0.5 - 0.5*S.y)`
#     (Common.ush:1658) - clip to UV flips Y.
#
# Composing those two gives the backward (current -> previous) UV displacement
# this module returns:
#
#     du = -0.5 * V.x        dv = +0.5 * V.y
#
# which is where the X-negative / Y-positive sign pattern comes from. It was
# guessed correctly before; it is now derived.
#
# Jitter: the encoded vector is already de-jittered. Calculate3DVelocityBase()
# (VelocityCommon.ush:9) subtracts View.TemporalAAJitter.xy from the current
# screen position and .zw from the previous one before differencing, so the
# TAA/TSR sub-pixel offset is not in the buffer and must not be removed again.
#
# Why the old fitted constants looked right: linearising the sqrt encoding
# around the dominant motion in the station capture (a train at ~11 px/frame
# across a 1212-wide velocity buffer) gives an apparent slope of
#     (11/1212) / (sqrt(2*11/1212) * sqrt(2) * 0.2495) = 0.191 UV per unit,
# against the -0.1758 / +0.1991 that were fitted. The fit was not wrong about
# its own data - it measured the local tangent of a curve and reported it as
# the curve. That is also the whole of the "~10x disagreement with Common.ush":
# 2.004 is the coefficient of the sqrt term, 0.19 is a chord across it, and the
# ratio between them is a function of how fast the scene happened to be moving.
ENCODE_ZERO = 32767.0 / 65535.0     # Common.ush:2069
ENCODE_SCALE = 0.499 * 0.5          # Common.ush:2069
ENCODE_GAMMA = True                 # Common.ush:238, SM5+ => always on here

# ---------------------------------------------------------------------------
# Engine version.
#
# The x/y encoding above is stable across the versions checked: UE 5.2 and
# UE 5.7.1 both guard VELOCITY_ENCODE_GAMMA on `FEATURE_LEVEL >= FEATURE_LEVEL_SM5`
# and both apply the identical `V.xy = (V.xy * abs(V.xy)) * 0.5` on decode. So
# the displacement decode ports between them unchanged.
#
# The DEPTH channels do NOT. 5.7.1 steals the bottom 1-2 bits of channel 3 to
# carry bHasPixelAnimation / temporal responsiveness:
#
#   5.7.1:  V.z = asfloat((z << 16) | (w & VELOCITY_Z_LOW_MASK))   // 0xFFFE or 0xFFFC
#   5.2:    V.z = asfloat((z << 16) |  w)                          // no mask at all
#
# On 5.2 those bits are ordinary float32 mantissa. Masking them off loses (a
# negligible amount of) precision, but *reporting bit 0 as a flag* would be
# presenting the low mantissa bit of a float as a semantic boolean - which is
# the exact mistake this project already made once, when channel 2's bimodality
# was written up as "a flag, not a velocity component" (it is the sign bit of
# this same float). Same error, one bit lower down. So the flag is only decoded
# where it is known to exist.
#
# Verified endpoints only. 5.2: no mask, no flag. 5.7.1: masked, flag present.
# The introduction point in between has NOT been checked - anything else warns
# rather than guessing, because a silently wrong boolean is worse than a noisy
# one.
#
# One caveat on the 5.7 entry, found while porting and worth stating because it
# is the same class of thing this table exists to prevent. VELOCITY_Z_LOW_MASK
# is not a constant even within 5.7.1 (Common.ush:2049-2055):
#
#     #if VELOCITY_ENCODE_TEMPORAL_RESPONSIVENESS
#         #define TEMPORAL_RESPONSIVENESS_MASK  0x3
#         #define VELOCITY_Z_LOW_MASK           0xFFFC     <- two bits stolen
#     #else
#         #define TEMPORAL_RESPONSIVENESS_MASK  0x1
#         #define VELOCITY_Z_LOW_MASK           0xFFFE     <- one bit stolen
#     #endif
#
# and VELOCITY_ENCODE_TEMPORAL_RESPONSIVENESS depends on
# VELOCITY_SUPPORT_TEMPORAL_RESPONSIVENESS, a per-shader-platform property we
# cannot read from a dump. 0xFFFE below is therefore the *narrower* of the two
# possibilities, chosen deliberately: if the title actually uses 0xFFFC, this
# leaves bit 1 in the mantissa, which perturbs V.z by ~1e-7 relative - nothing.
# Guessing the other way round would mask off a real mantissa bit AND report
# bit 1 as part of a flag it is not. Both bits are low mantissa either way, so
# V.z is safe; only bHasPixelAnimation (bit 0) is claimed, and bit 0 is stolen
# under both branches.
ENGINE_VERSION = (5, 7)

_VERIFIED_ENCODINGS = {
    # version: (velocity_z_low_mask, has_pixel_animation_bit)
    (5, 2): (0xFFFF, False),
    (5, 7): (0xFFFE, True),
}


def engine_version(meta=None):
    """The engine version to decode with: from the capture if it recorded one.

    ENGINE_VERSION is a module global, which is fine for one title at a time and
    silently wrong the moment two dumps from different engine versions are
    analysed in the same session - the second one gets whatever the first one
    set. The hook now writes engine_version_major/minor into meta.txt when
    MV_ENGINE_VERSION was set for the capture, so the dump carries its own
    answer and the global is only a fallback.
    """
    if meta and "engine_version_major" in meta and "engine_version_minor" in meta:
        return (meta["engine_version_major"], meta["engine_version_minor"])
    # Not in the capture. MV_ENGINE_VERSION lets the operator supply it after
    # the fact, which is not as good as recording it but is far better than the
    # alternative: falling back to a default that is wrong for the title, and
    # then reporting bHasPixelAnimation for an engine version that has no such
    # flag. That is exactly the error this whole mechanism exists to prevent -
    # reading semantics into bits of a value whose type has not been
    # established - and it happened here, silently, on the first third-party
    # capture, because the game was launched without the variable set.
    env = os.environ.get("MV_ENGINE_VERSION", "")
    if "." in env:
        try:
            major, minor = env.split(".", 1)
            return (int(major), int(minor.split(".")[0]))
        except ValueError:
            pass
    return ENGINE_VERSION


def depth_encoding(version=None):
    """(velocity_z_low_mask, has_pixel_animation_bit) for an engine version."""
    version = version or ENGINE_VERSION
    if version in _VERIFIED_ENCODINGS:
        return _VERIFIED_ENCODINGS[version]
    import warnings
    nearest = min(_VERIFIED_ENCODINGS, key=lambda v: abs(v[0] * 100 + v[1] - (version[0] * 100 + version[1])))
    warnings.warn(
        f"UE {version[0]}.{version[1]} velocity depth encoding has not been verified against "
        f"engine source; falling back to the UE {nearest[0]}.{nearest[1]} rules. Check "
        f"VELOCITY_Z_LOW_MASK in that version's Common.ush before trusting V.z or "
        f"bHasPixelAnimation.")
    return _VERIFIED_ENCODINGS[nearest]

# UE5 clamps the *encoded* channel to [0,1], so the representable clip-space
# delta is bounded. Recorded here because it is the ceiling on anything this
# module can report, not because anything divides by it.
MAX_CLIP_DELTA = (ENCODE_ZERO / ENCODE_SCALE) ** 2 * 0.5 * 2.0


def decode_velocity_clip(path, meta):
    """Decode SceneVelocity into the engine's own clip-space delta.

    Returns (h, w, 2) of `V.xy` exactly as DecodeVelocityFromTexture() would
    hand it to a shader: current-minus-previous, clip space, x right, y up.
    Use this to compare against engine code; use decode_velocity() to compare
    against images.
    """
    raw = load_velocity_raw(path, meta)
    v = (raw[:, :, :2] - ENCODE_ZERO) / ENCODE_SCALE
    if ENCODE_GAMMA:
        v = (v * np.abs(v)) * 0.5
    return v


def decode_velocity(path, meta):
    """Decode the velocity buffer into screen-space UV displacement.

    Returned value is the displacement from this frame's pixel back to where
    that surface was in the previous frame, in UV units (fraction of screen).
    Only meaningful where velocity_written_mask() is true.
    """
    v = decode_velocity_clip(path, meta)
    out = np.empty(v.shape, dtype=np.float32)
    out[:, :, 0] = -0.5 * v[:, :, 0]
    out[:, :, 1] = +0.5 * v[:, :, 1]
    return out


def decode_velocity_depth(path, meta, version=None):
    """Decode channels 2 and 3, which are not a colour and not a displacement.

    Under VELOCITY_ENCODE_DEPTH (Common.ush:2071) the encoder splits the
    float32 bit pattern of `V.z` - the DeviceZ delta, current minus previous -
    across the two spare 16-bit channels:

        EncodedV.z = (asuint(V.z) >> 16) & 0xFFFF
        EncodedV.w = (asuint(V.z)        & VELOCITY_Z_LOW_MASK) | ResponsivenessMask

    with the bottom 1-2 bits of w stolen for the temporal-responsiveness /
    bHasPixelAnimation flags. Reassembling the halves is the only way to read
    them: on their own, channel 2 looks bimodal and channel 3 looks like noise,
    which is exactly what the top and bottom halves of a clustered float32 do
    look like when you plot them as if they were colours.

    Returns (device_z_delta, has_pixel_animation). has_pixel_animation is None
    on engine versions that do not encode it - see depth_encoding().
    """
    channels, _ = velocity_channels(meta)
    if channels != 4:
        raise ValueError(
            f"velocity format {meta['velocity_format']} has no depth channels. UE only encodes "
            f"V.z when NeedVelocityDepth() is true (VelocityRendering.cpp), which is also what "
            f"selects the 4-channel PF_A16B16G16R16 format; a 2-channel PF_G16R16 capture has "
            f"no channels 2/3 to decode.")
    mask, has_anim_bit = depth_encoding(version or engine_version(meta))
    raw = load_velocity_raw(path, meta)
    hi = np.round(raw[:, :, 2] * 65535.0).astype(np.uint32)
    lo = np.round(raw[:, :, 3] * 65535.0).astype(np.uint32)
    bits = ((hi << 16) | (lo & mask)).astype(np.uint32)
    anim = (lo & 0x1).astype(bool) if has_anim_bit else None
    return bits.view(np.float32), anim


def velocity_written_mask(path, meta):
    """True where the velocity buffer actually holds written data.

    UE5 only writes SceneVelocity for pixels it considers to have non-trivial
    motion; everywhere else the buffer holds its cleared value (0), which
    decodes to a large bogus displacement rather than zero. Those pixels must
    be excluded, not trusted.

    The test is `EncodedV.x > 0`, which is the engine's own - see
    `bIsRenderedVelocity` in VisualizeMotionVectors.usf:73 and
    PostProcessAmbientOcclusion.usf:1200. Zero motion encodes to 32767, so 0 in
    channel 0 is unambiguously "never written"; the engine falls back to
    reconstructing camera motion from depth for those pixels, which is why they
    are absent rather than zero.
    """
    raw = load_velocity_raw(path, meta)
    return raw[:, :, 0] > 0.0


# ---------------------------------------------------------------------------
# Scene depth.
#
# UE5 creates scene depth as PF_DepthStencil, which the D3D12 RHI maps to a
# TYPELESS format (D3D12RHI.cpp:117/127) - R24G8_TYPELESS or R32G8X24_TYPELESS
# depending on the r.D3D12.UseD24 cvar. Both are MULTI-PLANE: depth is plane 0,
# stencil is plane 1, with different formats and different row pitches. The hook
# writes every plane's footprint into meta_depth.txt rather than describing only
# the first, so this does not have to assume which case it is looking at.
#
# The values are DeviceZ exactly as the shader would read them: UE5 uses a
# reversed-Z buffer, so 1.0 is the near plane and 0.0 is the far plane / sky.
# Nothing is linearised here - the reprojection in reproject.py wants DeviceZ,
# because View_ClipToPrevClip is a clip-space transform.
DEPTH_PLANE0_DECODERS = {
    39: "float32",   # R32_TYPELESS       - plane 0 of R32G8X24_TYPELESS
    40: "float32",   # D32_FLOAT
    41: "float32",   # R32_FLOAT
    44: "unorm24",   # R24G8_TYPELESS
    45: "unorm24",   # D24_UNORM_S8_UINT
    46: "unorm24",   # R24_UNORM_X8_TYPELESS - plane 0 of R24G8_TYPELESS
    55: "unorm16",   # D16_UNORM
    56: "unorm16",   # R16_UNORM
}


def has_depth(meta):
    return "depth_plane0_format" in meta


def load_depth(path, meta):
    """Decode the depth capture's plane 0 into DeviceZ as (h, w) float32."""
    if not has_depth(meta):
        raise ValueError(
            "this dump has no meta_depth.txt, so it was taken before depth capture existed or the "
            "hook never identified a depth buffer in that session - check the 'depth:' lines in "
            "mv_hook.log rather than assuming the copy failed")
    fmt = meta["depth_plane0_format"]
    if fmt not in DEPTH_PLANE0_DECODERS:
        raise ValueError(
            f"depth plane 0 format {fmt} is not one this tool knows how to read. Add it only after "
            f"checking what GetCopyableFootprints reports for the resource format in question - the "
            f"plane formats of a typeless depth resource are not the resource's own format.")
    kind = DEPTH_PLANE0_DECODERS[fmt]
    w = meta["depth_plane0_width"]
    h = meta["depth_plane0_height"]
    pitch = meta["depth_plane0_row_pitch"]
    offset = meta["depth_plane0_offset"]
    bpp = 2 if kind == "unorm16" else 4
    raw = np.fromfile(path, dtype=np.uint8)
    useful = w * bpp
    out = np.empty((h, useful), dtype=np.uint8)
    for y in range(h):
        start = offset + y * pitch
        out[y] = raw[start:start + useful]
    if kind == "float32":
        return out.view(np.float32).reshape(h, w)
    if kind == "unorm24":
        # Depth is in the low 24 bits; the top 8 are the X8 padding, NOT the
        # stencil (stencil lives in plane 1, in its own footprint).
        packed = out.view(np.uint32).reshape(h, w)
        return (packed & 0x00FFFFFF).astype(np.float32) / 16777215.0
    return out.view(np.uint16).reshape(h, w).astype(np.float32) / 65535.0


# ---------------------------------------------------------------------------
# The View uniform buffer.
#
# The hook copies the few most frequently bound root constant buffers of each
# frame, without deciding which one is the View uniform buffer. That decision is
# made here, from the buffer's own contents, because deciding it in-process
# would mean hardcoding a struct offset for one build of one game - the exact
# class of mistake the previous phase of this project was about.

def load_view_cb(dump_dir, index, meta):
    """The captured candidate constant buffers for one frame, as (n, stride) uint8."""
    path = os.path.join(dump_dir, f"viewcb_{index:05d}.bin")
    if not os.path.exists(path):
        return None
    stride = meta.get("viewcb_stride", 4096)
    raw = np.fromfile(path, dtype=np.uint8)
    return raw.reshape(-1, stride)


def find_size_and_inv_size(buf, min_dim=64, max_dim=16384, tolerance=1e-4):
    """Offsets of every float4 of the form (W, H, 1/W, 1/H) in a buffer.

    This is the anchor the whole View-buffer layout is derived from, and it is
    self-validating in a way that matching against an expected resolution is
    not: two of the four floats have to be the reciprocals of the other two, to
    within float precision. That is not a coincidence any other four bytes in a
    few-KB struct are going to produce.

    It matters that this does NOT test against the velocity texture's extent.
    View_ViewSizeAndInvSize is the view RECT, and UE rounds render targets up -
    on the third-party title the velocity texture is 1708x1068 against a
    1707x1067 back buffer - so requiring them to be equal would reject the
    correct answer. The size is read out rather than matched.

    UE's View uniform buffer contains more than one field with this shape
    (View_ViewSizeAndInvSize and View_BufferSizeAndInvSize at least), so this
    returns all of them in offset order; the first is the view rect.
    """
    words = buf[: (len(buf) // 4) * 4].view(np.float32)
    hits = []
    for i in range(0, len(words) - 3, 4):  # float4s are 16-byte aligned
        w, h, iw, ih = (float(x) for x in words[i:i + 4])
        if not (min_dim <= w <= max_dim and min_dim <= h <= max_dim):
            continue
        if iw <= 0.0 or ih <= 0.0:
            continue
        if abs(iw * w - 1.0) < tolerance and abs(ih * h - 1.0) < tolerance:
            hits.append((i * 4, (w, h, iw, ih)))
    return hits


# Byte distance from View_ClipToPrevClip to View_ViewSizeAndInvSize, read off
# the member ordering in Engine/Source/Runtime/Engine/Public/SceneView.h:
#
#     FMatrix44f ClipToPrevClip          64      <- what we want
#     FMatrix44f ClipToPrevClipWithAA    64
#     FVector4f  TemporalAAJitter        16
#     FVector4f  GlobalClippingPlane     16
#     FVector2f  FieldOfViewWideAngles    8
#     FVector2f  PrevFieldOfViewWideAngles 8
#     FVector4f  ViewRectMin             16
#     FVector4f  ViewSizeAndInvSize            <- the anchor
#
# 64+64+16+16+8+8+16 = 192. NOTE the version dependence: UE 5.7.1 inserts
# FirstPersonFieldOfViewWideAngles and PrevFirstPersonFieldOfViewWideAngles
# (two more FVector2f) between PrevFieldOfViewWideAngles and ViewRectMin, which
# makes the distance 208 there. The offsets are checked below rather than
# trusted, and both are tried.
VIEW_LAYOUTS = {
    # name: (clipToPrevClip, clipToPrevClipWithAA, temporalAAJitter, viewRectMin) relative to the anchor
    "UE 5.2-style": (-192, -128, -64, -16),
    "UE 5.7-style": (-208, -144, -80, -16),
}


def read_matrix(buf, offset):
    """A float4x4 from a byte offset, as (4, 4) with M[row][col].

    Row-major, matching both FMatrix44f's C++ memory layout and how the shader
    reads it: UE compiles all its HLSL with D3DCOMPILE_PACK_MATRIX_ROW_MAJOR
    (D3DShaderCompiler.cpp:1343, and `-Zpr` on the DXC path), so a float4x4 in a
    uniform buffer is packed row-major and `mul(v, M)` is a row-vector product
    over exactly these bytes. Without that flag HLSL packs matrices
    column-major, the shader would see the transpose, and every reprojection
    below would be wrong in a way that still looks like a plausible motion
    field - so it is worth being able to point at the line.
    """
    return buf[offset:offset + 64].view(np.float32).reshape(4, 4).astype(np.float64)


def derive_view_layout(buf, render_extent=None):
    """Work out where ClipToPrevClip is in a candidate buffer, or return None.

    `render_extent` is the captured velocity texture's (width, height). Passing
    it is strongly recommended and the reason is a real failure: a (W, H, 1/W,
    1/H) float4 is a distinctive shape but it is not unique even within UE's own
    View buffer, which also carries BufferSizeAndInvSize and several smaller
    ones for downsampled buffers. Worse, an unrelated constant buffer can
    contain one too - and when the top-ranked candidate slot happened not to be
    the View buffer, a (66, 64, 1/66, 1/64) float4 in it anchored the layout at
    a completely wrong offset, passed three of the four structural checks, and
    produced a plausible-looking matrix that regressed at slope 0.0015.

    Constraining the anchor to something near the render extent fixes that, and
    it is not a fitted constant: the extent comes from the capture's own
    meta.txt. UE rounds render targets up, so the view rect can be a pixel or
    two smaller than the texture - measured 1707x1044 against a 1708x1044
    velocity buffer - hence a tolerance rather than equality.

    Returns a dict with the derived offsets, the view rect, the matrices and a
    list of the consistency checks that were applied - so a caller can print
    the evidence rather than a verdict.
    """
    anchors = find_size_and_inv_size(buf)
    if render_extent is not None:
        tolerance = 8
        anchors = [a for a in anchors
                   if abs(a[1][0] - render_extent[0]) <= tolerance
                   and abs(a[1][1] - render_extent[1]) <= tolerance]
    if not anchors:
        return None
    anchor_offset, (vw, vh, _, _) = anchors[0]
    best = None
    for name, (clip_rel, clip_aa_rel, jitter_rel, rect_rel) in VIEW_LAYOUTS.items():
        clip_off = anchor_offset + clip_rel
        if clip_off < 0:
            continue
        clip = read_matrix(buf, clip_off)
        clip_aa = read_matrix(buf, anchor_offset + clip_aa_rel)
        jitter = buf[anchor_offset + jitter_rel:anchor_offset + jitter_rel + 16].view(np.float32).astype(np.float64)
        rect_min = buf[anchor_offset + rect_rel:anchor_offset + rect_rel + 16].view(np.float32).astype(np.float64)
        checks = []
        # 1. ClipToPrevClip and ClipToPrevClipWithAA are adjacent members that
        #    differ ONLY by the TAA jitter, so they must be close but not equal.
        #    This is the strongest structural test available and it does not
        #    involve the velocity data at all.
        spread = float(np.max(np.abs(clip - clip_aa)))
        checks.append(("ClipToPrevClip and ...WithAA differ, but only slightly",
                       0.0 < spread < 0.1, f"max|difference| = {spread:.6f}"))
        # 2. Every entry finite and O(1). A projective clip-to-clip transform for
        #    a plausible camera step has no huge entries.
        finite = bool(np.all(np.isfinite(clip))) and float(np.max(np.abs(clip))) < 100.0
        checks.append(("ClipToPrevClip is finite and O(1)", finite,
                       f"max|entry| = {float(np.max(np.abs(clip))):.4f}"))
        # 3. TemporalAAJitter is a sub-pixel offset in screen-position units.
        jitter_ok = bool(np.all(np.isfinite(jitter))) and float(np.max(np.abs(jitter))) < 0.05
        checks.append(("TemporalAAJitter is a sub-pixel offset", jitter_ok,
                       f"= ({jitter[0]:+.6f}, {jitter[1]:+.6f}, {jitter[2]:+.6f}, {jitter[3]:+.6f})"))
        # 4. ViewRectMin is a pixel offset: small, non-negative, integral.
        rect_ok = bool(np.all(rect_min >= 0) and np.all(rect_min < 8192) and
                       np.all(np.abs(rect_min - np.round(rect_min)) < 1e-3))
        checks.append(("ViewRectMin is a non-negative integer pixel offset", rect_ok,
                       f"= ({rect_min[0]:.1f}, {rect_min[1]:.1f})"))
        passed = sum(1 for _, ok, _ in checks if ok)
        candidate = {
            "layout": name,
            "anchor_offset": anchor_offset,
            "anchors": anchors,
            "clip_to_prev_clip_offset": clip_off,
            "clip_to_prev_clip": clip,
            "clip_to_prev_clip_with_aa": clip_aa,
            "temporal_aa_jitter": jitter,
            "view_rect_min": rect_min,
            "view_size": (vw, vh),
            "checks": checks,
            "checks_passed": passed,
        }
        if best is None or passed > best["checks_passed"]:
            best = candidate
    return best


def load_color(path, meta):
    """Returns the back buffer as float RGB (h, w, 3) in [0,1]."""
    w, h = meta["color_width"], meta["color_height"]
    fmt = meta["color_format"]
    if fmt == DXGI_R10G10B10A2_UNORM:
        rows = _rows(path, w, h, meta["color_row_pitch"], 4)
        packed = rows.view(np.uint32).reshape(h, w)
        r = (packed & 0x3FF).astype(np.float32) / 1023.0
        g = ((packed >> 10) & 0x3FF).astype(np.float32) / 1023.0
        b = ((packed >> 20) & 0x3FF).astype(np.float32) / 1023.0
        return np.stack([r, g, b], axis=2)
    if fmt in (DXGI_R8G8B8A8_UNORM, DXGI_B8G8R8A8_UNORM):
        rows = _rows(path, w, h, meta["color_row_pitch"], 4)
        px = rows.reshape(h, w, 4).astype(np.float32) / 255.0
        if fmt == DXGI_B8G8R8A8_UNORM:
            return px[:, :, [2, 1, 0]]
        return px[:, :, :3]
    raise ValueError(f"unhandled colour format {fmt}")


def to_display(color):
    """Back buffer -> displayable RGB.

    The presented back buffer is already display-encoded (the swapchain is what
    goes to the monitor), so no gamma is applied here. An earlier version
    applied **(1/2.2) on the assumption the data was linear, which double
    gamma-corrected it and visibly washed out an otherwise dark scene - the
    give-away was captures looking much brighter than the game does.
    """
    return np.clip(color, 0, 1).astype(np.float32)


def flow_to_rgb(flow_uv, meta, max_pixels=None, gamma=0.5, floor=0.18):
    """HSV colour-wheel visualisation: hue = direction, value = magnitude.

    A real scene spans a wide range of speeds at once - here a train crossing
    at ~11px/frame alongside a weapon viewmodel drifting at ~1px/frame. Mapping
    magnitude to brightness linearly crushes everything slow to black, so the
    ramp is gamma-corrected (sqrt by default) and written pixels get a small
    brightness floor. Direction (hue) stays untouched; only the brightness ramp
    is perceptual, so colours remain comparable between frames.
    """
    import cv2
    h, w = flow_uv.shape[:2]
    # Convert UV displacement to pixels so the scale is physically meaningful.
    fx = flow_uv[:, :, 0] * w
    fy = flow_uv[:, :, 1] * h
    mag = np.sqrt(fx * fx + fy * fy)
    ang = np.arctan2(fy, fx)
    if max_pixels is None:
        max_pixels = max(np.percentile(mag, 99), 1e-3)
    norm = np.clip(mag / max_pixels, 0, 1) ** gamma
    hsv = np.zeros((h, w, 3), dtype=np.float32)
    hsv[:, :, 0] = (ang + np.pi) / (2 * np.pi) * 179.0
    hsv[:, :, 1] = 255.0
    hsv[:, :, 2] = (floor + (1.0 - floor) * norm) * 255.0
    return cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)


def frame_indices(dump_dir):
    idx = []
    for name in os.listdir(dump_dir):
        if name.startswith("vel_") and name.endswith(".bin"):
            idx.append(int(name[4:-4]))
    return sorted(idx)


# How many consecutive frames one F8 burst captures; must match kCaptureFrames
# in hook/src/capture.cpp. Only used by the legacy fallback below.
BURST = 60


def load_frame_map(dump_dir):
    """capture index -> the game frame the capture came from, or None.

    The hook appends one `captureIndex,frameIndex` line per drained frame to
    frames.csv. Dumps taken before that existed have no such file.
    """
    path = os.path.join(dump_dir, "frames.csv")
    if not os.path.exists(path):
        return None
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            capture, frame = line.split(",")[:2]
            out[int(capture)] = int(frame)
    return out or None


def consecutive_pairs(dump_dir, indices=None):
    """(prev, cur) capture pairs that really are adjacent game frames.

    The whole warp test assumes frame N-1 and frame N are one frame apart. That
    was previously assumed rather than checked: capture indices are a counter
    of what we managed to drain, so a dropped frame, a slot that was busy, or a
    second F8 burst all produce numerically adjacent captures that are not
    temporally adjacent. With frames.csv present this is now verified against
    the game's own frame counter and non-adjacent pairs are dropped.

    Returns (pairs, note) where note describes which rule was applied, so
    callers can print it rather than silently implying the strong one.
    """
    if indices is None:
        indices = frame_indices(dump_dir)
    index_set = set(indices)
    frames = load_frame_map(dump_dir)
    if frames is None:
        # Legacy dumps: the best available proxy is "not on a burst boundary",
        # which catches the gap between two F8 presses and nothing else.
        pairs = [(i - 1, i) for i in indices if (i - 1) in index_set and i % BURST != 0]
        return pairs, f"adjacency assumed (no frames.csv; burst-boundary rule, BURST={BURST})"
    pairs = []
    dropped = 0
    for i in indices:
        if (i - 1) not in index_set:
            continue
        if i in frames and (i - 1) in frames and frames[i] - frames[i - 1] == 1:
            pairs.append((i - 1, i))
        else:
            dropped += 1
    return pairs, f"adjacency verified against frames.csv ({len(pairs)} adjacent, {dropped} rejected)"
