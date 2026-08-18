#include "MeshAsset.h"
#include "AndroidOut.h"

#include <sstream>
#include <cmath>
#include <cstdlib>

namespace {

Vector3 makeV3(float x, float y, float z) {
    Vector3 v{};
    v.x = x; v.y = y; v.z = z;
    return v;
}

Vector2 makeV2(float u, float v) {
    Vector2 out{};
    out.u = u; out.v = v;
    return out;
}

Vector3 faceNormal(const Vector3 &a, const Vector3 &b, const Vector3 &c) {
    Vector3 u = makeV3(b.x - a.x, b.y - a.y, b.z - a.z);
    Vector3 v = makeV3(c.x - a.x, c.y - a.y, c.z - a.z);
    Vector3 n = makeV3(
            u.y * v.z - u.z * v.y,
            u.z * v.x - u.x * v.z,
            u.x * v.y - u.y * v.x);
    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-8f) {
        n.x /= len; n.y /= len; n.z /= len;
    } else {
        n = makeV3(0.f, 1.f, 0.f);
    }
    return n;
}

// Resolve a possibly-negative, 1-based OBJ index against a container size.
int resolveIndex(int raw, size_t count) {
    if (raw > 0) return raw - 1;
    if (raw < 0) return static_cast<int>(count) + raw; // negative = from end
    return -1;
}

} // namespace

MeshAsset::MeshData MeshAsset::parseObj(const std::string &text) {
    std::vector<Vector3> positions;
    std::vector<Vector2> uvs;
    std::vector<Vector3> normals;

    MeshData out;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        // strip trailing CR (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ls(line);
        std::string tag;
        ls >> tag;

        if (tag == "v") {
            float x, y, z; ls >> x >> y >> z;
            positions.push_back(makeV3(x, y, z));
        } else if (tag == "vt") {
            float u = 0.f, v = 0.f; ls >> u >> v;
            uvs.push_back(makeV2(u, v));
        } else if (tag == "vn") {
            float x, y, z; ls >> x >> y >> z;
            normals.push_back(makeV3(x, y, z));
        } else if (tag == "f") {
            // Collect the face's vertices, then fan-triangulate.
            struct Ref { int p; int t; int n; };
            std::vector<Ref> face;
            std::string tok;
            while (ls >> tok) {
                int p = 0, t = 0, n = 0;
                // formats: p | p/t | p//n | p/t/n
                size_t s1 = tok.find('/');
                if (s1 == std::string::npos) {
                    p = std::atoi(tok.c_str());
                } else {
                    p = std::atoi(tok.substr(0, s1).c_str());
                    size_t s2 = tok.find('/', s1 + 1);
                    if (s2 == std::string::npos) {
                        t = std::atoi(tok.substr(s1 + 1).c_str());
                    } else {
                        std::string tstr = tok.substr(s1 + 1, s2 - s1 - 1);
                        if (!tstr.empty()) t = std::atoi(tstr.c_str());
                        n = std::atoi(tok.substr(s2 + 1).c_str());
                    }
                }
                face.push_back({p, t, n});
            }
            if (face.size() < 3) continue;

            for (size_t i = 1; i + 1 < face.size(); ++i) {
                const Ref refs[3] = { face[0], face[i], face[i + 1] };

                Vector3 pos[3];
                bool haveNormal = true;
                Vector3 nrm[3];
                Vector2 uv[3];

                for (int k = 0; k < 3; ++k) {
                    int pi = resolveIndex(refs[k].p, positions.size());
                    pos[k] = (pi >= 0 && pi < (int) positions.size())
                             ? positions[pi] : makeV3(0.f, 0.f, 0.f);

                    if (refs[k].t != 0) {
                        int ti = resolveIndex(refs[k].t, uvs.size());
                        uv[k] = (ti >= 0 && ti < (int) uvs.size())
                                ? uvs[ti] : makeV2(0.f, 0.f);
                    } else {
                        uv[k] = makeV2(0.f, 0.f);
                    }

                    if (refs[k].n != 0 && !normals.empty()) {
                        int ni = resolveIndex(refs[k].n, normals.size());
                        if (ni >= 0 && ni < (int) normals.size()) nrm[k] = normals[ni];
                        else haveNormal = false;
                    } else {
                        haveNormal = false;
                    }
                }

                if (!haveNormal) {
                    Vector3 fn = faceNormal(pos[0], pos[1], pos[2]);
                    nrm[0] = nrm[1] = nrm[2] = fn;
                }

                for (int k = 0; k < 3; ++k) {
                    out.indices.push_back(static_cast<Index>(out.vertices.size()));
                    out.vertices.emplace_back(pos[k], nrm[k], uv[k]);
                }
            }
        }
    }

    return out;
}

MeshAsset::MeshData MeshAsset::loadObj(AAssetManager *assetManager, const std::string &assetPath) {
    AAsset *asset = AAssetManager_open(assetManager, assetPath.c_str(), AASSET_MODE_BUFFER);
    if (!asset) {
        aout << "MeshAsset: could not open " << assetPath << std::endl;
        return {};
    }
    off_t len = AAsset_getLength(asset);
    std::string text;
    text.resize(static_cast<size_t>(len));
    AAsset_read(asset, text.data(), static_cast<size_t>(len));
    AAsset_close(asset);

    MeshData data = parseObj(text);
    aout << "MeshAsset: loaded " << assetPath << " ("
         << data.vertices.size() << " verts)" << std::endl;
    return data;
}
