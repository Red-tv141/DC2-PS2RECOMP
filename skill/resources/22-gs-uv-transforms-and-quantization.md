# GS 12.4 Sprite UV Quantization & FBO Coordinate Inversion

> **Core Law**: A coordinate transform does NOT transform the quantization rules expressed in that coordinate. Flipping a coordinate space (`tq = 1 - tq`) inverts tie-break rules, bias nudges, and truncations. Exact restoration requires evaluating the quantization rule in the ORIGINAL coordinate space and mapping the result back.

---

## 1. The G406/G407 GS 12.4 Rounding Model

On the PlayStation 2 GS, texture coordinates for sprites are fixed-point 12.4 numbers. The hardware rounds these coordinates relative to the screen pixel grid to avoid seam artifacts and sub-pixel jitter.

```glsl
float g406RoundAxis(float value, int pixel, int origin, int flags) {
    bool legacy = (flags & 4) != 0;
    bool halfBoundary = legacy || uLinear != 0;
    float boundary = halfBoundary ? floor(value * 2.0 + 0.5) * 0.5 : floor(value + 0.5);
    float threshold = legacy ? 0.25 : (1.0 / 64.0);
    if (abs(value - boundary) > threshold) return value;
    bool down = ((flags & 2) != 0) && pixel != origin;
    bool up   = ((flags & 1) != 0) || (((flags & 2) != 0) && pixel == origin);
    float downNudge = legacy ? 0.25 : (1.0 / 16.0);
    float upNudge   = legacy ? 0.25 : 0.0;
    return down ? boundary - downNudge : (up ? boundary + upNudge : value);
}
```

### 1.1 Meaning of Rounding Variables
- `pixel` and `origin` are **GS Screen Coordinates** (`py` derived from `gl_FragCoord` back to GS screen space).
- `value` is the **GS Texel Coordinate** ($V$).
- `flags & 2` means: *"Bias DOWN except on the origin row, where it biases UP."*

---

## 2. The FBO V-Flip Defect (Phase G623/G624)

When sampling from a direct producer FBO (`srcFbp`), the OpenGL texture coordinate $T$ is vertically flipped relative to native GS VRAM:
$$t_q = 1.0 - t_q$$

### 2.1 Why Naive Repairs Fail
1. **Flipping `uvOriginY` (`uvOriginY' = fbH - 1 - uvOriginY`)**: `uvOriginY` is a **screen** coordinate, not a texel coordinate. Screen space is not flipped; only the texture coordinate is flipped. Inverting `uvOriginY` breaks a term that was already correct.
2. **Swapping Bias Bits**: `flags & 2` cannot be mirrored by bit swapping because bits 1 and 2 cannot express *"bias UP except on the origin row, where it biases DOWN"*. Furthermore, `floor(v + 0.5)` round-half-up mathematically becomes round-half-down when negated.

### 2.2 The Exact Mathematical Restoration
Undo the flip, evaluate the identical GS rounding rule on the unflipped coordinate, and re-flip the result:

```glsl
bool  flipV = uUvFlipV != 0;
float flipH = float(textureSize(uTex, 0).y);
float vIn   = flipV ? (flipH - uvTex.y) : uvTex.y;
vec2  rounded = vec2(g406RoundAxis(uvTex.x, px, uUvOriginX, uUvRoundU),
                     g406RoundAxis(vIn,     py, uUvOriginY, uUvRoundV));
if (uLinear != 0) rounded = trunc(rounded * 16.0) * (1.0 / 16.0);
if (flipV) rounded.y = flipH - rounded.y;
```

---

## 3. Shader Uniform and Architecture Constraints

### 3.1 Mode Bit vs Real Uniform
- Do not place shader flip flags in high bits of `uMode` if multidraw optimizations (e.g. G560) mask `uMode` (e.g. `#define uMode (vG560State.x & 65535)`).
- Route `uUvFlipV` as a dedicated uniform through the shadow state cache (`kG496UFlipV = 9`), writing only when `mode & 16384` is active.

### 3.2 Blast Radius Verification
- Never assume a shader feature is only used by a single draw call based on source reading alone.
- In Phase G624, source reading suggested only 1 draw used `srcFbp` rounding, but census instrumentation (`[G624:uvflip]`) revealed **2.2 million resident `srcFbp` binds per run** carrying rounding flags (~489/frame). Always verify blast radius with empirical censuses.
