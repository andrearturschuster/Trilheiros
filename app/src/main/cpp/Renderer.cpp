#include "Renderer.h"

#include <jni.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <GLES3/gl3.h>
#include <memory>
#include <vector>
#include <android/imagedecoder.h>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "AndroidOut.h"
#include "Shader.h"
#include "Utility.h"
#include "TextureAsset.h"

//! executes glGetString and outputs the result to logcat
#define PRINT_GL_STRING(s) {aout << #s": "<< glGetString(s) << std::endl;}

/*!
 * @brief if glGetString returns a space separated list of elements, prints each one on a new line
 *
 * This works by creating an istringstream of the input c-style string. Then that is used to create
 * a vector -- each element of the vector is a new element in the input string. Finally a foreach
 * loop consumes this and outputs it to logcat using @a aout
 */
#define PRINT_GL_STRING_AS_LIST(s) { \
std::istringstream extensionStream((const char *) glGetString(s));\
std::vector<std::string> extensionList(\
        std::istream_iterator<std::string>{extensionStream},\
        std::istream_iterator<std::string>());\
aout << #s":\n";\
for (auto& extension: extensionList) {\
    aout << extension << "\n";\
}\
aout << std::endl;\
}

// Vertex shader, you'd typically load this from assets
static const char *vertex = R"vertex(#version 300 es
in vec3 inPosition;
in vec2 inUV;

out vec2 fragUV;

uniform mat4 uMVP;

void main() {
    fragUV = inUV;
    gl_Position = uMVP * vec4(inPosition, 1.0);
}
)vertex";

// Fragment shader, you'd typically load this from assets
static const char *fragment = R"fragment(#version 300 es
precision mediump float;

in vec2 fragUV;

uniform sampler2D uTexture;
uniform vec4 uColor;

out vec4 outColor;

void main() {
    outColor = texture(uTexture, fragUV) * uColor;
}
)fragment";

Renderer::~Renderer() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
}

void Renderer::update() {
    if (gameState_ != GameState::GAMEPLAY) return;

    float dt = 1.0f / 60.0f; // Simplified fixed delta time

    // Transmission logic
    float gearRatio = 0.0f;
    if (vehicleState_.currentGear == -1) {
        gearRatio = currentVehicle_.gearRatios[0];
    } else if (vehicleState_.currentGear > 0) {
        // gearRatios[2] is 1st gear
        gearRatio = currentVehicle_.gearRatios[vehicleState_.currentGear + 1];
    }

    float targetAccel = acceleratorValue_;
    if (isDamageEnabled_ && vehicleHealth_ <= 0.0f) {
        targetAccel = 0.0f;
        carSpeed_ = 0.0f;
    }

    // Fuel Logic
    if (currentFuel_ > 0.0f) {
        float consumption = (vehicleState_.rpm / 7000.0f) * currentVehicle_.fuelConsumptionRate * (0.5f + fabsf(acceleratorValue_) * 0.5f) * dt;
        currentFuel_ -= consumption;
        if (currentFuel_ < 0.0f) currentFuel_ = 0.0f;
    } else {
        targetAccel = 0.0f;
        carSpeed_ *= 0.95f; // Coasting to a stop
    }

    // Refueling Logic
    for (const auto& station : fuelStations_) {
        float dx = carX_ - station.x;
        float dz = carZ_ - station.z;
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist < station.radius && fabsf(carSpeed_) < 0.5f) {
            if (currentFuel_ < fuelCapacity_) {
                float refillAmount = 5.0f * dt;
                if (currentFuel_ + refillAmount > fuelCapacity_) refillAmount = fuelCapacity_ - currentFuel_;
                currentFuel_ += refillAmount;
                playerMoney_ -= (long)(refillAmount * 2.0f); // $2 per fuel unit
                if (playerMoney_ < 0) playerMoney_ = 0;
            }
        }
    }

    // Traction Control: limit accelerator if wheel spin would occur
    if (vehicleState_.isTCEnabled && acceleratorValue_ > 0.1f) {
        float maxAllowedAccel = 0.4f + fabsf(carSpeed_) * 0.05f;
        if (targetAccel > maxAllowedAccel) {
            targetAccel = maxAllowedAccel;
        }
    }

    // Physics: Force = Mass * Acceleration => Accel = Force / Mass
    // Here we treat Torque * targetAccel * gearRatio as a force-like value.
    float totalWeight = currentVehicle_.weight;
    if (currentVehicle_.currentAddonIndex != -1) {
        totalWeight += addonCatalog_[currentVehicle_.currentAddonIndex].weightImpact;
    }
    totalWeight += currentCargo_.weight;

    float force = currentVehicle_.engineTorque * targetAccel * gearRatio * 50.0f;
    float acceleration = force / totalWeight;

    // Mud Logic
    float mudFriction = 0.0f;
    bool inMud = false;
    for (const auto& mud : mudZones_) {
        float dx = carX_ - mud.x;
        float dz = carZ_ - mud.z;
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist < mud.radius) {
            mudFriction = mud.friction;
            inMud = true;
            break;
        }
    }

    if (inMud) {
        float penalty = mudFriction;
        if (vehicleState_.is4x4Enabled) penalty *= 0.5f;
        acceleration *= (1.0f - penalty);
    }

    // 4x4 Grip bonus: better acceleration
    if (vehicleState_.is4x4Enabled) {
        acceleration *= 1.5f;
    }

    if (vehicleState_.currentGear != 0) {
        carSpeed_ += acceleration * dt;
    } else {
        carSpeed_ *= 0.95f; // Neutral friction/rolling resistance
    }

    // Stuck logic: if in mud, very slow and low torque, stop
    if (inMud && fabsf(carSpeed_) < 0.2f && acceleration < 0.1f) {
        carSpeed_ = 0.0f;
    }

    // Speed limiting based on gear ratio
    float maxSpeed = (gearRatio != 0.0f) ? (30.0f / fabsf(gearRatio)) : 0.0f;
    if (carSpeed_ > maxSpeed) carSpeed_ = maxSpeed;
    if (carSpeed_ < -maxSpeed) carSpeed_ = -maxSpeed;

    // Slope/Gravity Logic
    carY_ = 0.0f;
    carPitch_ = 0.0f;
    for (const auto& slope : slopeZones_) {
        float halfW = slope.width / 2.0f;
        float halfL = slope.length / 2.0f;
        if (carX_ > slope.x - halfW && carX_ < slope.x + halfW &&
            carZ_ > slope.z - halfL && carZ_ < slope.z + halfL) {

            float progress = (carZ_ - (slope.z - halfL)) / slope.length;
            carY_ = progress * slope.heightDelta;

            float angle = atan2f(slope.heightDelta, slope.length);
            carPitch_ = -angle;

            // Gravity: subtract if going up (carSpeed_ > 0), add if going down
            float gravityEffect = 5.0f * sinf(angle) * (totalWeight / 2000.0f);
            carSpeed_ -= gravityEffect * dt;
        }
    }

    // Natural deceleration (Drag proportional to speed and weight)
    carSpeed_ *= (0.99f - (totalWeight / 100000.0f));

    // RPM simulation
    if (gearRatio != 0.0f) {
        vehicleState_.rpm = fabsf(carSpeed_ * gearRatio * 300.0f) + 800.0f;
    } else {
        vehicleState_.rpm = fabsf(acceleratorValue_ * 5000.0f) + 800.0f;
    }
    if (vehicleState_.rpm > 7000.0f) vehicleState_.rpm = 7000.0f;

    // RPM Damage logic
    if (isDamageEnabled_) {
        if (vehicleState_.rpm > 6800.0f) {
            rpmOverLimitDuration_ += dt;
            if (rpmOverLimitDuration_ > 1.0f) {
                vehicleHealth_ -= 5.0f * dt; // Continuous damage after 1s
            }
        } else {
            rpmOverLimitDuration_ = 0.0f;
        }
    }

    float currentSteer = steer_;
    if (useTilt_) {
        // Normalize tilt: accelerometer Y is typically around -9.8 to 9.8
        // Let's say 5.0 is full turn
        currentSteer = -tilt_ / 5.0f;
        if (currentSteer > 1.0f) currentSteer = 1.0f;
        if (currentSteer < -1.0f) currentSteer = -1.0f;
    }

    // Heavier vehicles turn slower
    float weightFactor = 1500.0f / totalWeight;
    float rotationStep = currentSteer * carSpeed_ * 0.1f * weightFactor;
    // Diff Lock: reduces turning speed due to locked axles fighting
    if (vehicleState_.isDiffLockEnabled) {
        rotationStep *= 0.5f;
    }

    carRotation_ += rotationStep * dt;

    carX_ += sinf(carRotation_) * carSpeed_ * dt;
    carZ_ += cosf(carRotation_) * carSpeed_ * dt;

    if (buttonFeedbackTimer_ > 0.0f) {
        buttonFeedbackTimer_ -= dt;
        if (buttonFeedbackTimer_ <= 0.0f) pressedButtonId_ = -1;
    }

    // Winch Logic
    if (isWinchAttached_ && winchAnchorIndex_ >= 0 && winchAnchorIndex_ < obstacles_.size()) {
        const auto& anchor = obstacles_[winchAnchorIndex_];
        float dx = anchor.x - carX_;
        float dz = anchor.z - carZ_;
        float dist = sqrtf(dx * dx + dz * dz);

        // Auto-detach if too far
        if (dist > maxWinchDistance_ + 2.0f) {
            isWinchAttached_ = false;
            isWinching_ = false;
            winchAnchorIndex_ = -1;
        }

        if (isWinching_) {
            // Apply pulling force towards anchor
            float pullStrength = 2.0f; // Speed of winching
            if (dist > 0.5f) { // Don't pull if already at anchor
                carX_ += (dx / dist) * pullStrength * dt;
                carZ_ += (dz / dist) * pullStrength * dt;
                // Also give a slight speed boost in that direction to overcome friction
                carSpeed_ += 0.5f * dt;
            }
        }
    }

    // Boundary check (50x50 ground)
    if (carX_ > 48.0f) carX_ = 48.0f;
    if (carX_ < -48.0f) carX_ = -48.0f;
    if (carZ_ > 48.0f) carZ_ = 48.0f;
    if (carZ_ < -48.0f) carZ_ = -48.0f;

    // Goal Check: If carZ > 45.0, finish mission
    if (carZ_ > 45.0f) {
        float cargoBonus = (currentCargo_.type != CargoType::NONE) ? (currentCargo_.value * (currentCargo_.health / 100.0f)) : 0.0f;
        playerMoney_ += 500 + (int)cargoBonus;
        saveGame();
        gameState_ = GameState::SHOP;
        resetVehicle();
        return;
    }

    // Checkpoints Check
    for (auto& cp : checkpoints_) {
        if (!cp.reached && carZ_ > cp.z) {
            cp.reached = true;
            lastCheckpointX_ = carX_;
            lastCheckpointY_ = carY_;
            lastCheckpointZ_ = carZ_;
            lastCheckpointRotation_ = carRotation_;
            hasReachedAnyCheckpoint_ = true;
            playerMoney_ += 100;
            saveGame();
        }
    }

    // Collision detection
    for (const auto& obs : obstacles_) {
        float dx = carX_ - obs.x;
        float dz = carZ_ - obs.z;
        float distSq = dx*dx + dz*dz;
        if (distSq < 1.0f) { // Collision radius 1.0
            carSpeed_ = 0.0f;
            if (isDamageEnabled_) {
                vehicleHealth_ -= 20.0f;
                if (currentCargo_.type != CargoType::NONE) {
                    currentCargo_.health -= 15.0f * currentCargo_.fragility;
                    if (currentCargo_.health <= 0.0f) {
                        currentCargo_.health = 0.0f;
                        gameState_ = GameState::SHOP; // Fail mission
                        resetVehicle();
                        return;
                    }
                }
            }
            // Push back slightly
            carX_ -= sinf(carRotation_) * 0.2f;
            carZ_ -= cosf(carRotation_) * 0.2f;
            break;
        }
    }

    // Simple camera follow
    camX_ = carX_ - sinf(carRotation_) * 8.0f;
    camZ_ = carZ_ - cosf(carRotation_) * 8.0f;
    camY_ = 4.0f + carY_;

    // Smoke Particle Logic
    if (isDamageEnabled_ && vehicleHealth_ < 50.0f) {
        smokeSpawnTimer_++;
        // As health gets lower, spawn rate increases (threshold decreases)
        int spawnThreshold = (int)(vehicleHealth_ / 5.0f); // 50 health -> every 10 frames, 10 health -> every 2 frames
        if (spawnThreshold < 2) spawnThreshold = 2;

        if (smokeSpawnTimer_ >= spawnThreshold) {
            smokeSpawnTimer_ = 0;
            Particle p{};
            // Spawn at engine bay (slightly in front of car center)
            p.x = carX_ + sinf(carRotation_) * 0.5f;
            p.y = 0.8f;
            p.z = carZ_ + cosf(carRotation_) * 0.5f;
            p.life = 2.0f;
            p.maxLife = 2.0f;
            p.scale = 0.2f;
            particles_.push_back(p);
        }
    }

    // Update Particles
    for (auto it = particles_.begin(); it != particles_.end(); ) {
        it->y += 0.5f * dt; // Smoke rises
        it->x += ((float)rand() / RAND_MAX - 0.5f) * 0.2f * dt; // Random drift
        it->z += ((float)rand() / RAND_MAX - 0.5f) * 0.2f * dt;
        it->life -= dt;
        it->scale += 0.5f * dt; // Smoke expands

        if (it->life <= 0) {
            it = particles_.erase(it);
        } else {
            ++it;
        }
    }
}

