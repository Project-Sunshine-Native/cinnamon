#pragma once

#include <math.h>
#include <string.h>

// ===[ Matrix4f Type ]===

// Column-major 4x4 matrix (OpenGL native layout)
// Layout:
//   m[0]  m[4]  m[8]   m[12] (tx)
//   m[1]  m[5]  m[9]   m[13] (ty)
//   m[2]  m[6]  m[10]  m[14] (tz)
//   m[3]  m[7]  m[11]  m[15] (1)
typedef struct {
    float m[16]; // m[col*4 + row]
} Matrix4f;

// ===[ Identity / Copy ]===

static Matrix4f* Matrix4f_identity(Matrix4f* dest) {
    memset(dest->m, 0, sizeof(dest->m));
    dest->m[0] = 1.0f;
    dest->m[5] = 1.0f;
    dest->m[10] = 1.0f;
    dest->m[15] = 1.0f;
    return dest;
}

static Matrix4f* Matrix4f_copy(Matrix4f* dest, const Matrix4f* src) {
    memcpy(dest->m, src->m, sizeof(dest->m));
    return dest;
}

// ===[ Multiply ]===

// dest = a * b (safe if dest aliases a or b)
static Matrix4f* Matrix4f_multiply(Matrix4f* dest, const Matrix4f* a, const Matrix4f* b) {
    float tmp[16];
    for (int col = 0; 4 > col; col++) {
        for (int row = 0; 4 > row; row++) {
            tmp[col * 4 + row] =
                a->m[0 * 4 + row] * b->m[col * 4 + 0] +
                a->m[1 * 4 + row] * b->m[col * 4 + 1] +
                a->m[2 * 4 + row] * b->m[col * 4 + 2] +
                a->m[3 * 4 + row] * b->m[col * 4 + 3];
        }
    }
    memcpy(dest->m, tmp, sizeof(tmp));
    return dest;
}

// ===[ Orthographic Projection ]===

// Post-multiply orthographic projection onto dest: dest = dest * ortho(l, r, b, t, n, f)
static Matrix4f* Matrix4f_ortho(Matrix4f* dest, float left, float right, float bottom, float top, float near, float far) {
    Matrix4f ortho;
    memset(ortho.m, 0, sizeof(ortho.m));
    ortho.m[0] = 2.0f / (right - left);
    ortho.m[5] = 2.0f / (top - bottom);
    ortho.m[10] = -2.0f / (far - near);
    ortho.m[12] = -(right + left) / (right - left);
    ortho.m[13] = -(top + bottom) / (top - bottom);
    ortho.m[14] = -(far + near) / (far - near);
    ortho.m[15] = 1.0f;
    return Matrix4f_multiply(dest, dest, &ortho);
}

// ===[ Translate ]===

// Post-multiply translation onto dest: dest = dest * T(x, y, z)
// Optimized: only column 3 changes when post-multiplying a translation matrix
static Matrix4f* Matrix4f_translate(Matrix4f* dest, float x, float y, float z) {
    dest->m[12] += dest->m[0] * x + dest->m[4] * y + dest->m[8] * z;
    dest->m[13] += dest->m[1] * x + dest->m[5] * y + dest->m[9] * z;
    dest->m[14] += dest->m[2] * x + dest->m[6] * y + dest->m[10] * z;
    dest->m[15] += dest->m[3] * x + dest->m[7] * y + dest->m[11] * z;
    return dest;
}

// ===[ Rotate Z ]===

// Post-multiply Z-axis rotation onto dest: dest = dest * Rz(angleRadians)
static Matrix4f* Matrix4f_rotateZ(Matrix4f* dest, float angleRadians) {
    float c = cosf(angleRadians);
    float s = sinf(angleRadians);
    // Columns 0 and 1 are affected: new_col0 = col0*c + col1*s, new_col1 = col0*(-s) + col1*c
    for (int row = 0; 4 > row; row++) {
        float a0 = dest->m[0 * 4 + row];
        float a1 = dest->m[1 * 4 + row];
        dest->m[0 * 4 + row] = a0 * c + a1 * s;
        dest->m[1 * 4 + row] = a0 * (-s) + a1 * c;
    }
    return dest;
}

// ===[ Scale ]===

// Post-multiply scale onto dest: dest = dest * S(sx, sy, sz)
// Optimized: scales each column directly
static Matrix4f* Matrix4f_scale(Matrix4f* dest, float sx, float sy, float sz) {
    for (int row = 0; 4 > row; row++) {
        dest->m[0 * 4 + row] *= sx;
        dest->m[1 * 4 + row] *= sy;
        dest->m[2 * 4 + row] *= sz;
    }
    return dest;
}

// ===[ Set Transform 2D ]===

// Directly sets dest to a combined translate * rotateZ * scale matrix (no post-multiply)
// Equivalent to: identity -> translate(x, y, 0) -> rotateZ(angleRad) -> scale(sx, sy, 1)
static Matrix4f* Matrix4f_setTransform2D(Matrix4f* dest, float x, float y, float sx, float sy, float angleRad) {
    float c = cosf(angleRad);
    float s = sinf(angleRad);

    // Column 0: rotated+scaled X axis
    dest->m[0] = c * sx;
    dest->m[1] = s * sx;
    dest->m[2] = 0.0f;
    dest->m[3] = 0.0f;

    // Column 1: rotated+scaled Y axis
    dest->m[4] = -s * sy;
    dest->m[5] = c * sy;
    dest->m[6] = 0.0f;
    dest->m[7] = 0.0f;

    // Column 2: Z axis (identity for 2D)
    dest->m[8] = 0.0f;
    dest->m[9] = 0.0f;
    dest->m[10] = 1.0f;
    dest->m[11] = 0.0f;

    // Column 3: translation
    dest->m[12] = x;
    dest->m[13] = y;
    dest->m[14] = 0.0f;
    dest->m[15] = 1.0f;

    return dest;
}

// ===[ Transform Point ]===

// Transform a 2D point (x, y) through the matrix (w=1), writing results to outX, outY
// Useful for CPU-side vertex transforms (e.g. PS2/gsKit software rendering)
static void Matrix4f_transformPoint(const Matrix4f* mat, float x, float y, float* outX, float* outY) {
    *outX = mat->m[0] * x + mat->m[4] * y + mat->m[12];
    *outY = mat->m[1] * x + mat->m[5] * y + mat->m[13];
}
