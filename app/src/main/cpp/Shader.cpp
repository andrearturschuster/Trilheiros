#include "Shader.h"

#include "AndroidOut.h"
#include "Model.h"
#include "Utility.h"

Shader *Shader::loadShader(
        const std::string &vertexSource,
        const std::string &fragmentSource,
        const std::string &positionAttributeName,
        const std::string &uvAttributeName,
        const std::string &normalAttributeName,
        const std::string &projectionMatrixUniformName,
        const std::string &colorUniformName,
        const std::string &modelMatrixUniformName,
        const std::string &lightPosUniformName,
        const std::string &lightDirUniformName,
        const std::string &lightColorUniformName,
        const std::string &lightEnabledUniformName) {
    Shader *shader = nullptr;

    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, vertexSource);
    if (!vertexShader) {
        return nullptr;
    }

    GLuint fragmentShader = loadShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!fragmentShader) {
        glDeleteShader(vertexShader);
        return nullptr;
    }

    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);

        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            GLint logLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);

            // If we fail to link the shader program, log the result for debugging
            if (logLength) {
                GLchar *log = new GLchar[logLength];
                glGetProgramInfoLog(program, logLength, nullptr, log);
                aout << "Failed to link program with:\n" << log << std::endl;
                delete[] log;
            }

            glDeleteProgram(program);
        } else {
            // Get the attribute and uniform locations by name. You may also choose to hardcode
            // indices with layout= in your shader, but it is not done in this sample
            GLint positionAttribute = glGetAttribLocation(program, positionAttributeName.c_str());
            GLint uvAttribute = glGetAttribLocation(program, uvAttributeName.c_str());
            GLint normalAttribute = glGetAttribLocation(program, normalAttributeName.c_str());
            GLint projectionMatrixUniform = glGetUniformLocation(
                    program,
                    projectionMatrixUniformName.c_str());
            GLint colorUniform = glGetUniformLocation(program, colorUniformName.c_str());
            GLint modelMatrixUniform = glGetUniformLocation(program, modelMatrixUniformName.c_str());
            GLint lightPosUniform = glGetUniformLocation(program, lightPosUniformName.c_str());
            GLint lightDirUniform = glGetUniformLocation(program, lightDirUniformName.c_str());
            GLint lightColorUniform = glGetUniformLocation(program, lightColorUniformName.c_str());
            GLint lightEnabledUniform = glGetUniformLocation(program, lightEnabledUniformName.c_str());

            // Ensure the sampler uses texture unit 0
            glUseProgram(program);
            GLint uTextureLocation = glGetUniformLocation(program, "uTexture");
            if (uTextureLocation != -1) {
                glUniform1i(uTextureLocation, 0);
            }

            // Create a new shader even if some uniforms are not found.
            // Critical attributes are position and UV.
            if (positionAttribute != -1 && uvAttribute != -1) {
                shader = new Shader(
                        program,
                        positionAttribute,
                        uvAttribute,
                        normalAttribute,
                        projectionMatrixUniform,
                        colorUniform,
                        modelMatrixUniform,
                        lightPosUniform,
                        lightDirUniform,
                        lightColorUniform,
                        lightEnabledUniform);
            } else {
                glDeleteProgram(program);
            }
        }
    }

    // The shaders are no longer needed once the program is linked. Release their memory.
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return shader;
}

GLuint Shader::loadShader(GLenum shaderType, const std::string &shaderSource) {
    Utility::assertGlError();
    GLuint shader = glCreateShader(shaderType);
    if (shader) {
        auto *shaderRawString = (GLchar *) shaderSource.c_str();
        GLint shaderLength = shaderSource.length();
        glShaderSource(shader, 1, &shaderRawString, &shaderLength);
        glCompileShader(shader);

        GLint shaderCompiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &shaderCompiled);

        // If the shader doesn't compile, log the result to the terminal for debugging
        if (!shaderCompiled) {
            GLint infoLength = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLength);

            if (infoLength) {
                auto *infoLog = new GLchar[infoLength];
                glGetShaderInfoLog(shader, infoLength, nullptr, infoLog);
                aout << "Failed to compile with:\n" << infoLog << std::endl;
                delete[] infoLog;
            }

            glDeleteShader(shader);
            shader = 0;
        }
    }
    return shader;
}

void Shader::activate() const {
    glUseProgram(program_);
}

void Shader::deactivate() const {
    glUseProgram(0);
}

void Shader::drawModel(const Model &model) const {
    glBindBuffer(GL_ARRAY_BUFFER, model.getVBO());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model.getIBO());

    // The position attribute is 3 floats
    glVertexAttribPointer(
            position_, // attrib
            3, // elements
            GL_FLOAT, // of type float
            GL_FALSE, // don't normalize
            sizeof(Vertex), // stride is Vertex bytes
            nullptr // pull from the start of the vertex data (offset 0)
    );
    glEnableVertexAttribArray(position_);

    // The uv attribute is 2 floats (now after position + normal)
    glVertexAttribPointer(
            uv_, // attrib
            2, // elements
            GL_FLOAT, // of type float
            GL_FALSE, // don't normalize
            sizeof(Vertex), // stride is Vertex bytes
            (void*)(2 * sizeof(Vector3)) // offset: past position and normal
    );
    glEnableVertexAttribArray(uv_);

    // The normal attribute is 3 floats, right after the position
    if (normal_ != -1) {
        glVertexAttribPointer(
                normal_,
                3,
                GL_FLOAT,
                GL_FALSE,
                sizeof(Vertex),
                (void*)sizeof(Vector3) // offset: past position
        );
        glEnableVertexAttribArray(normal_);
    }

    // Setup the texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, model.getTexture().getTextureID());

    // Draw as indexed triangles
    glDrawElements(GL_TRIANGLES, model.getIndexCount(), GL_UNSIGNED_SHORT, nullptr);

    glDisableVertexAttribArray(uv_);
    if (normal_ != -1) glDisableVertexAttribArray(normal_);
    glDisableVertexAttribArray(position_);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void Shader::setProjectionMatrix(float *projectionMatrix) const {
    glUniformMatrix4fv(projectionMatrix_, 1, false, projectionMatrix);
}

void Shader::setModelMatrix(float *modelMatrix) const {
    if (modelMatrix_ != -1) {
        glUniformMatrix4fv(modelMatrix_, 1, false, modelMatrix);
    }
}

void Shader::setColor(float r, float g, float b, float a) const {
    glUniform4f(color_, r, g, b, a);
}

void Shader::setLightParams(float *pos, float *dir, float *color, bool enabled) const {
    if (lightPos_ != -1) glUniform3fv(lightPos_, 1, pos);
    if (lightDir_ != -1) glUniform3fv(lightDir_, 1, dir);
    if (lightColor_ != -1) glUniform4fv(lightColor_, 1, color);
    if (lightEnabled_ != -1) glUniform1i(lightEnabled_, enabled ? 1 : 0);
}

void Shader::setLightEnabled(bool enabled) const {
    if (lightEnabled_ != -1) glUniform1i(lightEnabled_, enabled ? 1 : 0);
}
