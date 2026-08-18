#ifndef ANDROIDGLINVESTIGATIONS_UTILITY_H
#define ANDROIDGLINVESTIGATIONS_UTILITY_H

#include <cassert>

class Utility {
public:
    static bool checkAndLogGlError(bool alwaysLog = false);

    static inline void assertGlError() { assert(checkAndLogGlError()); }

    /**
     * Generates an orthographic projection matrix given the half height, aspect ratio, near, and far
     * planes
     *
     * @param outMatrix the matrix to write into
     * @param halfHeight half of the height of the screen
     * @param aspect the width of the screen divided by the height
     * @param near the distance of the near plane
     * @param far the distance of the far plane
     * @return the generated matrix, this will be the same as @a outMatrix so you can chain calls
     *     together if needed
     */
    static float *buildOrthographicMatrix(
            float *outMatrix,
            float halfHeight,
            float aspect,
            float near,
            float far);

    static float *buildPerspectiveMatrix(
            float *outMatrix,
            float fov,
            float aspect,
            float near,
            float far);

    static float *buildIdentityMatrix(float *outMatrix);

    static void multiplyMatrices(float *out, const float *a, const float *b);

    static void buildTranslationMatrix(float *outMatrix, float x, float y, float z);

    static void buildRotationYMatrix(float *outMatrix, float angle);

    static void buildScaleMatrix(float *outMatrix, float sx, float sy, float sz);
};

#endif //ANDROIDGLINVESTIGATIONS_UTILITY_H