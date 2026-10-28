/* gx_matrix.c — fixed-point matrix ops, arithmetic identical to melonDS
 * src/GPU3D.cpp (MatrixLoadIdentity/Load4x4/Load4x3/Mult4x4/Mult4x3/Mult3x3/
 * Scale/Translate).  melonDS is GPLv2; this file reimplements the documented
 * hardware arithmetic (integer ops on 1.19.12 values) and is our own code.
 *
 * Layout: m[4*c + r] = M(r, c), clip = M * v.
 * All products use int64 then arithmetic-shift >>12 (truncation toward -inf),
 * matching the hardware pipeline's fixed-point multiply.
 */
#include <string.h>
#include "gx_matrix.h"

void gx_mat_identity(int32_t *m)
{
    m[0]  = 0x1000; m[1]  = 0; m[2]  = 0; m[3]  = 0;
    m[4]  = 0; m[5]  = 0x1000; m[6]  = 0; m[7]  = 0;
    m[8]  = 0; m[9]  = 0; m[10] = 0x1000; m[11] = 0;
    m[12] = 0; m[13] = 0; m[14] = 0; m[15] = 0x1000;
}

void gx_mat_load4x4(int32_t *m, const int32_t *s)
{
    memcpy(m, s, 16 * sizeof(int32_t));
}

void gx_mat_load4x3(int32_t *m, const int32_t *s)
{
    m[0] = s[0];  m[1] = s[1];  m[2] = s[2];   m[3] = 0;
    m[4] = s[3];  m[5] = s[4];  m[6] = s[5];   m[7] = 0;
    m[8] = s[6];  m[9] = s[7];  m[10] = s[8];  m[11] = 0;
    m[12] = s[9]; m[13] = s[10]; m[14] = s[11]; m[15] = 0x1000;
}

void gx_mat_mult4x4(int32_t *m, const int32_t *s)
{
    int32_t tmp[16];
    memcpy(tmp, m, 16 * sizeof(int32_t));

    m[0]  = (int32_t)(((int64_t)s[0] * tmp[0]  + (int64_t)s[1] * tmp[4]  + (int64_t)s[2] * tmp[8]  + (int64_t)s[3] * tmp[12]) >> 12);
    m[1]  = (int32_t)(((int64_t)s[0] * tmp[1]  + (int64_t)s[1] * tmp[5]  + (int64_t)s[2] * tmp[9]  + (int64_t)s[3] * tmp[13]) >> 12);
    m[2]  = (int32_t)(((int64_t)s[0] * tmp[2]  + (int64_t)s[1] * tmp[6]  + (int64_t)s[2] * tmp[10] + (int64_t)s[3] * tmp[14]) >> 12);
    m[3]  = (int32_t)(((int64_t)s[0] * tmp[3]  + (int64_t)s[1] * tmp[7]  + (int64_t)s[2] * tmp[11] + (int64_t)s[3] * tmp[15]) >> 12);

    m[4]  = (int32_t)(((int64_t)s[4] * tmp[0]  + (int64_t)s[5] * tmp[4]  + (int64_t)s[6] * tmp[8]  + (int64_t)s[7] * tmp[12]) >> 12);
    m[5]  = (int32_t)(((int64_t)s[4] * tmp[1]  + (int64_t)s[5] * tmp[5]  + (int64_t)s[6] * tmp[9]  + (int64_t)s[7] * tmp[13]) >> 12);
    m[6]  = (int32_t)(((int64_t)s[4] * tmp[2]  + (int64_t)s[5] * tmp[6]  + (int64_t)s[6] * tmp[10] + (int64_t)s[7] * tmp[14]) >> 12);
    m[7]  = (int32_t)(((int64_t)s[4] * tmp[3]  + (int64_t)s[5] * tmp[7]  + (int64_t)s[6] * tmp[11] + (int64_t)s[7] * tmp[15]) >> 12);

    m[8]  = (int32_t)(((int64_t)s[8] * tmp[0]  + (int64_t)s[9] * tmp[4]  + (int64_t)s[10] * tmp[8]  + (int64_t)s[11] * tmp[12]) >> 12);
    m[9]  = (int32_t)(((int64_t)s[8] * tmp[1]  + (int64_t)s[9] * tmp[5]  + (int64_t)s[10] * tmp[9]  + (int64_t)s[11] * tmp[13]) >> 12);
    m[10] = (int32_t)(((int64_t)s[8] * tmp[2]  + (int64_t)s[9] * tmp[6]  + (int64_t)s[10] * tmp[10] + (int64_t)s[11] * tmp[14]) >> 12);
    m[11] = (int32_t)(((int64_t)s[8] * tmp[3]  + (int64_t)s[9] * tmp[7]  + (int64_t)s[10] * tmp[11] + (int64_t)s[11] * tmp[15]) >> 12);

    m[12] = (int32_t)(((int64_t)s[12] * tmp[0]  + (int64_t)s[13] * tmp[4]  + (int64_t)s[14] * tmp[8]  + (int64_t)s[15] * tmp[12]) >> 12);
    m[13] = (int32_t)(((int64_t)s[12] * tmp[1]  + (int64_t)s[13] * tmp[5]  + (int64_t)s[14] * tmp[9]  + (int64_t)s[15] * tmp[13]) >> 12);
    m[14] = (int32_t)(((int64_t)s[12] * tmp[2]  + (int64_t)s[13] * tmp[6]  + (int64_t)s[14] * tmp[10] + (int64_t)s[15] * tmp[14]) >> 12);
    m[15] = (int32_t)(((int64_t)s[12] * tmp[3]  + (int64_t)s[13] * tmp[7]  + (int64_t)s[14] * tmp[11] + (int64_t)s[15] * tmp[15]) >> 12);
}

