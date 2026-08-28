#pragma once

#include "lvgl.h"

/**
 * Brick Breaker, built from lvgl objects rather than a canvas so the panel
 * costs a few hundred bytes instead of a full framebuffer.
 *
 * Paddle moves on LV_KEY_LEFT/RIGHT from the trackball, ENTER launches.
 */
class BrickBreakerPanel
{
  public:
    BrickBreakerPanel(lv_obj_t *panel);
    ~BrickBreakerPanel(void);

    // called from the view's task_handler while the panel is active
    void task_handler(void);
    // LV_KEY_LEFT / LV_KEY_RIGHT / LV_KEY_ENTER
    void onKey(uint32_t key);
    // re-measure and restart; call when the panel becomes visible
    void activate(void);
    // hand input back to the normal UI navigation
    void deactivate(void);

    lv_obj_t *getPanel(void) { return panel; }

  protected:
    static constexpr int c_cols = 8;
    static constexpr int c_rows = 5;

    enum State { eReady, ePlaying, eCleared, eOver };

    void buildLevel(void);
    void resetBall(void);
    void launchBall(void);
    void step(void);
    bool hitBrick(float x, float y);
    void updateStatus(void);
    void grabInput(bool grab);

    lv_obj_t *panel = nullptr;
    lv_obj_t *paddle = nullptr;
    lv_obj_t *ball = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *banner = nullptr;
    lv_obj_t *bricks[c_rows][c_cols] = {};

    // panel geometry, measured on activate()
    int16_t w = 0, h = 0;
    int16_t brickW = 0, brickH = 0, originX = 0, originY = 0;

    float paddleX = 0, ballX = 0, ballY = 0, ballVX = 0, ballVY = 0;
    uint32_t lastStep = 0;

    // The trackball driver rate-limits to one event per 250ms, so a key press
    // starts a glide that continues until the next one is due rather than
    // moving the paddle a fixed step.
    int8_t moveDir = 0;
    uint32_t moveUntil = 0;
    float paddleSpeed = 0;

    // input capture
    // direct trackball polling, independent of the lvgl indev path
    bool tbLast[2] = {true, true};
    uint16_t gpioEvents = 0, keyEvents = 0;
    uint32_t lastKeyCode = 0;

    lv_group_t *gameGroup = nullptr;
    lv_group_t *savedGroup[4] = {};
    lv_indev_t *savedIndev[4] = {};
    uint8_t savedCount = 0;
    uint16_t score = 0;
    uint8_t lives = 3, level = 1, bricksLeft = 0;
    State state = eReady;
};
