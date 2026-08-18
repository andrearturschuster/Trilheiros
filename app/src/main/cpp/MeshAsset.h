#ifndef TRILHEIRO_MESHASSET_H
#define TRILHEIRO_MESHASSET_H

#include <vector>
#include <string>
#include <android/asset_manager.h>
#include "Model.h"

/*!
 * Minimal Wavefront OBJ loader.
 *
 * Supports v / vt / vn and triangular or polygonal faces written as
 * "f v", "f v/vt", "f v//vn" or "f v/vt/vn" (1-based indices). Polygons are
 * fan-triangulated. If a face has no normals, a flat face normal is computed.
 *
 * The result is returned as raw vertex/index arrays ready to build a Model:
 *     auto md = MeshAsset::loadObj(assetManager, "car.obj");
 *     models_.emplace_back(md.vertices, md.indices, texture);
 *
 * To use real hand-modelled art later, just drop a triangulated .obj into
 * app/src/main/assets/ and load it here -- no other code changes required.
 */
class MeshAsset {
public:
    struct MeshData {
        std::vector<Vertex> vertices;
        std::vector<Index> indices;
    };

    static MeshData loadObj(AAssetManager *assetManager, const std::string &assetPath);

    // Parses OBJ text directly (used by loadObj; exposed for host-side testing).
    static MeshData parseObj(const std::string &text);
};

#endif // TRILHEIRO_MESHASSET_H
