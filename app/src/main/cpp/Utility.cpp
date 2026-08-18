#include "Utility.h"
#include "AndroidOut.h"

#include <GLES3/gl3.h>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHECK_ERROR(e) case e: aout << "GL Error: "#e << std::endl; break;

bool Utility::checkAndLogGlError(bool alwaysLog) {
    GLenum error = glGetError();
    if (error == GL_NO_ERROR) {
        if (alwaysLog) {
            aout << "No GL error" << std::endl;
        }
        return true;
    } else {
        switch (error) {
            CHECK_ERROR(GL_INVALID_ENUM);
            CHECK_ERROR(GL_INVALID_VALUE);
            CHECK_ERROR(GL_INVALID_OPERATION);
            CHECK_ERROR(GL_INVALID_FRAMEBUFFER_OPERATION);
            CHECK_ERROR(GL_OUT_OF_MEMORY);
            default:
                aout << "Unknown GL error: " << error << std::endl;
        }
        return false;
    }
}

float *
Utility::buildOrthographicMatrix(float *outMatrix, float halfHeight, float aspect, float near,
                                 float far) {
    float halfWidth = halfHeight * aspect;

    // column 1
    outMatrix[0] = 1.f / halfWidth;
    outMatrix[1] = 0.f;
    outMatrix[2] = 0.f;
    outMatrix[3] = 0.f;

    // column 2
    outMatrix[4] = 0.f;
    outMatrix[5] = 1.f / halfHeight;
    outMatrix[6] = 0.f;
    outMatrix[7] = 0.f;

    // column 3
    outMatrix[8] = 0.f;
    outMatrix[9] = 0.f;
    outMatrix[10] = -2.f / (far - near);
    outMatrix[11] = -(far + near) / (far - near);

    // column 4
    outMatrix[12] = 0.f;
    outMatrix[13] = 0.f;
    outMatrix[14] = 0.f;
    outMatrix[15] = 1.f;

    return outMatrix;
}

void Utility::multiplyMatrices(float *out, const float *a, const float *b) {
    float tmp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            tmp[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] +
                             a[1 * 4 + r] * b[c * 4 + 1] +
                             a[2 * 4 + r] * b[c * 4 + 2] +
                             a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    for (int i = 0; i < 16; i++) out[i] = tmp[i];
}

void Utility::buildTranslationMatrix(float *outMatrix, float x, float y, float z) {
    buildIdentityMatrix(outMatrix);
    outMatrix[12] = x;
    outMatrix[13] = y;
    outMatrix[14] = z;
}

void Utility::buildRotationYMatrix(float *outMatrix, float angle) {
    buildIdentityMatrix(outMatrix);
    float s = sinf(angle);
    float c = cosf(angle);
    outMatrix[0] = c;
    outMatrix[2] = -s;
    outMatrix[8] = s;
    outMatrix[10] = c;
}

void Utility::buildScaleMatrix(float *outMatrix, float sx, float sy, float sz) {
    buildIdentityMatrix(outMatrix);
    outMatrix[0] = sx;
    outMatrix[5] = sy;
    outMatrix[10] = sz;
}

float *Utility::buildPerspectiveMatrix(float *outMatrix, float fov, float aspect, float near, float far) {
    float f = 1.0f / tanf(fov * (M_PI / 360.0f));
    float rangeInv = 1.0f / (near - far);

    outMatrix[0] = f / aspect;
    outMatrix[1] = 0.0f;
    outMatrix[2] = 0.0f;
    outMatrix[3] = 0.0f;

    outMatrix[4] = 0.0f;
    outMatrix[5] = f;
    outMatrix[6] = 0.0f;
    outMatrix[7] = 0.0f;

    outMatrix[8] = 0.0f;
    outMatrix[9] = 0.0f;
    outMatrix[10] = (far + near) * rangeInv;
    outMatrix[11] = -1.0f;

    outMatrix[12] = 0.0f;
    outMatrix[13] = 0.0f;
    outMatrix[14] = (2.0f * far * near) * rangeInv;
    outMatrix[15] = 0.0f;

    return outMatrix;
}

float *Utility::buildIdentityMatrix(float *outMatrix) {
    // column 1
    outMatrix[0] = 1.f;
    outMatrix[1] = 0.f;
    outMatrix[2] = 0.f;
    outMatrix[3] = 0.f;

    // column 2
    outMatrix[4] = 0.f;
    outMatrix[5] = 1.f;
    outMatrix[6] = 0.f;
    outMatrix[7] = 0.f;

    // column 3
    outMatrix[8] = 0.f;
    outMatrix[9] = 0.f;
    outMatrix[10] = 1.f;
    outMatrix[11] = 0.f;

    // column 4
    outMatrix[12] = 0.f;
    outMatrix[13] = 0.f;
    outMatrix[14] = 0.f;
    outMatrix[15] = 1.f;

    return outMatrix;
}