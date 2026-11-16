// Shared declarations of the DOS2 mobile post replacements (tools/dos2_post_mobile.py). The support buffer layout is the translator's
// (1392 bytes; _m5[i] = render scale of texture i, _m5[0] = of the framebuffer; the host writes 1.0 everywhere, xbg_support_defaults).
#define SUPPORT_BUFFER(b) layout(set = 0, binding = b, std140) uniform support_buffer \
    { uint _m0; uint _m1[8]; vec4 _m2; vec4 _m3; int _m4; float _m5[73]; ivec4 _m6; int _m7; } sb;
#define CBUF(b, name) layout(set = 0, binding = b, std140) uniform fp_##name { vec4 _m0[4096]; } name;

#ifdef NEED_RS
// texelFetch coordinate with the translation's render-scale fix-up (fc = the matching gl_FragCoord component)
int rs_fix(int c, float s, float fc)
{
    if (s == 1.0) return c;
    if (s < 0.0) return int(fma(float(c), -s, mod(fc, -s)));
    return int(float(c) * s);
}
ivec2 rs_fix2(ivec2 c, float s) { return ivec2(rs_fix(c.x, s, gl_FragCoord.x), rs_fix(c.y, s, gl_FragCoord.y)); }
#endif

// the translation's NaN guard on sample coordinates
vec2 nanz(vec2 v) { return mix(v, vec2(0.0), isnan(v)); }
