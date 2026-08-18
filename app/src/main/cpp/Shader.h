#ifndef ANDROIDGLINVESTIGATIONS_SHADER_H
#define ANDROIDGLINVESTIGATIONS_SHADER_H

#include <string>
#include <GLES3/gl3.h>

class Model;

/*!
 * A class representing a simple shader program. It consists of vertex and fragment components. The
 * input attributes are a position (as a Vector3) and a uv (as a Vector2). It also takes a uniform
 * to be used as the entire model/view/projection matrix. The shader expects a single texture for
 * fragment shading, and does no other lighting calculations (thus no uniforms for lights or normal
 * attributes).
 */
class Shader {
public:
    /*!
     * Loads a shader given the full sourcecode and names for necessary attributes and uniforms to
     * link to. Returns a valid shader on success or null on failure. Shader resources are
     * automatically cleaned up on destruction.
     *
     * @param vertexSource The full source code for your vertex program
     * @param fragmentSource The full source code of your fragment program
     * @param positionAttributeName The name of the position attribute in your vertex program
     * @param uvAttributeName The name of the uv coordinate attribute in your vertex program
     * @param projectionMatrixUniformName The name of your model/view/projection matrix uniform
     * @param colorUniformName The name of your color tint uniform
     * @return a valid Shader on success, otherwise null.
     */
    static Shader *loadShader(
            const std::string &vertexSource,
            const std::string &fragmentSource,
            const std::string &positionAttributeName,
            const std::string &uvAttributeName,
            const std::string &projectionMatrixUniformName,
            const std::string &colorUniformName,
            const std::string &modelMatrixUniformName,
            const std::string &lightPosUniformName,
            const std::string &lightDirUniformName,
            const std::string &lightColorUniformName,
            const std::string &lightEnabledUniformName);

    inline ~Shader() {
        if (program_) {
            glDeleteProgram(program_);
            program_ = 0;
        }
    }

    /*!
     * Prepares the shader for use, call this before executing any draw commands
     */
    void activate() const;

    /*!
     * Cleans up the shader after use, call this after executing any draw commands
     */
    void deactivate() const;

    /*!
     * Renders a single model
     * @param model a model to render
     */
    void drawModel(const Model &model) const;

    /*!
     * Sets the model/view/projection matrix in the shader.
     * @param projectionMatrix sixteen floats, column major, defining an OpenGL projection matrix.
     */
    void setProjectionMatrix(float *projectionMatrix) const;

    /*!
     * Sets the model matrix in the shader.
     */
    void setModelMatrix(float *modelMatrix) const;

    /*!
     * Sets a color tint for the shader.
     * @param r red component [0, 1]
     * @param g green component [0, 1]
     * @param b blue component [0, 1]
     * @param a alpha component [0, 1]
     */
    void setColor(float r, float g, float b, float a) const;

    void setLightParams(float *pos, float *dir, float *color, bool enabled) const;
    void setLightEnabled(bool enabled) const;

private:
    /*!
     * Helper function to load a shader of a given type
     * @param shaderType The OpenGL shader type. Should either be GL_VERTEX_SHADER or GL_FRAGMENT_SHADER
     * @param shaderSource The full source of the shader
     * @return the id of the shader, as returned by glCreateShader, or 0 in the case of an error
     */
    static GLuint loadShader(GLenum shaderType, const std::string &shaderSource);

    /*!
     * Constructs a new instance of a shader. Use @a loadShader
     */
    constexpr Shader(
            GLuint program,
            GLint position,
            GLint uv,
            GLint projectionMatrix,
            GLint color,
            GLint modelMatrix,
            GLint lightPos,
            GLint lightDir,
            GLint lightColor,
            GLint lightEnabled)
            : program_(program),
              position_(position),
              uv_(uv),
              projectionMatrix_(projectionMatrix),
              color_(color),
              modelMatrix_(modelMatrix),
              lightPos_(lightPos),
              lightDir_(lightDir),
              lightColor_(lightColor),
              lightEnabled_(lightEnabled) {}

    GLuint program_;
    GLint position_;
    GLint uv_;
    GLint projectionMatrix_;
    GLint color_;
    GLint modelMatrix_;
    GLint lightPos_;
    GLint lightDir_;
    GLint lightColor_;
    GLint lightEnabled_;
};

#endif //ANDROIDGLINVESTIGATIONS_SHADER_H