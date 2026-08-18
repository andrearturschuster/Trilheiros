#ifndef ANDROIDGLINVESTIGATIONS_MODEL_H
#define ANDROIDGLINVESTIGATIONS_MODEL_H

#include <vector>
#include <GLES3/gl3.h>
#include "TextureAsset.h"

union Vector3 {
    struct {
        float x, y, z;
    };
    float idx[3];
};

union Vector2 {
    struct {
        float x, y;
    };
    struct {
        float u, v;
    };
    float idx[2];
};

struct Vertex {
    // Back-compatible: quads without an explicit normal default to facing up (+Y).
    Vertex(const Vector3 &inPosition, const Vector2 &inUV)
            : position(inPosition), uv(inUV) {
        normal.x = 0.f; normal.y = 1.f; normal.z = 0.f;
    }

    // Full constructor used by loaded 3D meshes.
    constexpr Vertex(const Vector3 &inPosition, const Vector3 &inNormal, const Vector2 &inUV)
            : position(inPosition), normal(inNormal), uv(inUV) {}

    Vector3 position;
    Vector3 normal;
    Vector2 uv;
};

typedef uint16_t Index;

class Model {
public:
    inline Model(
            std::vector<Vertex> vertices,
            std::vector<Index> indices,
            std::shared_ptr<TextureAsset> spTexture)
            : vertices_(std::move(vertices)),
              indices_(std::move(indices)),
              spTexture_(std::move(spTexture)),
              vbo_(0),
              ibo_(0) {}

    inline ~Model() {
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (ibo_) glDeleteBuffers(1, &ibo_);
    }

    inline Model(Model&& other) noexcept
            : vertices_(std::move(other.vertices_)),
              indices_(std::move(other.indices_)),
              spTexture_(std::move(other.spTexture_)),
              vbo_(other.vbo_),
              ibo_(other.ibo_) {
        other.vbo_ = 0;
        other.ibo_ = 0;
    }

    inline Model& operator=(Model&& other) noexcept {
        if (this != &other) {
            if (vbo_) glDeleteBuffers(1, &vbo_);
            if (ibo_) glDeleteBuffers(1, &ibo_);
            vertices_ = std::move(other.vertices_);
            indices_ = std::move(other.indices_);
            spTexture_ = std::move(other.spTexture_);
            vbo_ = other.vbo_;
            ibo_ = other.ibo_;
            other.vbo_ = 0;
            other.ibo_ = 0;
        }
        return *this;
    }

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    void uploadToGPU() {
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, vertices_.size() * sizeof(Vertex), vertices_.data(), GL_STATIC_DRAW);

        glGenBuffers(1, &ibo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices_.size() * sizeof(Index), indices_.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    inline GLuint getVBO() const { return vbo_; }
    inline GLuint getIBO() const { return ibo_; }

    inline const Vertex *getVertexData() const {
        return vertices_.data();
    }

    inline const size_t getIndexCount() const {
        return indices_.size();
    }

    inline const Index *getIndexData() const {
        return indices_.data();
    }

    inline const TextureAsset &getTexture() const {
        return *spTexture_;
    }

private:
    std::vector<Vertex> vertices_;
    std::vector<Index> indices_;
    std::shared_ptr<TextureAsset> spTexture_;
    GLuint vbo_, ibo_;
};

#endif //ANDROIDGLINVESTIGATIONS_MODEL_H