void gx_mat_mult4x3(int32_t *m, const int32_t *s)
{
    int32_t tmp[16];
    memcpy(tmp, m, 16 * sizeof(int32_t));

    m[0]  = (int32_t)(((int64_t)s[0] * tmp[0]  + (int64_t)s[1] * tmp[4]  + (int64_t)s[2] * tmp[8]) >> 12);
    m[1]  = (int32_t)(((int64_t)s[0] * tmp[1]  + (int64_t)s[1] * tmp[5]  + (int64_t)s[2] * tmp[9]) >> 12);
    m[2]  = (int32_t)(((int64_t)s[0] * tmp[2]  + (int64_t)s[1] * tmp[6]  + (int64_t)s[2] * tmp[10]) >> 12);
    m[3]  = (int32_t)(((int64_t)s[0] * tmp[3]  + (int64_t)s[1] * tmp[7]  + (int64_t)s[2] * tmp[11]) >> 12);

    m[4]  = (int32_t)(((int64_t)s[3] * tmp[0]  + (int64_t)s[4] * tmp[4]  + (int64_t)s[5] * tmp[8]) >> 12);
    m[5]  = (int32_t)(((int64_t)s[3] * tmp[1]  + (int64_t)s[4] * tmp[5]  + (int64_t)s[5] * tmp[9]) >> 12);
    m[6]  = (int32_t)(((int64_t)s[3] * tmp[2]  + (int64_t)s[4] * tmp[6]  + (int64_t)s[5] * tmp[10]) >> 12);
    m[7]  = (int32_t)(((int64_t)s[3] * tmp[3]  + (int64_t)s[4] * tmp[7]  + (int64_t)s[5] * tmp[11]) >> 12);

    m[8]  = (int32_t)(((int64_t)s[6] * tmp[0]  + (int64_t)s[7] * tmp[4]  + (int64_t)s[8] * tmp[8]) >> 12);
    m[9]  = (int32_t)(((int64_t)s[6] * tmp[1]  + (int64_t)s[7] * tmp[5]  + (int64_t)s[8] * tmp[9]) >> 12);
    m[10] = (int32_t)(((int64_t)s[6] * tmp[2]  + (int64_t)s[7] * tmp[6]  + (int64_t)s[8] * tmp[10]) >> 12);
    m[11] = (int32_t)(((int64_t)s[6] * tmp[3]  + (int64_t)s[7] * tmp[7]  + (int64_t)s[8] * tmp[11]) >> 12);

    m[12] = (int32_t)(((int64_t)s[9] * tmp[0]  + (int64_t)s[10] * tmp[4]  + (int64_t)s[11] * tmp[8]  + (int64_t)0x1000 * tmp[12]) >> 12);
    m[13] = (int32_t)(((int64_t)s[9] * tmp[1]  + (int64_t)s[10] * tmp[5]  + (int64_t)s[11] * tmp[9]  + (int64_t)0x1000 * tmp[13]) >> 12);
    m[14] = (int32_t)(((int64_t)s[9] * tmp[2]  + (int64_t)s[10] * tmp[6]  + (int64_t)s[11] * tmp[10] + (int64_t)0x1000 * tmp[14]) >> 12);
    m[15] = (int32_t)(((int64_t)s[9] * tmp[3]  + (int64_t)s[10] * tmp[7]  + (int64_t)s[11] * tmp[11] + (int64_t)0x1000 * tmp[15]) >> 12);
}

