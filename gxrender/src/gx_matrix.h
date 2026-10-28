/* gx_matrix.h — DS geometry-engine matrix stack (4x4, 1.19.12 fixed point).
 *
 * Semantics copied from melonDS src/GPU3D.cpp (MatrixLoadIdentity / MatrixLoad*
 * / MatrixMult* / MatrixScale / MatrixTranslate).  Matrices are stored flat in
 * column-major order: m[4*c + r] = M(r, c); the vertex transform is clip = M*v.
 */
#ifndef GX_MATRIX_H
#define GX_MATRIX_H

#include <stdint.h>

void gx_mat_identity(int32_t *m);
void gx_mat_load4x4(int32_t *m, const int32_t *s);   /* 16 params */
void gx_mat_load4x3(int32_t *m, const int32_t *s);   /* 12 params */
void gx_mat_mult4x4(int32_t *m, const int32_t *s);   /* m <- m*s */
void gx_mat_mult4x3(int32_t *m, const int32_t *s);   /* m <- m*s */
void gx_mat_mult3x3(int32_t *m, const int32_t *s);   /* m <- m*s (linear part) */
void gx_mat_scale(int32_t *m, const int32_t *s);     /* scale rows by s[0..2] */
void gx_mat_translate(int32_t *m, const int32_t *s); /* m <- m*T(s) */

#endif /* GX_MATRIX_H */
