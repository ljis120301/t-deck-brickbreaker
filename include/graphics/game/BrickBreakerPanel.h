#pragma once

#include "lvgl.h"

/**
 * Brick Breaker, built from lvgl objects rather than a canvas so the panel
 * costs a few hundred bytes instead of a full framebuffer.
 *
 * Aim the launch with a horizontal roll, fire with ENTER, then roll to move
 * the paddle. Bricks take one to three hits and crack as they take damage.
 */
class BrickBreakerPanel
{
  public:
    BrickBreakerPanel(lv_obj_t *panel);
    ~BrickBreakerPanel(void);

    // called from the view's task_handler while the panel is active
    void task_handler(void);
    // LV_KEY_* from the lvgl indev; movement comes from the driver's raw
    // detent counters instead, so this only really handles ENTER.
    void onKey(uint32_t key);
    // re-measure and restart; call when the panel becomes visible
    void activate(void);
    // hand input back to the normal UI navigation
    void deactivate(void);

    lv_obj_t *getPanel(void) { return panel; }

  protected:
    static constexpr int c_cols = 8;
    static constexpr int c_rows = 5;
    static constexpr int c_aimDots = 6;
    static constexpr int c_bars = 3;      // vertical bars on a metal brick
    static constexpr int c_wallSegs = 26; // hazard stripe segments per wall

    enum State { eReady, ePlaying, eCleared, eOver };

    void buildLevel(void);
    void resetBall(void);
    void launchBall(void);
    void step(void);
    bool hitBrick(float x, float y);
    void updateStatus(void);
    void grabInput(bool grab);
    static void onPanelDeleted(lv_event_t *e);

    void pollTrackball(uint32_t now);
    void paintBrick(int r, int c);
    void buildWalls(void);
    void loadHiScore(void);
    void saveHiScore(void);
    void layoutAim(void);
    void showAim(bool show);

    // Every lv_obj below is a descendant of `panel` and is owned by lvgl, not
    // by us. If lvgl ever deletes the panel (theme rebuild, screen teardown)
    // all of them are freed underneath us, so we latch `dead` and stop
    // touching any of them. crackA/crackB are also held by lvgl as raw
    // pointers, so this object must outlive its lvgl tree.
    bool dead = false;
    lv_obj_t *panel = nullptr;
    lv_obj_t *paddle = nullptr;
    lv_obj_t *ball = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *banner = nullptr;
    lv_obj_t *bricks[c_rows][c_cols] = {};
    lv_obj_t *cracks[c_rows][c_cols] = {};
    uint8_t brickHp[c_rows][c_cols] = {};
    lv_obj_t *aimDots[c_aimDots] = {};
    lv_obj_t *bars[c_rows][c_cols][c_bars] = {};
    lv_obj_t *wallSeg[2][c_wallSegs] = {};
    lv_obj_t *hiLabel = nullptr;
    lv_obj_t *livesLabel = nullptr;

    // Crack geometry, sized to the brick at activate(). All crack lines share
    // these arrays, which is fine because lvgl only stores the pointer.
    lv_point_precise_t crackA[4] = {};
    lv_point_precise_t crackB[4] = {};

    // panel geometry, measured on activate()
    int16_t w = 0, h = 0;
    int16_t playLeft = 0, playRight = 0;
    int16_t brickW = 0, brickH = 0, originX = 0, originY = 0;

    float paddleX = 0, ballX = 0, ballY = 0, ballVX = 0, ballVY = 0;
    float aimAngle = 0;    // radians from straight up, negative = left
    float rateEma = 0;     // detents/sec, smoothed, drives acceleration
    uint32_t lastStep = 0;
    uint32_t score = 0, hiScore = 0;
    uint32_t shownScore = 0xffffffffu, shownHi = 0xffffffffu;
    uint8_t shownLives = 0xff, shownLevel = 0xff;
    int8_t shownState = -1;   // banner text is only rewritten on a state change
    int8_t aimVisible = -1;   // aim dots are only toggled when visibility flips
    uint8_t lives = 3, level = 1, bricksLeft = 0;
    State state = eReady;

    // input capture
    lv_group_t *gameGroup = nullptr;
    lv_group_t *savedGroup[4] = {};
    lv_indev_t *savedIndev[4] = {};
    uint8_t savedCount = 0;
};
