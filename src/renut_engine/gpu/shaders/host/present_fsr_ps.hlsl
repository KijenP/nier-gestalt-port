// NieR (2026-11-13): present_ps with AMD FidelityFX Super Resolution 1's EASU upscaler (Edge Adaptive Spatial
// Upsampling, FidelityFX FSR 1.0, MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.) in place of the
// bilinear sample, for a frame drawn smaller than the screen (render_scale_percent below 100). Its 12 taps find the
// edge's direction and length at each output pixel and weight a Lanczos-like lobe along it; the result is clamped
// to the 2x2 around it (no ringing). Then a light sharpening, also held inside that 2x2's range, in place of the
// RCAS pass (which would need an output-sized target of its own).
// Same push as present_ps: param0 = the gamma exponent, param1's bits = the gamma ramp's slot (0: none).
#include "utility.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 Fetch(Texture2D<float4> tex, int2 p, int2 last)
{
    return tex.Load(int3(clamp(p, int2(0, 0), last), 0)).rgb;
}

float Luma(float3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);  // as FSR's EASU: green-weighted, cheap
}

// One of the four bilinear corners' contribution to the edge direction and length (FsrEasuSetF).
void EasuSet(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    // Horizontal: B (left), C (centre), D (right).
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    // Vertical: A (above), C, E (below).
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

// One tap of the lobe (FsrEasuTapF).
void EasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len2, float lob, float clp, float3 c)
{
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len2;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

float4 main(PSInput input) : SV_Target0
{
    Texture2D<float4> tex = g_Textures[g_Push.textureIndex];
    uint tw, th;
    tex.GetDimensions(tw, th);
    const int2 last = int2(int(tw) - 1, int(th) - 1);
    // The output pixel's centre in the source's pixels, less half a pixel: f is the texel at its top left.
    float2 pp = input.uv * float2(tw, th) - 0.5;
    const float2 fp = floor(pp);
    pp -= fp;
    const int2 p = int2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    const float3 bC = Fetch(tex, p + int2(0, -1), last), cC = Fetch(tex, p + int2(1, -1), last);
    const float3 eC = Fetch(tex, p + int2(-1, 0), last), fC = Fetch(tex, p, last);
    const float3 gC = Fetch(tex, p + int2(1, 0), last), hC = Fetch(tex, p + int2(2, 0), last);
    const float3 iC = Fetch(tex, p + int2(-1, 1), last), jC = Fetch(tex, p + int2(0, 1), last);
    const float3 kC = Fetch(tex, p + int2(1, 1), last), lC = Fetch(tex, p + int2(2, 1), last);
    const float3 nC = Fetch(tex, p + int2(0, 2), last), oC = Fetch(tex, p + int2(1, 2), last);
    const float bL = Luma(bC), cL = Luma(cC), eL = Luma(eC), fL = Luma(fC), gL = Luma(gC), hL = Luma(hC);
    const float iL = Luma(iC), jL = Luma(jC), kL = Luma(kC), lL = Luma(lC), nL = Luma(nC), oL = Luma(oC);

    // The edge: its direction and how long it is, bilinearly over the four centre texels.
    float2 dir = 0.0;
    float len = 0.0;
    EasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    EasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    EasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    EasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    const float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    const bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : rsqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    const float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    const float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    const float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    const float clp = 1.0 / lob;

    float3 aC = 0.0;
    float aW = 0.0;
    EasuTap(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    EasuTap(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    EasuTap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);
    EasuTap(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);
    EasuTap(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);
    EasuTap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);
    EasuTap(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);
    EasuTap(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);
    EasuTap(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);
    EasuTap(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);
    EasuTap(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);
    EasuTap(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);
    const float3 min4 = min(min(fC, gC), min(jC, kC));
    const float3 max4 = max(max(fC, gC), max(jC, kC));
    float3 color = min(max4, max(min4, aC / max(aW, 1e-5)));

    // Light sharpening: away from the 2x2's bilinear blend, by how much room the range leaves (as CAS / RCAS keep
    // it inside the neighbourhood), then clamped to it.
    const float3 blend = lerp(lerp(fC, gC, pp.x), lerp(jC, kC, pp.x), pp.y);
    const float3 room = min(min4, 1.0 - max4) / max(max4, 1e-3);
    const float3 amount = 0.35 * sqrt(saturate(room));
    color = clamp(color + (color - blend) * amount, min4, max4);

    // As present_ps: the game's gamma ramp, then the output gamma.
    const uint ramp = asuint(g_Push.param1);
    if (ramp != 0u)
    {
        const uint3 i = uint3(saturate(color) * 255.0 + 0.5);
        color = float3(g_Textures[ramp].Load(int3(i.x, 0, 0)).x,
                       g_Textures[ramp].Load(int3(i.y, 0, 0)).y,
                       g_Textures[ramp].Load(int3(i.z, 0, 0)).z);
    }
    if (g_Push.param0 != 1.0)
        color = pow(saturate(color), g_Push.param0);
    return float4(color, 1.0);
}