void Renderer::render() {
    // Check to see if the surface has changed size. This is _necessary_ to do every frame when
    // using immersive mode as you'll get no other notification that your renderable area has
    // changed.
    updateRenderArea();

    // When the renderable area changes, the projection matrix has to also be updated. This is true
    // even if you change from the sample orthographic projection matrix as your aspect ratio has
    // likely changed.
    if (shaderNeedsNewProjectionMatrix_) {
        Utility::buildPerspectiveMatrix(
                projectionMatrix_,
                45.f,
                float(width_) / height_,
                0.1f,
                100.f);

        // make sure the matrix isn't generated every frame
        shaderNeedsNewProjectionMatrix_ = false;
    }

    // clear the color buffer
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    shader_->setColor(1.0f, 1.0f, 1.0f, 1.0f);

    // View matrix (Camera)
    float viewMatrix[16];
    float camTranslation[16];
    float camRotation[16];

    // To look towards +Z from a position at -Z:
    // 1. Translate by -cameraPos
    // 2. Rotate by 180 degrees around Y (to face +Z)
    // 3. Rotate by -carRotation (to follow car)
    Utility::buildTranslationMatrix(camTranslation, -camX_, -camY_, -camZ_);
    Utility::buildRotationYMatrix(camRotation, M_PI - carRotation_);
    Utility::multiplyMatrices(viewMatrix, camRotation, camTranslation);

    // Render Ground
    if (models_.size() >= 1) {
        float modelMatrix[16];
        float mvp[16];
        float tmp[16];

        // Ground is static at (0,0,0)
        Utility::buildIdentityMatrix(modelMatrix);
        Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
        Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
        shader_->setProjectionMatrix(mvp);
        shader_->drawModel(models_[0]);
    }

    // Render Mud Zones
    if (models_.size() > 10) {
        for (const auto& mud : mudZones_) {
            float modelMatrix[16];
            float scale[16];
            float translation[16];
            float mvp[16];
            float tmp[16];

            Utility::buildTranslationMatrix(translation, mud.x, 0.0f, mud.z);
            Utility::buildScaleMatrix(scale, mud.radius, 1.0f, mud.radius);
            Utility::multiplyMatrices(modelMatrix, translation, scale);

            Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
            Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
            shader_->setProjectionMatrix(mvp);
            shader_->setColor(0.3f, 0.2f, 0.1f, 0.8f); // Mud brown
            shader_->drawModel(models_[10]);
        }
    }

    // Render Slope Zones (Ramps)
    if (models_.size() > 11) {
        for (const auto& slope : slopeZones_) {
            float modelMatrix[16];
            float scale[16];
            float translation[16];
            float mvp[16];
            float tmp[16];

            Utility::buildTranslationMatrix(translation, slope.x, 0.0f, slope.z);
            Utility::buildScaleMatrix(scale, slope.width / 2.0f, slope.heightDelta, slope.length / 2.0f);
            Utility::multiplyMatrices(modelMatrix, translation, scale);

            Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
            Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
            shader_->setProjectionMatrix(mvp);
            shader_->setColor(0.6f, 0.6f, 0.6f, 1.0f); // Grey ramp
            shader_->drawModel(models_[11]);
        }
    }

    // Render Car
    if (models_.size() >= 2) {
        float modelMatrix[16];
        float translation[16];
        float rotY[16];
        float rotX[16];
        float mvp[16];
        float tmp[16];

        Utility::buildTranslationMatrix(translation, carX_, 0.5f + carY_, carZ_);
        Utility::buildRotationYMatrix(rotY, carRotation_);
        // Manual Pitch (Rotation X)
        Utility::buildIdentityMatrix(rotX);
        float s = sinf(carPitch_);
        float c = cosf(carPitch_);
        rotX[5] = c; rotX[6] = s; rotX[9] = -s; rotX[10] = c;

        Utility::multiplyMatrices(tmp, translation, rotY);
        Utility::multiplyMatrices(modelMatrix, tmp, rotX);

        Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
        Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
        shader_->setColor(1.0f, 1.0f, 1.0f, 1.0f);
        shader_->setProjectionMatrix(mvp);
        shader_->drawModel(models_[1]);

        // Render Addon if equipped
        if (currentVehicle_.currentAddonIndex != -1 && models_.size() > 5) {
            const auto& addon = addonCatalog_[currentVehicle_.currentAddonIndex];
            float addonModelMatrix[16];
            float addonOffset[16];
            float addonMvp[16];
            float addonTmp[16];

            Utility::buildIdentityMatrix(addonOffset);
            int modelIdx = 4; // Default to roof rack

            if (addon.type == AddonType::ROOF_RACK) {
                Utility::buildTranslationMatrix(addonOffset, 0.0f, 0.4f, 0.0f); // On top
                modelIdx = 4;
            } else if (addon.type == AddonType::TRAILER || addon.type == AddonType::SEMI_TRAILER) {
                Utility::buildTranslationMatrix(addonOffset, 0.0f, 0.0f, -2.5f); // Behind
                modelIdx = 5;
            } else if (addon.type == AddonType::BED_CARGO || addon.type == AddonType::LARGE_BED_CARGO) {
                Utility::buildTranslationMatrix(addonOffset, 0.0f, 0.2f, -0.5f); // In bed
                modelIdx = 4; // Reuse roof rack quad for cargo
            }

            // Combine car transform with addon offset
            Utility::multiplyMatrices(addonModelMatrix, modelMatrix, addonOffset);

            Utility::multiplyMatrices(addonTmp, viewMatrix, addonModelMatrix);
            Utility::multiplyMatrices(addonMvp, projectionMatrix_, addonTmp);
            shader_->setProjectionMatrix(addonMvp);
            shader_->drawModel(models_[modelIdx]);
        }

        // Render Cargo if any
        if (currentCargo_.type != CargoType::NONE && models_.size() > 9) {
            float cargoModelMatrix[16];
            float cargoOffset[16];
            float cargoMvp[16];
            float cargoTmp[16];

            int modelIdx = 6; // Default Box
            Utility::buildIdentityMatrix(cargoOffset);

            if (currentCargo_.type == CargoType::LIGHT_BOX) {
                modelIdx = 6;
                if (currentVehicle_.category == VehicleCategory::CAR)
                    Utility::buildTranslationMatrix(cargoOffset, 0.0f, 0.5f, 0.0f); // On rack
                else
                    Utility::buildTranslationMatrix(cargoOffset, 0.0f, 0.2f, -0.5f); // In bed
            } else if (currentCargo_.type == CargoType::MEDIUM_BARREL) {
                modelIdx = 7;
                if (currentVehicle_.currentAddonIndex != -1 && addonCatalog_[currentVehicle_.currentAddonIndex].type == AddonType::TRAILER)
                    Utility::buildTranslationMatrix(cargoOffset, 0.0f, 0.1f, -2.5f); // On trailer
                else
                    Utility::buildTranslationMatrix(cargoOffset, 0.0f, 0.2f, -0.5f); // In bed
            } else if (currentCargo_.type == CargoType::HEAVY_LOGS) {
                modelIdx = 8;
                Utility::buildTranslationMatrix(cargoOffset, 0.0f, 0.2f, -0.8f); // In truck bed
            } else if (currentCargo_.type == CargoType::SEMI_CONTAINER) {
                modelIdx = 9;
                Utility::buildTranslationMatrix(cargoOffset, 0.0f, 0.1f, -2.5f); // On semi trailer
            }

            Utility::multiplyMatrices(cargoModelMatrix, modelMatrix, cargoOffset);

            Utility::multiplyMatrices(cargoTmp, viewMatrix, cargoModelMatrix);
            Utility::multiplyMatrices(cargoMvp, projectionMatrix_, cargoTmp);
            shader_->setProjectionMatrix(cargoMvp);
            shader_->drawModel(models_[modelIdx]);
        }
    }

    // Render Obstacles (Trees)
    if (models_.size() >= 3) {
        for (const auto& obs : obstacles_) {
            float modelMatrix[16];
            float mvp[16];
            float tmp[16];

            Utility::buildTranslationMatrix(modelMatrix, obs.x, 0.0f, obs.z);
            Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
            Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
            shader_->setProjectionMatrix(mvp);
            shader_->drawModel(models_[2]);
        }
    }

    // Render Checkpoints
    if (models_.size() > 12) {
        for (const auto& cp : checkpoints_) {
            float modelMatrix[16];
            float mvp[16];
            float tmp[16];

            Utility::buildTranslationMatrix(modelMatrix, cp.x, 0.0f, cp.z);
            Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
            Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
            shader_->setProjectionMatrix(mvp);

            if (cp.reached) shader_->setColor(0.0f, 1.0f, 0.0f, 1.0f); // Green
            else shader_->setColor(1.0f, 1.0f, 0.0f, 1.0f); // Yellow

            shader_->drawModel(models_[12]);

            // Draw a second one slightly offset to make a "gate"
            Utility::buildTranslationMatrix(modelMatrix, cp.x + 5.0f, 0.0f, cp.z);
            Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
            Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
            shader_->setProjectionMatrix(mvp);
            shader_->drawModel(models_[12]);

            Utility::buildTranslationMatrix(modelMatrix, cp.x - 5.0f, 0.0f, cp.z);
            Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
            Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
            shader_->setProjectionMatrix(mvp);
            shader_->drawModel(models_[12]);
        }
    }

    // Render Fuel Stations
    if (models_.size() > 13) {
        for (const auto& station : fuelStations_) {
            float modelMatrix[16];
            float mvp[16];
            float tmp[16];

            Utility::buildTranslationMatrix(modelMatrix, station.x, 0.0f, station.z);
            Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
            Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
            shader_->setProjectionMatrix(mvp);
            shader_->setColor(1.0f, 0.2f, 0.2f, 1.0f); // Red pump
            shader_->drawModel(models_[13]);
        }
    }

    // Render Smoke Particles
    if (models_.size() >= 4 && !particles_.empty()) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE); // Don't write to depth buffer for particles

        for (const auto& p : particles_) {
            float modelMatrix[16];
            float translation[16];
            float scale[16];
            float mvp[16];
            float tmp[16];

            Utility::buildTranslationMatrix(translation, p.x, p.y, p.z);
            Utility::buildScaleMatrix(scale, p.scale, p.scale, p.scale);
            Utility::multiplyMatrices(modelMatrix, translation, scale);

            Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
            Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
            shader_->setProjectionMatrix(mvp);

            float alpha = p.life / p.maxLife;
            shader_->setColor(0.3f, 0.3f, 0.3f, alpha);
            shader_->drawModel(models_[3]); // Use UI quad model
        }
        glDepthMask(GL_TRUE);
    }

    // Render Winch Cable
    if (isWinchAttached_ && winchAnchorIndex_ >= 0 && winchAnchorIndex_ < obstacles_.size() && models_.size() >= 4) {
        const auto& anchor = obstacles_[winchAnchorIndex_];
        float carFrontX = carX_ + sinf(carRotation_) * 1.0f;
        float carFrontZ = carZ_ + cosf(carRotation_) * 1.0f;

        float dx = anchor.x - carFrontX;
        float dz = anchor.z - carFrontZ;
        float dist = sqrtf(dx * dx + dz * dz);
        float angle = atan2f(dx, dz);

        float modelMatrix[16];
        float translation[16];
        float rotation[16];
        float scale[16];
        float mvp[16];
        float tmp[16];

        // Position at midpoint between car and anchor
        Utility::buildTranslationMatrix(translation, (carFrontX + anchor.x) / 2.0f, 0.5f, (carFrontZ + anchor.z) / 2.0f);
        Utility::buildRotationYMatrix(rotation, angle);
        Utility::buildScaleMatrix(scale, 0.05f, 0.05f, dist / 2.0f); // Thin and long

        Utility::multiplyMatrices(tmp, translation, rotation);
        Utility::multiplyMatrices(modelMatrix, tmp, scale);

        Utility::multiplyMatrices(tmp, viewMatrix, modelMatrix);
        Utility::multiplyMatrices(mvp, projectionMatrix_, tmp);
        shader_->setProjectionMatrix(mvp);
        shader_->setColor(0.2f, 0.2f, 0.2f, 1.0f); // Dark grey cable
        shader_->drawModel(models_[3]); // Use UI quad model as a line/thin quad
    }

    // Render UI
    renderUI();

    // Present the rendered image. This is an implicit glFlush.
    auto swapResult = eglSwapBuffers(display_, surface_);
    assert(swapResult == EGL_TRUE);
}

