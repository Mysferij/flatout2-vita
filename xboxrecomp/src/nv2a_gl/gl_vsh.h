/* gl_vsh.h -- NV2A vertex transform as GLSL (see gl_vsh.c). */
#ifndef NV2A_GL_VSH_H
#define NV2A_GL_VSH_H

#include <stddef.h>
#include <stdint.h>

/* The body of a vertex program, as `void nv2a_program(void)`, from slot
 * `start` to the FINAL instruction. Returns bytes written, -1 on overflow. */
int nv2a_gl_vsh_program(const uint32_t (*prog)[4], uint32_t slots,
                        uint32_t start, char *buf, size_t cap);

/* Nonzero: emit Vulkan GLSL (nv2a_vk). Set once, before the first shader. */
extern int nv2a_shader_vk;
/* Nonzero (Vulkan): the device clamps depth per fragment (depthClamp), so
 * the vertex shader leaves clip z unclamped. Set before the first shader. */
extern int nv2a_shader_depth_clamp;

/* Declarations and helpers every vertex shader starts with. */
const char *nv2a_gl_vsh_prelude(void);
/* main() for a vertex-program shader (follows the program body). */
const char *nv2a_gl_vsh_main_program(void);
/* main() for fixed-function / pre-transformed batches (no program body). */
const char *nv2a_gl_vsh_fixed(void);

#if defined(__vita__)
/* GLSL ES (Vita): the attribute slots vN the vertex parts read (bit N). */
uint32_t nv2a_gl_vsh_es_used_attribs(const char *const *parts, int n);
/* GLSL ES (Vita): "attribute vec4 vN;" lines for the slots in mask. */
const char *nv2a_gl_vsh_es_attribs(uint32_t mask);
/* GLSL ES (Vita): the most attribute locations a program may bind (v0..v7). */
#define NV2A_ES_MAX_LOCS 8
/* GLSL ES (Vita): compacts the slots in mask to locations 0..n-1 (loc[N],
 * 0xFF = unread). Returns n, or -1 when more than NV2A_ES_MAX_LOCS are read. */
int nv2a_gl_vsh_es_locs(uint32_t mask, uint8_t loc[16]);
/* GLSL ES (Vita): copies a shader part renaming vN (N in mask) to vloc[N].
 * Returns the length written, or -1 if it does not fit in cap. */
long nv2a_gl_vsh_es_rename(const char *src, char *dst, size_t cap,
                           const uint8_t loc[16], uint32_t mask);
/* GLSL ES (Vita): bit i set = varying i (vD0 vD1 vT0 vT1 vT2 vT3 vFog) is
 * read by the fragment body fsrc (only the text after "void main()"). */
uint32_t nv2a_gl_es_varying_mask(const char *fsrc);
/* GLSL ES (Vita): the fragment declarations ("varying ...;" lines) for mask. */
const char *nv2a_gl_es_varying_text(uint32_t mask);
/* GLSL ES (Vita): the c[] rows the program at prog[start..slots) reads or
 * writes (1..192); c is declared with this many rows, and uploaded as many. */
uint32_t nv2a_gl_vsh_const_rows(const uint32_t (*prog)[4], uint32_t slots,
                                uint32_t start);
/* GLSL ES (Vita): the vertex prelude for a program whose vertex parts are
 * parts[0..n) and whose fragment source is fsrc: declares what the parts
 * reference (c[rows]) and the varyings in nv2a_gl_es_varying_mask(fsrc).
 * Static buffer; NULL if it does not fit. */
const char *nv2a_gl_vsh_es_prelude(const char *const *parts, int n,
                                   const char *fsrc, uint32_t rows);
#endif

#endif
