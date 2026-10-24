// betterframe-xp: one pass = deband (haasn, MIT) + CAS sharpen (AMD, MIT) + center weighting.
// Modified for Steam Frame (side-by-side eyes, 4320x2160 swapchain of vrcompositor).
// See ../../THIRD_PARTY.md for the original licenses.
#version 450

// Colour maths runs in half precision (RelaxedPrecision) - about twice the ALU rate on Adreno.
// Coordinates, hashes and the seed stay highp: fp16 cannot address a 4320 px wide texture.
precision mediump float;

layout(set = 0, binding = 0) uniform sampler2D img;

// Live parameters (rewritten by the layer when params.conf changes).
layout(set = 1, binding = 0) uniform Params
{
    highp vec2  texel;    // 1 / swapchain size
    float sharpness;      // CAS 0..1
    float sharpenAmount;  // 0 = off, 1 = full CAS
    float debandAmount;   // 0 = off, 1 = full
    float debandRange;    // pixels
    float debandThreshold;// in 8-bit steps
    float centerRadius;   // 0..1 (from eye center). Full strength inside
    float edgeAmount;     // strength multiplier at the edge of each eye
    float splitEyes;      // 1 = left/right halves are separate eyes
    float showSplit;      // 1 = only the right half of each eye is processed (A/B)
    float detailBoost;    // 0 = plain CAS, 1 = sharpen only textured areas (grass) x2
    float grainAmount;    // 0 = off, 1 = strongest luma grain
    float grainSize;      // grain cell size in pixels (1..4)
    float grainAnimate;   // 1 = new grain every frame, 0 = static
    float casCross;       // 1 = CAS reads the cross only (5 taps), 0 = full 3x3 (9 taps)
    float casLuma;        // 1 = CAS on luma (one weight for all channels), 0 = per colour channel
    float cornerSkip;     // eye radius beyond which pixels are left alone, 0 = off
    float contrast;       // 1 = unchanged (pivot 0.5)
    float saturation;     // 1 = unchanged, 0 = greyscale
    float gamma;          // 1 = unchanged, > 1 brightens mid-tones
    float temperature;    // 1 = unchanged, > 1 warmer (keeps the overall luma)
    float brightness;     // 1 = unchanged
    highp float frameSeed;// written by the layer every frame (0..63)
} p;

layout(location = 0) in highp vec2 textureCoord;
layout(location = 0) out vec4 fragColor;

#define tex0(c) textureLod(img, c, 0.0)
#define tex0o(c, o) textureLodOffset(img, c, 0.0, o)