void Renderer::drawStyledButton(float x, float y, float w, float h, float r, float g, float b, float a, bool pressed) {
    float ortho[16];
    Utility::buildIdentityMatrix(ortho);
    ortho[0] = 2.0f / (float)width_;
    ortho[5] = -2.0f / (float)height_;
    ortho[12] = -1.0f;
    ortho[13] = 1.0f;

    auto drawQuad = [&](float qx, float qy, float qw, float qh, float qr, float qg, float qb, float qa) {
        float modelMatrix[16];
        float mvp[16];
        Utility::buildIdentityMatrix(modelMatrix);
        modelMatrix[0] = qw / 2.0f;
        modelMatrix[5] = qh / 2.0f;
        modelMatrix[12] = qx;
        modelMatrix[13] = qy;
        Utility::multiplyMatrices(mvp, ortho, modelMatrix);
        shader_->setProjectionMatrix(mvp);
        shader_->setColor(qr, qg, qb, qa);
        shader_->drawModel(models_[3]);
    };

    float scale = pressed ? 0.9f : 1.0f;
    float sw = w * scale;
    float sh = h * scale;
    drawQuad(x, y, sw + 4.0f, sh + 4.0f, 0.05f, 0.05f, 0.05f, a); // Border
    drawQuad(x, y, sw, sh, r, g, b, a); // Center
}

void Renderer::drawProgressBar(float x, float y, float w, float h, float progress, float r, float g, float b) {
    float ortho[16];
    Utility::buildIdentityMatrix(ortho);
    ortho[0] = 2.0f / (float)width_;
    ortho[5] = -2.0f / (float)height_;
    ortho[12] = -1.0f;
    ortho[13] = 1.0f;

    auto drawQuad = [&](float qx, float qy, float qw, float qh, float qr, float qg, float qb, float qa) {
        float modelMatrix[16];
        float mvp[16];
        Utility::buildIdentityMatrix(modelMatrix);
        modelMatrix[0] = qw / 2.0f;
        modelMatrix[5] = qh / 2.0f;
        modelMatrix[12] = qx;
        modelMatrix[13] = qy;
        Utility::multiplyMatrices(mvp, ortho, modelMatrix);
        shader_->setProjectionMatrix(mvp);
        shader_->setColor(qr, qg, qb, qa);
        shader_->drawModel(models_[3]);
    };

    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    drawQuad(x, y, w + 4.0f, h + 4.0f, 0.1f, 0.1f, 0.1f, 0.8f); // BG
    drawQuad(x - (w * (1.0f - progress)) / 2.0f, y, w * progress, h, r, g, b, 1.0f); // Fill
}