void gx_mat_mult3x3(int32_t *m, const int32_t *s)
{
    int32_t tmp[12];
    memcpy(tmp, m, 12 * sizeof(int32_t));

    m[0]  = (int32_t)(((int64_t)s[0] * tmp[0]  + (int64_t)s[1] * tmp[4]  + (int64_t)s[2] * tmp[8]) >> 12);
    m[1]  = (int32_t)(((int64_t)s[0] * tmp[1]  + (int64_t)s[1] * tmp[5]  + (int64_t)s[2] * tmp[9]) >> 12);
    m[2]  = (int32_t)(((int64_t)s[0] * tmp[2]  + (int64_t)s[1] * tmp[6]  + (int64_t)s[2] * tmp[10]) >> 12);
    m[3]  = (int32_t)(((int64_t)s[0] * tmp[3]  + (int64_t)s[1] * tmp[7]  + (int64_t)s[2] * tmp[11]) >> 12);

    m[4]  = (int32_t)(((int64_t)s[3] * tmp[0]  + (int64_t)s[4] * tmp[4]  + (int64_t)s[5] * tmp[8]) >> 12);
    m[5]  = (int32_t)(((int64_t)s[3] * tmp[1]  + (int64_t)s[4] * tmp[5]  + (int64_t)s[5] * tmp[9]) >> 12);
    m[6]  = (int32_t)(((int64_t)s[3] * tmp[2]  + (int64_t)s[4] * tmp[6]  + (int64_t)s[5] * tmp[10]) >> 12);
    m[7]  = (int32_t)(((int64_t)s[3] * tmp[3]  + (int64_t)s[4] * tmp[7]  + (int64_t)s[5] * tmp[11]) >> 12);

    m[8]  = (int32_t)(((int64_t)s[6] * tmp[0]  + (int64_t)s[7] * tmp[4]  + (int64_t)s[8] * tmp[8]) >> 12);
    m[9]  = (int32_t)(((int64_t)s[6] * tmp[1]  + (int64_t)s[7] * tmp[5]  + (int64_t)s[8] * tmp[9]) >> 12);
    m[10] = (int32_t)(((int64_t)s[6] * tmp[2]  + (int64_t)s[7] * tmp[6]  + (int64_t)s[8] * tmp[10]) >> 12);
    m[11] = (int32_t)(((int64_t)s[6] * tmp[3]  + (int64_t)s[7] * tmp[7]  + (int64_t)s[8] * tmp[11]) >> 12);
}

void gx_mat_scale(int32_t *m, const int32_t *s)
{
    int i;
    for (i = 0; i < 4; i++) m[i]  = (int32_t)(((int64_t)s[0] * m[i]) >> 12);
    for (i = 4; i < 8; i++) m[i]  = (int32_t)(((int64_t)s[1] * m[i]) >> 12);
    for (i = 8; i < 12; i++) m[i] = (int32_t)(((int64_t)s[2] * m[i]) >> 12);
}

void gx_mat_translate(int32_t *m, const int32_t *s)
{
    m[12] += (int32_t)(((int64_t)s[0] * m[0] + (int64_t)s[1] * m[4] + (int64_t)s[2] * m[8]) >> 12);
    m[13] += (int32_t)(((int64_t)s[0] * m[1] + (int64_t)s[1] * m[5] + (int64_t)s[2] * m[9]) >> 12);
    m[14] += (int32_t)(((int64_t)s[0] * m[2] + (int64_t)s[1] * m[6] + (int64_t)s[2] * m[10]) >> 12);
    m[15] += (int32_t)(((int64_t)s[0] * m[3] + (int64_t)s[1] * m[7] + (int64_t)s[2] * m[11]) >> 12);
}
