#ifndef ANDROIDGLINVESTIGATIONS_RENDERER_H
#define ANDROIDGLINVESTIGATIONS_RENDERER_H

#include <EGL/egl.h>
#include <memory>
#include <vector>
#include <string>

#include "Model.h"
#include "Shader.h"

struct android_app;

struct Particle {
    float x, y, z;
    float life;
    float maxLife;
    float scale;
};

enum class VehicleCategory { CAR, PICKUP, TRUCK, SEMI };
enum class AddonType { ROOF_RACK, TRAILER, BED_CARGO, LARGE_BED_CARGO, SEMI_TRAILER };

struct AddonConfig {
    std::string name;
    int price;
    float weightImpact;
    AddonType type;
    bool isPurchased;
};

struct VehicleConfig {
    std::string name;
    int price;
    bool isPurchased;
    int maxGears;
    float gearRatios[7]; // index 0 = reverse, 1 = neutral (0.0), 2-7 = gears 1-6
    bool has4x4;
    bool hasDiffLock;
    bool hasTractionControl;
    float engineTorque;
    float weight;
    float fuel;
    float fuelConsumptionRate;
    VehicleCategory category;
    int currentAddonIndex; // -1 for none
    std::vector<AddonType> supportedAddonTypes;
};

struct VehicleState {
    int currentGear; // -1=Reverse, 0=Neutral, 1-5
    bool is4x4Enabled;
    bool isDiffLockEnabled;
    bool isTCEnabled;
    float rpm;
};

enum class GameState { START_SCREEN, SHOP, GAMEPLAY, GAME_OVER };

struct Checkpoint {
    float x, z;
    bool reached;
};

enum class CargoType { NONE, LIGHT_BOX, MEDIUM_BARREL, HEAVY_LOGS, SEMI_CONTAINER };

struct Cargo {
    CargoType type = CargoType::NONE;
    std::string name = "None";
    float weight = 0.0f;
    int value = 0;
    float fragility = 0.0f;
    float health = 100.0f;
};

struct MudZone {
    float x, z, radius, friction;
};

struct FuelStation {
    float x, z, radius;
};

struct SlopeZone {
    float x, z, width, length, heightDelta;
};

class Renderer {
public:
    /*!
     * @param pApp the android_app this Renderer belongs to, needed to configure GL
     */
    inline Renderer(android_app *pApp) :
            app_(pApp),
            display_(EGL_NO_DISPLAY),
            surface_(EGL_NO_SURFACE),
            context_(EGL_NO_CONTEXT),
            width_(0),
            height_(0),
            shaderNeedsNewProjectionMatrix_(true) {
        initVehicle();
        initRenderer();
    }

    virtual ~Renderer();

    /*!
     * Handles input from the android_app.
     *
     * Note: this will clear the input queue
     */
    void handleInput();

    /*!
     * Renders all the models in the renderer
     */
    void render();

    void renderUI();
    void drawStyledButton(float x, float y, float w, float h, float r, float g, float b, float a, bool pressed);
    void drawProgressBar(float x, float y, float w, float h, float progress, float r, float g, float b);

    /*!
     * Updates game logic
     */
    void update();

    /*!
     * Sets the tilt value from the accelerometer
     */
    void setTilt(float tilt) { tilt_ = tilt; }

    // Vehicle controls
    void shiftGear(int delta);
    void toggle4x4();
    void toggleDiffLock();
    void toggleTC();

    void setDamageEnabled(bool enabled) { isDamageEnabled_ = enabled; }
    void resetVehicle();

    void saveGame();
    void loadGame();

    // Shop logic
    void buyUpgradeTorque();
    void upgradeFuelCapacity();
    void repairVehicle();
    void refuelFullTank();
    void purchaseVehicle(int index);
    void purchaseAddon(int addonIndex);
    void nextVehicle();
    void startMission();
    void acceptMission(int cargoTypeIndex);

private:
    /*!
     * Performs necessary OpenGL initialization. Customize this if you want to change your EGL
     * context or application-wide settings.
     */
    void initRenderer();

    /*!
     * Initializes the vehicle configuration and state
     */
    void initVehicle();

    /*!
     * @brief we have to check every frame to see if the framebuffer has changed in size. If it has,
     * update the viewport accordingly
     */
    void updateRenderArea();

    /*!
     * Creates the models for this sample. You'd likely load a scene configuration from a file or
     * use some other setup logic in your full game.
     */
    void createModels();

    bool areHeadlightsOn_ = false;

    android_app *app_;

    EGLDisplay display_;
    EGLSurface surface_;
    EGLContext context_;
    EGLint width_;
    EGLint height_;

    bool shaderNeedsNewProjectionMatrix_;

    float projectionMatrix_[16];

    std::unique_ptr<Shader> shader_;
    std::vector<Model> models_;

    // Vehicle state
    VehicleConfig currentVehicle_;
    VehicleState vehicleState_;
    std::vector<VehicleConfig> vehicleCatalog_;
    std::vector<AddonConfig> addonCatalog_;
    int currentVehicleIndex_ = 0;

    // Economy
    long playerMoney_ = 1000;
    GameState gameState_ = GameState::GAMEPLAY;
    Cargo currentCargo_;

    // Car state
    float carX_ = 0.0f;
    float carY_ = 0.0f;
    float carZ_ = 0.0f;
    float carRotation_ = 0.0f;
    float carPitch_ = 0.0f;
    float carSpeed_ = 0.0f;
    float currentFuel_ = 100.0f;
    float fuelCapacity_ = 100.0f;

    // Input state
    float acceleratorValue_ = 0.0f;
    float steer_ = 0.0f;
    float tilt_ = 0.0f;
    bool useTilt_ = true;
    int pressedButtonId_ = -1; // -1 = none, 1=4x4, 2=Diff, 3=TC, 4=Winch, 5=Pull, 6=Start, 7=Repair, 8=Torque, 9=Next
    float buttonFeedbackTimer_ = 0.0f;

    // Camera state
    float camX_ = 0.0f;
    float camY_ = 5.0f;
    float camZ_ = 10.0f;

    // Obstacles
    struct Obstacle {
        float x, z;
    };
    std::vector<Obstacle> obstacles_;
    std::vector<MudZone> mudZones_;
    std::vector<FuelStation> fuelStations_;
    std::vector<SlopeZone> slopeZones_;

    // Checkpoints
    std::vector<Checkpoint> checkpoints_;
    float lastCheckpointX_ = 0.0f;
    float lastCheckpointY_ = 0.0f;
    float lastCheckpointZ_ = 0.0f;
    float lastCheckpointRotation_ = 0.0f;
    bool hasReachedAnyCheckpoint_ = false;

    // Damage System
    bool isDamageEnabled_ = true;
    float vehicleHealth_ = 100.0f;
    float rpmOverLimitDuration_ = 0.0f;

    // Particle System
    std::vector<Particle> particles_;
    int smokeSpawnTimer_ = 0;

    // Winch State
    bool isWinchAttached_ = false;
    int winchAnchorIndex_ = -1;
    float winchCableLength_ = 0.0f;
    bool isWinching_ = false;
    const float maxWinchDistance_ = 10.0f;
};

#endif //ANDROIDGLINVESTIGATIONS_RENDERER_H