// Float-only hash: no sin() and no integer ops. Cheaper than the integer hash3 it replaced.
highp float hash12(highp vec2 v)
{
    highp vec3 q = fract(vec3(v.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

const vec3 kLuma = vec3(0.299, 0.587, 0.114);

// Returns the CAS change; also gives the contrast (max - min of luma) for the detail-aware
// weight, the grain mask and the deband gate.
// casCross = 1: min / max over the cross only (5 taps). casCross = 0: the corners are read too
// (9 taps, AMD CAS sum form). The weighted sum always uses the cross.
// casLuma = 1: every tap is turned into its luma first, so one weight serves all channels.
// The taps use constant offsets (textureLodOffset needs them); the corners are read only when used.
vec3 casDelta(highp vec2 uv, vec3 e, out float contrast)
{
    vec3 b = tex0o(uv, ivec2( 0, -1)).rgb;
    vec3 d = tex0o(uv, ivec2(-1,  0)).rgb;
    vec3 f = tex0o(uv, ivec2( 1,  0)).rgb;
    vec3 h = tex0o(uv, ivec2( 0,  1)).rgb;
    vec3 a = e, c = e, g = e, i = e;
    if (p.casCross < 0.5)
    {
        a = tex0o(uv, ivec2(-1, -1)).rgb;
        c = tex0o(uv, ivec2( 1, -1)).rgb;
        g = tex0o(uv, ivec2(-1,  1)).rgb;
        i = tex0o(uv, ivec2( 1,  1)).rgb;
    }
    if (p.casLuma > 0.5)
    {
        b = vec3(dot(b, kLuma)); d = vec3(dot(d, kLuma));
        f = vec3(dot(f, kLuma)); h = vec3(dot(h, kLuma));
        a = vec3(dot(a, kLuma)); c = vec3(dot(c, kLuma));
        g = vec3(dot(g, kLuma)); i = vec3(dot(i, kLuma));
    }
    vec3 ec = p.casLuma > 0.5 ? vec3(dot(e, kLuma)) : e;

    vec3 mn = min(min(min(d, ec), min(f, b)), h);
    vec3 mx = max(max(max(d, ec), max(f, b)), h);

    if (p.casCross > 0.5)
    {
        contrast = max(dot(mx - mn, kLuma), 0.0);
    }
    else
    {
        mn += min(min(min(mn, a), min(g, c)), i);
        mx += max(max(max(mx, a), max(g, c)), i);
        // mn / mx are (cross + 3x3) sums, so halve them to get the 3x3 range.
        contrast = max(dot(mx - mn, kLuma) * 0.5, 0.0);
    }

    vec3 amp  = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);
    amp       = inversesqrt(max(amp, vec3(1e-4)));
    float peak = 8.0 - 3.0 * p.sharpness;
    vec3 w    = -vec3(1.0) / (amp * peak);
    vec3 outc = clamp(((b + d + f + h) * w + ec) / (1.0 + 4.0 * w), 0.0, 1.0);
    return outc - ec;
}

vec3 deband(highp vec2 uv, highp vec2 px, vec3 ori)
{
    highp float ang  = hash12(px) * 6.2831853;
    highp vec2  dir  = vec2(cos(ang), sin(ang));
    highp float dist = (0.5 + 0.5 * hash12(px + vec2(37.0, 11.0))) * p.debandRange;
    highp vec2  o    = dir * dist * p.texel;

    vec3 r0 = tex0(uv + o).rgb;
    vec3 r1 = tex0(uv - o).rgb;
    vec3 r2 = tex0(uv + vec2(-o.y, o.x)).rgb;
    vec3 r3 = tex0(uv + vec2(o.y, -o.x)).rgb;
    vec3 avg = (r0 + r1 + r2 + r3) * 0.25;

    vec3 maxDiff = max(max(abs(r0 - ori), abs(r1 - ori)), max(abs(r2 - ori), abs(r3 - ori)));
    float th = p.debandThreshold / 255.0;
    // Only flat areas (all 4 references within threshold) are smoothed; edges stay.
    vec3 k = clamp(1.0 - maxDiff / max(th, 1e-4), 0.0, 1.0);
    vec3 res = mix(ori, avg, k);

    // Triangular dither so the 8-bit output does not form new bands.
    float n = hash12(px + vec2(13.0, 71.0)) - hash12(px + vec2(59.0, 29.0));
    return res + vec3(n / 255.0) * max(max(k.r, k.g), k.b);
}

// Luma grain in -1..1 (triangular), one value per grainSize x grainSize cell.
float grain(highp vec2 px)
{
    highp vec2 cell = floor(px / max(p.grainSize, 1.0));
    // A different offset each frame gives a new pattern (64 patterns, then it repeats).
    if (p.grainAnimate > 0.5)
        cell += vec2(97.0, 61.0) * p.frameSeed;
    return hash12(cell) + hash12(cell + vec2(47.0, 17.0)) - 1.0;
}

// Colours are graded after the detail work: contrast around mid grey, saturation against luma,
// gamma, white balance, then brightness. Every neutral value (1.0) makes the step an exact no-op,
// and colorActive() lets the shader skip the two pow() calls entirely when nothing is set.
bool colorActive()
{
    return p.contrast != 1.0 || p.saturation != 1.0 || p.gamma != 1.0
        || p.temperature != 1.0 || p.brightness != 1.0;
}

vec3 grade(vec3 c)
{
    c = (clamp(c, 0.0, 1.0) - 0.5) * p.contrast + 0.5;
    c = mix(vec3(dot(c, kLuma)), c, p.saturation);
    c = pow(max(c, vec3(0.0)), vec3(1.0 / p.gamma));
    // White balance as RGB gains normalised by their luma, so warm/cool does not change the
    // overall brightness (otherwise the temperature slider would double as a brightness slider).
    vec3 wb = vec3(p.temperature, 1.0, 1.0 / p.temperature);
    c *= wb / max(dot(wb, kLuma), 1e-4);
    return clamp(c * p.brightness, 0.0, 1.0);
}

void main()
{
    highp vec2 uv  = textureCoord;
    highp vec2 px  = gl_FragCoord.xy;
    vec4 src = tex0(uv);
    vec3 col = src.rgb;

    // Position inside the current eye (0..1).
    highp vec2 eyeUv = uv;
    if (p.splitEyes > 0.5)
        eyeUv.x = fract(uv.x * 2.0);

    if (p.showSplit > 0.5)
    {
        // A divider line in the middle of each eye: 3 px white with a 3 px black border on each side,
        // so the boundary is visible on bright and dark scenes alike. Left of it: untouched source
        // (no sharpen, no deband, no grain, no colour grade), right: everything, so the split compare
        // shows the full difference including the colour grade.
        // One pixel is 2 * texel.x in eyeUv.x when the eyes share one image, 1 * texel.x otherwise.
        highp float pixelInEye = p.texel.x * (p.splitEyes > 0.5 ? 2.0 : 1.0);
        highp float linePx     = abs(eyeUv.x - 0.5) / pixelInEye;
        if (linePx < 1.5)
        {
            fragColor = vec4(1.0, 1.0, 1.0, src.a);
            return;
        }
        if (linePx < 4.5)
        {
            fragColor = vec4(0.0, 0.0, 0.0, src.a);
            return;
        }
        if (eyeUv.x < 0.5)
        {
            fragColor = src;
            return;
        }
    }

    float r = length((eyeUv - 0.5) * 2.0);
    // min(): smoothstep is undefined when both edges are equal (centerRadius = 1).
    float weight = mix(1.0, p.edgeAmount, smoothstep(min(p.centerRadius, 0.95), 1.0, r));

    // Corners of each eye (beyond cornerSkip, e.g. the inscribed circle at 1.15): the effects are
    // weak there and the reads are skipped. The colour grade still applies (it reads nothing), so
    // there is no visible ring where the corrected area ends.
    if (p.cornerSkip > 0.0 && r > p.cornerSkip)
    {
        fragColor = vec4(colorActive() ? grade(src.rgb) : src.rgb, src.a);
        return;
    }

    // One cross read gives the contrast used by deband, the detail mask and grain.
    float contrast = 0.0;
    vec3  cas      = vec3(0.0);
    if (p.sharpenAmount > 0.0 || p.grainAmount > 0.0 || p.debandAmount > 0.0)
        cas = casDelta(uv, src.rgb, contrast);

    // Heuristic gate: where the 3x3 contrast is already well above the threshold (edges, texture),
    // the far taps of deband almost never pass it, so they are skipped. Gradients still get smoothed.
    if (p.debandAmount > 0.0 && contrast < 3.0 * p.debandThreshold / 255.0)
        col = mix(col, deband(uv, px, src.rgb), clamp(p.debandAmount * weight, 0.0, 1.0));

    if (p.sharpenAmount > 0.0)
    {
        // Texture mask: 1 for faint detail left by the encoder (grass, ground),
        // 0 for flat areas (noise) and strong edges (ringing).
        float texMask = smoothstep(0.006, 0.03, contrast) * (1.0 - smoothstep(0.12, 0.30, contrast));
        float detail  = mix(1.0, 0.25 + 1.75 * texMask, p.detailBoost);
        col += cas * (p.sharpenAmount * weight * detail);
    }

    if (p.grainAmount > 0.0)
    {
        // Strongest in mid-tones and smooth-but-not-flat areas; none on strong edges.
        float luma  = dot(clamp(col, 0.0, 1.0), vec3(0.299, 0.587, 0.114));
        float tone  = clamp(4.0 * luma * (1.0 - luma), 0.0, 1.0);
        float area  = 1.0 - smoothstep(0.08, 0.25, contrast);
        col += vec3(grain(px) * 0.04 * p.grainAmount * tone * area * weight);
    }

    col = clamp(col, 0.0, 1.0);
    if (colorActive())
        col = grade(col);

    fragColor = vec4(col, src.a);
}
