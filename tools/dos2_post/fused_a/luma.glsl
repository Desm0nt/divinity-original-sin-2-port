// NB-18b-8 (route A, bundle F1A): the luma proxy FXAA's analysis taps use on the pre-LUT HDR image (R11G11B10F, linear light).
// The console's FXAA (ps 691768aa) searches edges on G of the post-LUT, post-Sight RGBA8 image with RELATIVE thresholds only
// (edge when max - min >= max / 6; the absolute minimum is 0), so a proxy needs the display curve's local contrast, not its scale:
//   LUMA 1  g                  linear light (bundle F1A, the default): the most sensitive - on four stand-in tone curves it lost < 1 %
//                              of the edges the console anti-aliases (tools/dos2_post_fuse.py eq_dec / eq_luts); extra edges cost a
//                              second LUT, and their colours are mixed after the LUT, so two near-white highlights stay as they are
//   LUMA 0  sqrt(g)            gamma 2 (bundle F1AS): the mid-tone slope of a display curve; loses 4..14 % of the console's edges
//   LUMA 2  sqrt(g / (g + 1))  gamma 2 with a shoulder (bundle F1AR): fewest extra edges, loses up to 23 %
// PQ(g) (the LUT's input coding) is NOT offered: its slope is 0.17..0.25 of a decade against ~0.45 of the display, it misses edges.
#ifndef LUMA
#define LUMA 1
#endif
#if LUMA == 2
#define PROXY(x) sqrt(max(x, 0.0) / (max(x, 0.0) + 1.0))
#elif LUMA == 1
#define PROXY(x) (x)
#else
#define PROXY(x) sqrt(max(x, 0.0))
#endif