void Renderer::renderUI() {
    if (models_.size() < 4) return;

    glDisable(GL_DEPTH_TEST);

    float ortho[16];
    Utility::buildIdentityMatrix(ortho);
    ortho[0] = 2.0f / (float)width_;
    ortho[5] = -2.0f / (float)height_;
    ortho[12] = -1.0f;
    ortho[13] = 1.0f;

    auto drawQuad = [&](float qx, float qy, float qw, float qh, float qr = 1.0f, float qg = 1.0f, float qb = 1.0f, float qa = 1.0f) {
        float modelMatrix[16];
        float mvp[16];
        Utility::buildIdentityMatrix(modelMatrix);
        modelMatrix[0] = qw / 2.0f;
        modelMatrix[5] = qh / 2.0f;
        modelMatrix[12] = qx;
        modelMatrix[13] = qy;
        Utility::multiplyMatrices(mvp, ortho, modelMatrix);
        shader_->setProjectionMatrix(mvp);
        shader_->setColor(qr, qg, qb, qa);
        shader_->drawModel(models_[3]);
    };

    if (gameState_ == GameState::GAMEPLAY) {
        // Integrated Dashboard at the top
        float dashY = 40.0f;
        float dashW = 600.0f;
        float dashX = (float)width_ / 2.0f;
        drawQuad(dashX, dashY, dashW, 60.0f, 0.1f, 0.1f, 0.1f, 0.7f); // Background panel

        // RPM Bar (inside dashboard)
        float rpmNorm = vehicleState_.rpm / 7000.0f;
        float rpmR = (rpmNorm > 0.9f) ? 1.0f : ((rpmNorm > 0.7f) ? 1.0f : 0.0f);
        float rpmG = (rpmNorm > 0.9f) ? 0.0f : 1.0f;
        drawProgressBar(dashX - 140.0f, dashY, 250.0f, 30.0f, rpmNorm, rpmR, rpmG, 0.0f);

        // Speed Display (center of dash)
        float speedKph = fabsf(carSpeed_ * 3.6f);
        float speedNorm = speedKph / 120.0f;
        drawProgressBar(dashX + 140.0f, dashY, 250.0f, 30.0f, speedNorm, 0.0f, 0.6f, 1.0f);

        // Mission Progress Bar (Top Center)
        float progress = carZ_ / 45.0f;
        drawProgressBar((float)width_ / 2.0f, 90.0f, 400.0f, 15.0f, progress, 1.0f, 0.8f, 0.0f);

        // Steering Wheel Icon / Context
        if (useTilt_) {
            float wheelX = 150.0f;
            float wheelY = (float)height_ - 150.0f;
            // Background circle-ish
            drawQuad(wheelX, wheelY, 120.0f, 120.0f, 1.0f, 1.0f, 1.0f, 0.1f);
            // Steering indicator (moving dot or bar)
            float tiltOffset = -tilt_ * 10.0f;
            drawQuad(wheelX + tiltOffset, wheelY, 20.0f, 60.0f, 1.0f, 1.0f, 1.0f, 0.5f);
        }

        // Health Bar (Right Side)
        drawProgressBar((float)width_ - 150.0f, 40.0f, 200.0f, 20.0f, vehicleHealth_ / 100.0f, 1.0f, 0.2f, 0.2f);

        // Fuel Bar (Below Health)
        float fuelNorm = currentFuel_ / fuelCapacity_;
        float fuelR = (fuelNorm < 0.2f) ? 1.0f : 1.0f;
        float fuelG = (fuelNorm < 0.2f) ? 0.0f : 0.8f;
        drawProgressBar((float)width_ - 150.0f, 70.0f, 200.0f, 20.0f, fuelNorm, fuelR, fuelG, 0.0f);

        // Reset indicator if dead
        if (vehicleHealth_ <= 0.0f) {
            drawStyledButton((float)width_ / 2.0f, (float)height_ / 2.0f, 400.0f, 100.0f, 0.8f, 0.1f, 0.1f, 0.9f, false);
        }

        // Controls
        drawQuad(80.0f, (float)height_ / 2.0f, 40.0f, 400.0f, 1.0f, 1.0f, 1.0f, 0.2f); // Gear
        float gearY = (float)height_ / 2.0f + 200.0f - (vehicleState_.currentGear + 1) * (400.0f / 7.0f);
        drawQuad(80.0f, gearY, 60.0f, 15.0f, 1.0f, 1.0f, 1.0f, 1.0f);

        drawQuad((float)width_ - 80.0f, (float)height_ / 2.0f, 40.0f, 400.0f, 1.0f, 1.0f, 1.0f, 0.2f); // Accel
        float accelY = (float)height_ / 2.0f - acceleratorValue_ * 200.0f;
        drawQuad((float)width_ - 80.0f, accelY, 60.0f, 15.0f, 1.0f, 1.0f, 1.0f, 1.0f);

        // Status Icons (4x4, Diff, TC) - Distinct Icons
        float iconY = (float)height_ - 80.0f;
        float centerX = (float)width_ / 2.0f;

        // 4x4
        drawStyledButton(centerX - 120.0f, iconY, 70.0f, 70.0f,
                         vehicleState_.is4x4Enabled ? 0.2f : 0.1f,
                         vehicleState_.is4x4Enabled ? 0.9f : 0.1f,
                         0.2f, 1.0f, pressedButtonId_ == 1);
        // Small 4x4 text simulation
        drawQuad(centerX - 120.0f, iconY, 40.0f, 10.0f, 1.0f, 1.0f, 1.0f, 0.8f);

        // Diff Lock
        drawStyledButton(centerX, iconY, 70.0f, 70.0f,
                         vehicleState_.isDiffLockEnabled ? 0.9f : 0.1f,
                         vehicleState_.isDiffLockEnabled ? 0.5f : 0.1f,
                         0.1f, 1.0f, pressedButtonId_ == 2);
        // X mark simulation
        drawQuad(centerX, iconY, 10.0f, 40.0f, 1.0f, 1.0f, 1.0f, 0.8f);

        // TC
        drawStyledButton(centerX + 120.0f, iconY, 70.0f, 70.0f,
                         vehicleState_.isTCEnabled ? 0.2f : 0.1f,
                         0.2f,
                         vehicleState_.isTCEnabled ? 0.9f : 0.1f, 1.0f, pressedButtonId_ == 3);

        // Winch & Pull
        drawStyledButton(centerX - 240.0f, iconY, 70.0f, 70.0f, 0.4f, 0.4f, 0.4f, 1.0f, pressedButtonId_ == 4);
        if (isWinchAttached_) {
            drawStyledButton(centerX + 240.0f, iconY, 70.0f, 70.0f,
                             isWinching_ ? 0.8f : 0.4f, 0.5f, 0.0f, 1.0f, pressedButtonId_ == 5);
        }

        // Cargo Health
        if (currentCargo_.type != CargoType::NONE) {
            drawProgressBar((float)width_ / 2.0f, 130.0f, 300.0f, 10.0f, currentCargo_.health / 100.0f, 0.0f, 1.0f, 0.5f);
        }

    } else if (gameState_ == GameState::SHOP) {
        // Clean Shop Background
        drawQuad((float)width_ / 2.0f, (float)height_ / 2.0f, (float)width_, (float)height_, 0.05f, 0.05f, 0.1f, 1.0f);

        // Header: Current Balance
        drawQuad((float)width_ / 2.0f, 50.0f, (float)width_, 100.0f, 0.1f, 0.1f, 0.15f, 1.0f);
        drawQuad(150.0f, 50.0f, 200.0f, 40.0f, 1.0f, 0.8f, 0.0f, 1.0f); // Money icon

        // Main Card for Vehicle
        float cardX = (float)width_ / 2.0f;
        float cardY = (float)height_ / 2.0f - 50.0f;
        drawQuad(cardX, cardY, 800.0f, 500.0f, 0.15f, 0.15f, 0.2f, 1.0f); // Card BG
        drawQuad(cardX, cardY - 200.0f, 760.0f, 60.0f, 0.25f, 0.25f, 0.3f, 1.0f); // Name box

        const auto& v = vehicleCatalog_[currentVehicleIndex_];

        // Vehicle Stats Bars
        float statsX = cardX - 180.0f;
        float statsY = cardY + 20.0f;
        // Power
        drawProgressBar(statsX, statsY, 300.0f, 20.0f, v.engineTorque / 600.0f, 1.0f, 0.4f, 0.0f);
        // Durability
        drawProgressBar(statsX, statsY + 60.0f, 300.0f, 20.0f, v.weight / 10000.0f, 0.4f, 0.8f, 1.0f);
        // Utility
        float utilityNorm = (float)v.supportedAddonTypes.size() / 3.0f;
        drawProgressBar(statsX, statsY + 120.0f, 300.0f, 20.0f, utilityNorm, 0.5f, 1.0f, 0.5f);

        // Next/Prev Buttons (Simplified to just Next)
        drawStyledButton(cardX + 250.0f, cardY + 50.0f, 200.0f, 80.0f, 0.3f, 0.3f, 0.6f, 1.0f, pressedButtonId_ == 9);

        // Accessories (Icons in the card)
        float addonX = cardX - 350.0f;
        float addonY = cardY + 200.0f;
        int count = 0;
        for (int i = 0; i < addonCatalog_.size(); ++i) {
            bool supported = false;
            for (auto type : v.supportedAddonTypes) if (type == addonCatalog_[i].type) { supported = true; break; }
            if (supported) {
                float ax = addonX + count * 100.0f;
                bool equipped = (v.currentAddonIndex == i);
                drawStyledButton(ax, addonY, 80.0f, 80.0f, equipped ? 0.0f : 0.2f, equipped ? 0.8f : 0.2f, 0.2f, 1.0f, false);
                count++;
            }
        }

        // Shop Buttons (Bottom Right)
        drawStyledButton(cardX + 250.0f, cardY - 50.0f, 200.0f, 60.0f, 0.2f, 0.7f, 0.2f, 1.0f, pressedButtonId_ == 8); // Upgrade Torque
        drawStyledButton(cardX + 250.0f, cardY - 120.0f, 200.0f, 60.0f, 0.7f, 0.2f, 0.2f, 1.0f, pressedButtonId_ == 7); // Repair
        drawStyledButton(cardX + 250.0f, cardY - 190.0f, 200.0f, 60.0f, 0.2f, 0.5f, 0.8f, 1.0f, pressedButtonId_ == 10); // Refuel
        drawStyledButton(cardX + 250.0f, cardY - 260.0f, 200.0f, 60.0f, 0.6f, 0.6f, 0.1f, 1.0f, pressedButtonId_ == 11); // Fuel Cap

        // Mission Select (Bottom strip)
        float missionY = (float)height_ - 180.0f;
        for (int i = 1; i <= 4; ++i) {
            float mx = (float)width_ / 2.0f - 375.0f + (i-1) * 250.0f;
            bool selected = (currentCargo_.type == (CargoType)i);
            drawStyledButton(mx, missionY, 220.0f, 60.0f, selected ? 0.8f : 0.2f, selected ? 0.7f : 0.2f, 0.1f, 1.0f, false);
        }

        // BIG START BUTTON
        if (v.isPurchased) {
            drawStyledButton((float)width_ / 2.0f, (float)height_ - 70.0f, 600.0f, 100.0f, 0.0f, 0.8f, 0.2f, 1.0f, pressedButtonId_ == 6);
        } else {
            drawStyledButton((float)width_ / 2.0f, (float)height_ - 70.0f, 600.0f, 100.0f, 1.0f, 0.5f, 0.0f, 1.0f, pressedButtonId_ == 6);
        }
    }

    glEnable(GL_DEPTH_TEST);
}

void Renderer::initRenderer() {
    // Choose your render attributes
    constexpr EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE
    };

    // The default display is probably what you want on Android
    auto display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, nullptr, nullptr);

    // figure out how many configs there are
    EGLint numConfigs;
    eglChooseConfig(display, attribs, nullptr, 0, &numConfigs);

    // get the list of configurations
    std::unique_ptr<EGLConfig[]> supportedConfigs(new EGLConfig[numConfigs]);
    eglChooseConfig(display, attribs, supportedConfigs.get(), numConfigs, &numConfigs);

    // Find a config we like.
    // Could likely just grab the first if we don't care about anything else in the config.
    // Otherwise hook in your own heuristic
    auto config = *std::find_if(
            supportedConfigs.get(),
            supportedConfigs.get() + numConfigs,
            [&display](const EGLConfig &config) {
                EGLint red, green, blue, depth;
                if (eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red)
                    && eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green)
                    && eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue)
                    && eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE, &depth)) {

                    aout << "Found config with " << red << ", " << green << ", " << blue << ", "
                         << depth << std::endl;
                    return red == 8 && green == 8 && blue == 8 && depth == 24;
                }
                return false;
            });

    aout << "Found " << numConfigs << " configs" << std::endl;
    aout << "Chose " << config << std::endl;

    // create the proper window surface
    EGLint format;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);
    EGLSurface surface = eglCreateWindowSurface(display, config, app_->window, nullptr);

    // Create a GLES 3 context
    EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, nullptr, contextAttribs);

    // get some window metrics
    auto madeCurrent = eglMakeCurrent(display, surface, surface, context);
    assert(madeCurrent);

    display_ = display;
    surface_ = surface;
    context_ = context;

    // make width and height invalid so it gets updated the first frame in @a updateRenderArea()
    width_ = -1;
    height_ = -1;

    PRINT_GL_STRING(GL_VENDOR);
    PRINT_GL_STRING(GL_RENDERER);
    PRINT_GL_STRING(GL_VERSION);
    PRINT_GL_STRING_AS_LIST(GL_EXTENSIONS);

    shader_ = std::unique_ptr<Shader>(
            Shader::loadShader(vertex, fragment, "inPosition", "inUV", "uMVP", "uColor"));
    assert(shader_);

    // Note: there's only one shader in this demo, so I'll activate it here. For a more complex game
    // you'll want to track the active shader and activate/deactivate it as necessary
    shader_->activate();

    // setup any other gl related global states
    glClearColor(0.53f, 0.81f, 0.92f, 1.0f); // Sky Blue

    // enable alpha globally for now, you probably don't want to do this in a game
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // get some demo models into memory
    srand(time(nullptr));
    createModels();
}

void Renderer::updateRenderArea() {
    EGLint width;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &width);

    EGLint height;
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &height);

    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        glViewport(0, 0, width, height);

        // make sure that we lazily recreate the projection matrix before we render
        shaderNeedsNewProjectionMatrix_ = true;
    }
}

void Renderer::createModels() {
    auto assetManager = app_->activity->assetManager;
    // Suggestion: Load "grass.png" here for the ground if available
    auto spTexture = TextureAsset::loadAsset(assetManager, "android_robot.png");

    std::vector<Index> indices = {
            0, 1, 2, 0, 2, 3
    };

    // Model 0: Ground
    std::vector<Vertex> groundVertices = {
            Vertex(Vector3{50.f, 0.f, 50.f}, Vector2{0.f, 0.f}),
            Vertex(Vector3{-50.f, 0.f, 50.f}, Vector2{20.f, 0.f}),
            Vertex(Vector3{-50.f, 0.f, -50.f}, Vector2{20.f, 20.f}),
            Vertex(Vector3{50.f, 0.f, -50.f}, Vector2{0.f, 20.f})
    };
    models_.emplace_back(groundVertices, indices, spTexture);

    // Model 1: Car
    std::vector<Vertex> carVertices = {
            Vertex(Vector3{0.5f, 0.f, 1.f}, Vector2{0.f, 0.f}),
            Vertex(Vector3{-0.5f, 0.f, 1.f}, Vector2{1.f, 0.f}),
            Vertex(Vector3{-0.5f, 0.f, -1.f}, Vector2{1.f, 1.f}),
            Vertex(Vector3{0.5f, 0.f, -1.f}, Vector2{0.f, 1.f})
    };
    models_.emplace_back(carVertices, indices, spTexture);

    // Model 2: Tree (Crossing Quads)
    std::vector<Vertex> treeVertices = {
            // Quad 1
            Vertex(Vector3{0.5f, 0.f, 0.f}, Vector2{0.f, 0.f}),
            Vertex(Vector3{-0.5f, 0.f, 0.f}, Vector2{1.f, 0.f}),
            Vertex(Vector3{-0.5f, 2.f, 0.f}, Vector2{1.f, 1.f}),
            Vertex(Vector3{0.5f, 2.f, 0.f}, Vector2{0.f, 1.f}),
            // Quad 2
            Vertex(Vector3{0.f, 0.f, 0.5f}, Vector2{0.f, 0.f}),
            Vertex(Vector3{0.f, 0.f, -0.5f}, Vector2{1.f, 0.f}),
            Vertex(Vector3{0.f, 2.f, -0.5f}, Vector2{1.f, 1.f}),
            Vertex(Vector3{0.f, 2.f, 0.5f}, Vector2{0.f, 1.f})
    };
    std::vector<Index> treeIndices = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7
    };
    models_.emplace_back(treeVertices, treeIndices, spTexture);

    // Generate random obstacles
    for (int i = 0; i < 30; ++i) {
        float x = (static_cast<float>(rand()) / RAND_MAX) * 80.0f - 40.0f;
        float z = (static_cast<float>(rand()) / RAND_MAX) * 80.0f - 40.0f;
        // Keep clear of starting area
        if (sqrtf(x * x + z * z) > 5.0f) {
            obstacles_.push_back({x, z});
        }
    }

    // Model 3: UI Quad (-1 to 1)
    std::vector<Vertex> uiVertices = {
            Vertex(Vector3{-1.f, -1.f, 0.f}, Vector2{0.f, 0.f}),
            Vertex(Vector3{1.f, -1.f, 0.f}, Vector2{1.f, 0.f}),
            Vertex(Vector3{1.f, 1.f, 0.f}, Vector2{1.f, 1.f}),
            Vertex(Vector3{-1.f, 1.f, 0.f}, Vector2{0.f, 1.f})
    };
    models_.emplace_back(uiVertices, indices, spTexture);

    // Model 4: Roof Rack (Small quad on top)
    std::vector<Vertex> roofRackVertices = {
            Vertex(Vector3{0.4f, 0.f, 0.4f}, Vector2{0.f, 0.f}),
            Vertex(Vector3{-0.4f, 0.f, 0.4f}, Vector2{1.f, 0.f}),
            Vertex(Vector3{-0.4f, 0.f, -0.4f}, Vector2{1.f, 1.f}),
            Vertex(Vector3{0.4f, 0.f, -0.4f}, Vector2{0.f, 1.f})
    };
    models_.emplace_back(roofRackVertices, indices, spTexture);

    // Model 5: Trailer (Quad with hitch offset)
    std::vector<Vertex> trailerVertices = {
            Vertex(Vector3{0.6f, 0.f, 1.0f}, Vector2{0.f, 0.f}),
            Vertex(Vector3{-0.6f, 0.f, 1.0f}, Vector2{1.f, 0.f}),
            Vertex(Vector3{-0.6f, 0.f, -1.0f}, Vector2{1.f, 1.f}),
            Vertex(Vector3{0.6f, 0.f, -1.0f}, Vector2{0.f, 1.f})
    };
    models_.emplace_back(trailerVertices, indices, spTexture);

    // Model 6: Box (Small cube-like)
    std::vector<Vertex> boxVertices = {
            Vertex(Vector3{0.3f, 0.0f, 0.3f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.3f, 0.0f, 0.3f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.3f, 0.6f, 0.3f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.3f, 0.6f, 0.3f}, Vector2{0.0f, 1.0f}),

            Vertex(Vector3{0.3f, 0.0f, -0.3f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.3f, 0.0f, -0.3f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.3f, 0.6f, -0.3f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.3f, 0.6f, -0.3f}, Vector2{0.0f, 1.0f})
    };
    std::vector<Index> cubeIndices = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    models_.emplace_back(boxVertices, cubeIndices, spTexture);

    // Model 7: Barrel (Cylinder-ish)
    std::vector<Vertex> barrelVertices = {
            Vertex(Vector3{0.3f, 0.0f, 0.0f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.3f, 0.0f, 0.0f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.3f, 0.8f, 0.0f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.3f, 0.8f, 0.0f}, Vector2{0.0f, 1.0f}),

            Vertex(Vector3{0.0f, 0.0f, 0.3f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{0.0f, 0.0f, -0.3f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{0.0f, 0.8f, -0.3f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.0f, 0.8f, 0.3f}, Vector2{0.0f, 1.0f})
    };
    models_.emplace_back(barrelVertices, cubeIndices, spTexture);

    // Model 8: Logs (Long cylinders)
    std::vector<Vertex> logVertices = {
            Vertex(Vector3{0.8f, 0.0f, 0.2f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.8f, 0.0f, 0.2f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.8f, 0.3f, 0.2f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.8f, 0.3f, 0.2f}, Vector2{0.0f, 1.0f}),

            Vertex(Vector3{0.8f, 0.3f, -0.2f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.8f, 0.3f, -0.2f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.8f, 0.6f, -0.2f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.8f, 0.6f, -0.2f}, Vector2{0.0f, 1.0f})
    };
    models_.emplace_back(logVertices, cubeIndices, spTexture);

    // Model 9: Container (Large box)
    std::vector<Vertex> containerVertices = {
            Vertex(Vector3{0.6f, 0.0f, 1.5f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.6f, 0.0f, 1.5f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.6f, 1.2f, 1.5f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.6f, 1.2f, 1.5f}, Vector2{0.0f, 1.0f}),

            Vertex(Vector3{0.6f, 0.0f, -1.5f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.6f, 0.0f, -1.5f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.6f, 1.2f, -1.5f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.6f, 1.2f, -1.5f}, Vector2{0.0f, 1.0f})
    };
    models_.emplace_back(containerVertices, cubeIndices, spTexture);

    // Model 10: Mud Zone (Flat quad slightly above ground)
    std::vector<Vertex> mudVertices = {
            Vertex(Vector3{1.0f, 0.01f, 1.0f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-1.0f, 0.01f, 1.0f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-1.0f, 0.01f, -1.0f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{1.0f, 0.01f, -1.0f}, Vector2{0.0f, 1.0f})
    };
    models_.emplace_back(mudVertices, indices, spTexture);

    // Model 11: Ramp (Sloped quad)
    std::vector<Vertex> rampVertices = {
            Vertex(Vector3{1.0f, 1.0f, 1.0f}, Vector2{0.0f, 0.0f}), // Top Front
            Vertex(Vector3{-1.0f, 1.0f, 1.0f}, Vector2{1.0f, 0.0f}), // Top Front
            Vertex(Vector3{-1.0f, 0.0f, -1.0f}, Vector2{1.0f, 1.0f}), // Bottom Back
            Vertex(Vector3{1.0f, 0.0f, -1.0f}, Vector2{0.0f, 1.0f})  // Bottom Back
    };
    models_.emplace_back(rampVertices, indices, spTexture);

    // Model 12: Checkpoint (Thin tall quad)
    std::vector<Vertex> cpVertices = {
            Vertex(Vector3{0.1f, 0.0f, 0.0f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.1f, 0.0f, 0.0f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.1f, 4.0f, 0.0f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.1f, 4.0f, 0.0f}, Vector2{0.0f, 1.0f})
    };
    models_.emplace_back(cpVertices, indices, spTexture);

    // Model 13: Fuel Pump (Box/Quad)
    std::vector<Vertex> pumpVertices = {
            Vertex(Vector3{0.4f, 0.0f, 0.4f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.4f, 0.0f, 0.4f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.4f, 1.5f, 0.4f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.4f, 1.5f, 0.4f}, Vector2{0.0f, 1.0f}),

            Vertex(Vector3{0.4f, 0.0f, -0.4f}, Vector2{0.0f, 0.0f}),
            Vertex(Vector3{-0.4f, 0.0f, -0.4f}, Vector2{1.0f, 0.0f}),
            Vertex(Vector3{-0.4f, 1.5f, -0.4f}, Vector2{1.0f, 1.0f}),
            Vertex(Vector3{0.4f, 1.5f, -0.4f}, Vector2{0.0f, 1.0f})
    };
    models_.emplace_back(pumpVertices, cubeIndices, spTexture);
}

void Renderer::handleInput() {
    // handle all queued inputs
    auto *inputBuffer = android_app_swap_input_buffers(app_);
    if (!inputBuffer) {
        // no inputs yet.
        return;
    }

    // handle motion events (motionEventsCounts can be 0).
    for (auto i = 0; i < inputBuffer->motionEventsCount; i++) {
        auto &motionEvent = inputBuffer->motionEvents[i];
        auto action = motionEvent.action & AMOTION_EVENT_ACTION_MASK;

        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_MOVE) {
            for (auto index = 0; index < motionEvent.pointerCount; index++) {
                auto &pointer = motionEvent.pointers[index];
                auto x = GameActivityPointerAxes_getX(&pointer);
                auto y = GameActivityPointerAxes_getY(&pointer);

                if (gameState_ == GameState::GAMEPLAY) {
                    // Gear Slider (left): x in [50, 150], y in [height/2 - 200, height/2 + 200]
                    if (x > 50.0f && x < 150.0f && y > (float)height_ / 2.0f - 200.0f && y < (float)height_ / 2.0f + 200.0f) {
                        float normY = ((float)height_ / 2.0f + 200.0f - y) / 400.0f; // 0 to 1
                        int gear = (int)(normY * 6.99f) - 1; // -1 to 5
                        if (gear < -1) gear = -1;
                        if (gear > 5) gear = 5;
                        vehicleState_.currentGear = gear;
                    }

                    // Accelerator Slider (right): x in [width - 150, width - 50]
                    if (x > (float)width_ - 150.0f && x < (float)width_ - 50.0f && y > (float)height_ / 2.0f - 200.0f && y < (float)height_ / 2.0f + 200.0f) {
                        acceleratorValue_ = ((float)height_ / 2.0f - y) / 200.0f; // -1 to 1
                        if (acceleratorValue_ > 1.0f) acceleratorValue_ = 1.0f;
                        if (acceleratorValue_ < -1.0f) acceleratorValue_ = -1.0f;
                    }

                    // Buttons (taps)
                    if (action == AMOTION_EVENT_ACTION_DOWN) {
                        float btnY = (float)height_ - 100.0f;
                        if (y > btnY - 40.0f && y < btnY + 40.0f) {
                            // 4x4
                            if (x > (float)width_ / 2.0f - 190.0f && x < (float)width_ / 2.0f - 110.0f) {
                                toggle4x4();
                                pressedButtonId_ = 1; buttonFeedbackTimer_ = 0.1f;
                            } else if (x > (float)width_ / 2.0f - 40.0f && x < (float)width_ / 2.0f + 40.0f) {
                                toggleDiffLock();
                                pressedButtonId_ = 2; buttonFeedbackTimer_ = 0.1f;
                            } else if (x > (float)width_ / 2.0f + 110.0f && x < (float)width_ / 2.0f + 190.0f) {
                                toggleTC();
                                pressedButtonId_ = 3; buttonFeedbackTimer_ = 0.1f;
                            } else if (x > (float)width_ / 2.0f - 340.0f && x < (float)width_ / 2.0f - 260.0f) {
                                // Winch Button Tapped
                                pressedButtonId_ = 4; buttonFeedbackTimer_ = 0.1f;
                                if (!isWinchAttached_) {
                                    // Search for closest obstacle in front
                                    float carFrontX = carX_ + sinf(carRotation_) * 1.0f;
                                    float carFrontZ = carZ_ + cosf(carRotation_) * 1.0f;
                                    float closestDistSq = maxWinchDistance_ * maxWinchDistance_;
                                    int bestIdx = -1;

                                    for (int j = 0; j < obstacles_.size(); ++j) {
                                        float dx = obstacles_[j].x - carFrontX;
                                        float dz = obstacles_[j].z - carFrontZ;
                                        float distSq = dx * dx + dz * dz;

                                        if (distSq < closestDistSq) {
                                            // Check if it's in front (dot product)
                                            float dot = dx * sinf(carRotation_) + dz * cosf(carRotation_);
                                            if (dot > 0) {
                                                closestDistSq = distSq;
                                                bestIdx = j;
                                            }
                                        }
                                    }

                                    if (bestIdx != -1) {
                                        isWinchAttached_ = true;
                                        winchAnchorIndex_ = bestIdx;
                                        isWinching_ = false;
                                    }
                                } else {
                                    // Detach
                                    isWinchAttached_ = false;
                                    isWinching_ = false;
                                    winchAnchorIndex_ = -1;
                                }
                            } else if (x > (float)width_ / 2.0f + 260.0f && x < (float)width_ / 2.0f + 340.0f) {
                                // Pull Button Tapped
                                if (isWinchAttached_) {
                                    isWinching_ = !isWinching_;
                                    pressedButtonId_ = 5; buttonFeedbackTimer_ = 0.1f;
                                }
                            }
                        }

                        // Reset if dead
                        if (vehicleHealth_ <= 0.0f) {
                            if (x > (float)width_ / 2.0f - 200.0f && x < (float)width_ / 2.0f + 200.0f &&
                                y > (float)height_ / 2.0f - 50.0f && y < (float)height_ / 2.0f + 50.0f) {
                                resetVehicle();
                            }
                        }
                    }
                } else if (gameState_ == GameState::SHOP) {
                    if (action == AMOTION_EVENT_ACTION_DOWN) {
                        // Check Addon Buttons
                        float addonY = (float)height_ / 2.0f + 150.0f; // Adjusted for new card layout
                        // Note: addon layout changed in renderUI, adjusting handleInput
                        float cardX = (float)width_ / 2.0f;
                        float cardY = (float)height_ / 2.0f - 50.0f;
                        float startAddonX = cardX - 350.0f;
                        float startAddonY = cardY + 200.0f;

                        if (y > startAddonY - 40.0f && y < startAddonY + 40.0f) {
                            int count = 0;
                            for (int i = 0; i < addonCatalog_.size(); ++i) {
                                bool supported = false;
                                for (auto type : vehicleCatalog_[currentVehicleIndex_].supportedAddonTypes) if (type == addonCatalog_[i].type) { supported = true; break; }
                                if (supported) {
                                    float ax = startAddonX + count * 100.0f;
                                    if (x > ax - 40.0f && x < ax + 40.0f) {
                                        purchaseAddon(i);
                                    }
                                    count++;
                                }
                            }
                        }

                        // Buy Upgrade
                        if (x > cardX + 150.0f && x < cardX + 350.0f) {
                            if (y > cardY - 80.0f && y < cardY - 20.0f) {
                                buyUpgradeTorque();
                                pressedButtonId_ = 8; buttonFeedbackTimer_ = 0.1f;
                            }
                            // Repair
                            else if (y > cardY - 150.0f && y < cardY - 90.0f) {
                                repairVehicle();
                                pressedButtonId_ = 7; buttonFeedbackTimer_ = 0.1f;
                            }
                            // Refuel
                            else if (y > cardY - 220.0f && y < cardY - 160.0f) {
                                refuelFullTank();
                                pressedButtonId_ = 10; buttonFeedbackTimer_ = 0.1f;
                            }
                            // Fuel Capacity Upgrade
                            else if (y > cardY - 290.0f && y < cardY - 230.0f) {
                                upgradeFuelCapacity();
                                pressedButtonId_ = 11; buttonFeedbackTimer_ = 0.1f;
                            }
                            // Next Vehicle
                            else if (y > cardY + 10.0f && y < cardY + 90.0f) {
                                nextVehicle();
                                pressedButtonId_ = 9; buttonFeedbackTimer_ = 0.1f;
                            }
                        }
                        // Mission Selection
                        else if (y > (float)height_ - 210.0f && y < (float)height_ - 150.0f) {
                            for (int i = 1; i <= 4; ++i) {
                                float btnX = (float)width_ / 2.0f - 375.0f + (i-1) * 250.0f;
                                if (x > btnX - 110.0f && x < btnX + 110.0f) {
                                    acceptMission(i);
                                }
                            }
                        }
                        // Start / Purchase
                        else if (x > (float)width_ / 2.0f - 300.0f && x < (float)width_ / 2.0f + 300.0f &&
                                 y > (float)height_ - 120.0f && y < (float)height_ - 20.0f) {
                            pressedButtonId_ = 6; buttonFeedbackTimer_ = 0.1f;
                            if (vehicleCatalog_[currentVehicleIndex_].isPurchased) {
                                startMission();
                            } else {
                                purchaseVehicle(currentVehicleIndex_);
                            }
                        }
                    }
                }
            }
        }
    }
    // clear the motion input count in this buffer for main thread to re-use.
    android_app_clear_motion_events(inputBuffer);
    android_app_clear_key_events(inputBuffer);
}

void Renderer::initVehicle() {
    gameState_ = GameState::SHOP;
    vehicleCatalog_.clear();
    addonCatalog_.clear();

    // Initialize Addons
    addonCatalog_ = {
            {"Roof Rack", 300, 50.0f, AddonType::ROOF_RACK, false},
            {"Small Trailer", 600, 200.0f, AddonType::TRAILER, false},
            {"Bed Cargo", 400, 150.0f, AddonType::BED_CARGO, false},
            {"Large Bed Cargo", 800, 400.0f, AddonType::LARGE_BED_CARGO, false},
            {"Semi-Trailer", 1500, 1000.0f, AddonType::SEMI_TRAILER, false}
    };

    // 1. Sedan (CAR)
    VehicleConfig sedan;
    sedan.name = "Sedan";
    sedan.price = 1000;
    sedan.isPurchased = true;
    sedan.maxGears = 5;
    sedan.gearRatios[0] = -2.5f; sedan.gearRatios[1] = 0.0f;
    sedan.gearRatios[2] = 3.5f; sedan.gearRatios[3] = 2.2f; sedan.gearRatios[4] = 1.4f; sedan.gearRatios[5] = 1.0f; sedan.gearRatios[6] = 0.7f;
    sedan.has4x4 = false; sedan.hasDiffLock = false; sedan.hasTractionControl = true;
    sedan.engineTorque = 100.0f;
    sedan.weight = 1200.0f;
    sedan.fuel = 100.0f;
    sedan.fuelConsumptionRate = 0.05f;
    sedan.category = VehicleCategory::CAR;
    sedan.currentAddonIndex = -1;
    sedan.supportedAddonTypes = {AddonType::ROOF_RACK, AddonType::TRAILER};
    vehicleCatalog_.push_back(sedan);

    // 2. Jeep (PICKUP)
    VehicleConfig jeep;
    jeep.name = "Jeep 4x4";
    jeep.price = 2000;
    jeep.isPurchased = false;
    jeep.maxGears = 5;
    jeep.gearRatios[0] = -3.0f; jeep.gearRatios[1] = 0.0f;
    jeep.gearRatios[2] = 4.0f; jeep.gearRatios[3] = 2.5f; jeep.gearRatios[4] = 1.6f; jeep.gearRatios[5] = 1.1f; jeep.gearRatios[6] = 0.8f;
    jeep.has4x4 = true; jeep.hasDiffLock = true; jeep.hasTractionControl = true;
    jeep.engineTorque = 140.0f;
    jeep.weight = 1600.0f;
    jeep.fuel = 100.0f;
    jeep.fuelConsumptionRate = 0.08f;
    jeep.category = VehicleCategory::PICKUP;
    jeep.currentAddonIndex = -1;
    jeep.supportedAddonTypes = {AddonType::BED_CARGO, AddonType::TRAILER};
    vehicleCatalog_.push_back(jeep);

    // 3. Heavy Truck (TRUCK)
    VehicleConfig truck;
    truck.name = "Heavy Truck";
    truck.price = 5000;
    truck.isPurchased = false;
    truck.maxGears = 6;
    truck.gearRatios[0] = -4.5f; truck.gearRatios[1] = 0.0f;
    truck.gearRatios[2] = 5.5f; truck.gearRatios[3] = 3.5f; truck.gearRatios[4] = 2.5f; truck.gearRatios[5] = 1.8f; truck.gearRatios[6] = 1.2f; truck.gearRatios[7] = 0.9f;
    truck.has4x4 = true; truck.hasDiffLock = true; truck.hasTractionControl = false;
    truck.engineTorque = 350.0f;
    truck.weight = 6000.0f;
    truck.fuel = 100.0f;
    truck.fuelConsumptionRate = 0.15f;
    truck.category = VehicleCategory::TRUCK;
    truck.currentAddonIndex = -1;
    truck.supportedAddonTypes = {AddonType::LARGE_BED_CARGO};
    vehicleCatalog_.push_back(truck);

    // 4. Semi-Horse (SEMI)
    VehicleConfig semi;
    semi.name = "Semi Horse";
    semi.price = 8000;
    semi.isPurchased = false;
    semi.maxGears = 6;
    semi.gearRatios[0] = -5.0f; semi.gearRatios[1] = 0.0f;
    semi.gearRatios[2] = 6.0f; semi.gearRatios[3] = 4.0f; semi.gearRatios[4] = 2.8f; semi.gearRatios[5] = 2.0f; semi.gearRatios[6] = 1.4f; semi.gearRatios[7] = 1.0f;
    semi.has4x4 = true; semi.hasDiffLock = true; semi.hasTractionControl = false;
    semi.engineTorque = 500.0f;
    semi.weight = 8000.0f;
    semi.fuel = 100.0f;
    semi.fuelConsumptionRate = 0.25f;
    semi.category = VehicleCategory::SEMI;
    semi.currentAddonIndex = -1;
    semi.supportedAddonTypes = {AddonType::SEMI_TRAILER};
    vehicleCatalog_.push_back(semi);

    currentVehicleIndex_ = 0;
    currentVehicle_ = vehicleCatalog_[currentVehicleIndex_];
    currentFuel_ = currentVehicle_.fuel;
    fuelCapacity_ = 100.0f;

    vehicleState_.currentGear = 0;
    vehicleState_.is4x4Enabled = false;
    vehicleState_.isDiffLockEnabled = false;
    vehicleState_.isTCEnabled = true;
    vehicleState_.rpm = 800.0f;

    // Initialization of Trail Elements
    mudZones_.clear();
    mudZones_.push_back({0.0f, 10.0f, 4.0f, 0.8f}); // Friction 0.8 = 80% reduction
    mudZones_.push_back({-5.0f, 25.0f, 3.0f, 0.9f}); // Deeper mud

    fuelStations_.clear();
    fuelStations_.push_back({5.0f, 5.0f, 3.0f});
    fuelStations_.push_back({-5.0f, 35.0f, 3.0f});

    slopeZones_.clear();
    slopeZones_.push_back({0.0f, 35.0f, 10.0f, 8.0f, 4.0f}); // A steep hill at z=35

    // Checkpoints
    checkpoints_.clear();
    checkpoints_.push_back({0.0f, 15.0f, false});
    checkpoints_.push_back({0.0f, 30.0f, false});
    hasReachedAnyCheckpoint_ = false;

    loadGame();
}

void Renderer::shiftGear(int delta) {
    int nextGear = vehicleState_.currentGear + delta;
    if (nextGear >= -1 && nextGear <= currentVehicle_.maxGears) {
        vehicleState_.currentGear = nextGear;
    }
}

void Renderer::toggle4x4() {
    if (currentVehicle_.has4x4) {
        vehicleState_.is4x4Enabled = !vehicleState_.is4x4Enabled;
    }
}

void Renderer::toggleDiffLock() {
    if (currentVehicle_.hasDiffLock) {
        vehicleState_.isDiffLockEnabled = !vehicleState_.isDiffLockEnabled;
    }
}

void Renderer::toggleTC() {
    if (currentVehicle_.hasTractionControl) {
        vehicleState_.isTCEnabled = !vehicleState_.isTCEnabled;
    }
}

void Renderer::resetVehicle() {
    vehicleHealth_ = 100.0f;
    if (hasReachedAnyCheckpoint_) {
        carX_ = lastCheckpointX_;
        carY_ = lastCheckpointY_;
        carZ_ = lastCheckpointZ_;
        carRotation_ = lastCheckpointRotation_;
    } else {
        carX_ = 0.0f;
        carY_ = 0.0f;
        carZ_ = 0.0f;
        carRotation_ = 0.0f;
    }
    carPitch_ = 0.0f;
    carSpeed_ = 0.0f;
    rpmOverLimitDuration_ = 0.0f;
    vehicleState_.rpm = 800.0f;
    vehicleState_.currentGear = 0;
    isWinchAttached_ = false;
    winchAnchorIndex_ = -1;
    isWinching_ = false;
}

void Renderer::saveGame() {
    if (!app_ || !app_->activity || !app_->activity->vm) return;

    JNIEnv *env;
    app_->activity->vm->AttachCurrentThread(&env, nullptr);

    jclass clazz = env->GetObjectClass(app_->activity->javaGameActivity);
    jmethodID saveMethod = env->GetMethodID(clazz, "savePlayerData", "(JLjava/lang/String;Ljava/lang/String;FF)V");

    std::string vData = "";
    for (const auto& v : vehicleCatalog_) vData += v.isPurchased ? "1" : "0";

    std::string aData = "";
    for (const auto& a : addonCatalog_) aData += a.isPurchased ? "1" : "0";

    jstring jvData = env->NewStringUTF(vData.c_str());
    jstring jaData = env->NewStringUTF(aData.c_str());

    env->CallVoidMethod(app_->activity->javaGameActivity, saveMethod, (jlong)playerMoney_, jvData, jaData, currentFuel_, fuelCapacity_);

    env->DeleteLocalRef(jvData);
    env->DeleteLocalRef(jaData);
    app_->activity->vm->DetachCurrentThread();
}

void Renderer::loadGame() {
    if (!app_ || !app_->activity || !app_->activity->vm) return;

    JNIEnv *env;
    app_->activity->vm->AttachCurrentThread(&env, nullptr);

    jclass clazz = env->GetObjectClass(app_->activity->javaGameActivity);
    jmethodID loadMethod = env->GetMethodID(clazz, "loadPlayerData", "()Ljava/lang/String;");

    jstring jData = (jstring)env->CallObjectMethod(app_->activity->javaGameActivity, loadMethod);
    const char *dataStr = env->GetStringUTFChars(jData, nullptr);
    std::string data(dataStr);
    env->ReleaseStringUTFChars(jData, dataStr);

    // Parse: money|vehicles|addons|fuel|fuelCap
    size_t pos1 = data.find('|');
    size_t pos2 = data.find('|', pos1 + 1);
    size_t pos3 = data.find('|', pos2 + 1);
    size_t pos4 = data.find('|', pos3 + 1);

    if (pos1 != std::string::npos && pos2 != std::string::npos) {
        playerMoney_ = std::stol(data.substr(0, pos1));
        std::string vData = data.substr(pos1 + 1, pos2 - pos1 - 1);
        std::string aData = (pos3 != std::string::npos) ? data.substr(pos2 + 1, pos3 - pos2 - 1) : data.substr(pos2 + 1);

        for (int i = 0; i < vData.length() && i < vehicleCatalog_.size(); ++i) {
            vehicleCatalog_[i].isPurchased = (vData[i] == '1');
        }
        for (int i = 0; i < aData.length() && i < addonCatalog_.size(); ++i) {
            addonCatalog_[i].isPurchased = (aData[i] == '1');
        }

        if (pos3 != std::string::npos && pos4 != std::string::npos) {
            currentFuel_ = std::stof(data.substr(pos3 + 1, pos4 - pos3 - 1));
            fuelCapacity_ = std::stof(data.substr(pos4 + 1));
        }
    }

    app_->activity->vm->DetachCurrentThread();
}

void Renderer::buyUpgradeTorque() {
    if (playerMoney_ >= 200) {
        playerMoney_ -= 200;
        currentVehicle_.engineTorque += 20.0f;
        // Update catalog as well
        vehicleCatalog_[currentVehicleIndex_].engineTorque = currentVehicle_.engineTorque;
        saveGame();
    }
}

void Renderer::upgradeFuelCapacity() {
    if (playerMoney_ >= 300) {
        playerMoney_ -= 300;
        fuelCapacity_ += 50.0f;
        saveGame();
    }
}

void Renderer::repairVehicle() {
    if (playerMoney_ >= 100) {
        playerMoney_ -= 100;
        vehicleHealth_ = 100.0f;
        saveGame();
    }
}

void Renderer::refuelFullTank() {
    if (playerMoney_ >= 50) {
        playerMoney_ -= 50;
        currentFuel_ = fuelCapacity_;
        saveGame();
    }
}

void Renderer::purchaseVehicle(int index) {
    if (index >= 0 && index < vehicleCatalog_.size()) {
        if (!vehicleCatalog_[index].isPurchased && playerMoney_ >= vehicleCatalog_[index].price) {
            playerMoney_ -= vehicleCatalog_[index].price;
            vehicleCatalog_[index].isPurchased = true;
            saveGame();
        }
    }
}

void Renderer::purchaseAddon(int addonIndex) {
    if (addonIndex >= 0 && addonIndex < addonCatalog_.size()) {
        auto& addon = addonCatalog_[addonIndex];

        // Check if current vehicle supports this addon type
        bool supported = false;
        for (auto type : currentVehicle_.supportedAddonTypes) {
            if (type == addon.type) {
                supported = true;
                break;
            }
        }

        if (!supported) return;

        if (!addon.isPurchased && playerMoney_ >= addon.price) {
            playerMoney_ -= addon.price;
            addon.isPurchased = true;
            currentVehicle_.currentAddonIndex = addonIndex;
            vehicleCatalog_[currentVehicleIndex_].currentAddonIndex = addonIndex;
            saveGame();
        } else if (addon.isPurchased) {
            // If already purchased, just equip it
            currentVehicle_.currentAddonIndex = addonIndex;
            vehicleCatalog_[currentVehicleIndex_].currentAddonIndex = addonIndex;
            saveGame();
        }
    }
}

void Renderer::nextVehicle() {
    currentVehicleIndex_ = (currentVehicleIndex_ + 1) % vehicleCatalog_.size();
    if (vehicleCatalog_[currentVehicleIndex_].isPurchased) {
        currentVehicle_ = vehicleCatalog_[currentVehicleIndex_];
    }
}

void Renderer::startMission() {
    if (vehicleCatalog_[currentVehicleIndex_].isPurchased) {
        currentVehicle_ = vehicleCatalog_[currentVehicleIndex_];
        resetVehicle();
        if (currentCargo_.type != CargoType::NONE) {
            currentCargo_.health = 100.0f;
        }
        gameState_ = GameState::GAMEPLAY;
    }
}

void Renderer::acceptMission(int cargoTypeIndex) {
    CargoType type = (CargoType)cargoTypeIndex;
    bool possible = false;

    AddonType currentAddon = AddonType::ROOF_RACK; // Dummy
    if (currentVehicle_.currentAddonIndex != -1) {
        currentAddon = addonCatalog_[currentVehicle_.currentAddonIndex].type;
    }

    if (type == CargoType::LIGHT_BOX) {
        if (currentVehicle_.currentAddonIndex != -1 || currentVehicle_.category != VehicleCategory::CAR) possible = true;
    } else if (type == CargoType::MEDIUM_BARREL) {
        if (currentVehicle_.category != VehicleCategory::CAR || (currentVehicle_.currentAddonIndex != -1 && currentAddon == AddonType::TRAILER)) possible = true;
    } else if (type == CargoType::HEAVY_LOGS) {
        if (currentVehicle_.category == VehicleCategory::TRUCK || currentVehicle_.category == VehicleCategory::SEMI) possible = true;
    } else if (type == CargoType::SEMI_CONTAINER) {
        if (currentVehicle_.currentAddonIndex != -1 && currentAddon == AddonType::SEMI_TRAILER) possible = true;
    } else if (type == CargoType::NONE) {
        possible = true;
    }

    if (possible) {
        currentCargo_.type = type;
        if (type == CargoType::LIGHT_BOX) {
            currentCargo_.name = "Light Box"; currentCargo_.weight = 100.0f; currentCargo_.value = 200; currentCargo_.fragility = 0.5f;
        } else if (type == CargoType::MEDIUM_BARREL) {
            currentCargo_.name = "Medium Barrel"; currentCargo_.weight = 400.0f; currentCargo_.value = 500; currentCargo_.fragility = 0.3f;
        } else if (type == CargoType::HEAVY_LOGS) {
            currentCargo_.name = "Heavy Logs"; currentCargo_.weight = 2000.0f; currentCargo_.value = 1200; currentCargo_.fragility = 0.1f;
        } else if (type == CargoType::SEMI_CONTAINER) {
            currentCargo_.name = "Semi Container"; currentCargo_.weight = 5000.0f; currentCargo_.value = 3000; currentCargo_.fragility = 0.2f;
        } else {
            currentCargo_.name = "None"; currentCargo_.weight = 0.0f; currentCargo_.value = 0; currentCargo_.fragility = 0.0f;
        }
        currentCargo_.health = 100.0f;
    }
}

extern android_app *gApp;

extern "C" {
JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_shiftGear(JNIEnv *env, jobject thiz, jint delta) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->shiftGear(delta);
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_toggle4x4(JNIEnv *env, jobject thiz) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->toggle4x4();
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_toggleDiffLock(JNIEnv *env, jobject thiz) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->toggleDiffLock();
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_toggleTC(JNIEnv *env, jobject thiz) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->toggleTC();
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_setDamageEnabled(JNIEnv *env, jobject thiz, jboolean enabled) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->setDamageEnabled(enabled);
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_resetVehicle(JNIEnv *env, jobject thiz) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->resetVehicle();
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_buyUpgradeTorque(JNIEnv *env, jobject thiz) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->buyUpgradeTorque();
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_repairVehicle(JNIEnv *env, jobject thiz) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->repairVehicle();
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_purchaseVehicle(JNIEnv *env, jobject thiz, jint index) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->purchaseVehicle(index);
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_purchaseAddon(JNIEnv *env, jobject thiz, jint index) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->purchaseAddon(index);
    }
}

JNIEXPORT void JNICALL
Java_com_example_trilheiro_MainActivity_acceptMission(JNIEnv *env, jobject thiz, jint cargoTypeIndex) {
    if (gApp && gApp->userData) {
        auto *pRenderer = reinterpret_cast<Renderer *>(gApp->userData);
        pRenderer->acceptMission(cargoTypeIndex);
    }
}
}
