#include "graphics/game/BrickBreakerPanel.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <cmath>
#include <cstdio>

#define PADDLE_H 6
#define BALL_D   7
#define STATUS_H 16

// EncoderInputDriver rate-limits the trackball to one event per 250ms, so a
// per-event step can never be fast enough. Instead each event starts a glide
// that runs until the next event is due, ramping up while the roll continues.
#define GLIDE_MS      320   // must exceed the driver's 250ms limiter
#define SPEED_START   7.0f  // px per 20ms tick -> 350 px/s
#define SPEED_MAX     18.0f // px per 20ms tick -> 900 px/s
#define SPEED_RAMP    0.6f  // gained per tick while the roll continues


BrickBreakerPanel::BrickBreakerPanel(lv_obj_t *panel) : panel(panel)
{
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0a0a12), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    status = lv_label_create(panel);
    lv_obj_set_style_text_color(status, lv_color_hex(0xb4b4be), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(status, 4, 2);
    lv_label_set_text(status, "");

    banner = lv_label_create(panel);
    lv_obj_set_style_text_color(banner, lv_color_hex(0xe6d23c), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(banner, "");

    paddle = lv_obj_create(panel);
    lv_obj_remove_flag(paddle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(paddle, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(paddle, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(paddle, lv_color_hex(0xdcdceb), LV_PART_MAIN | LV_STATE_DEFAULT);

    ball = lv_obj_create(panel);
    lv_obj_remove_flag(ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(ball, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ball, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_size(ball, BALL_D, BALL_D);

    for (int r = 0; r < c_rows; r++) {
        for (int c = 0; c < c_cols; c++) {
            lv_obj_t *b = lv_obj_create(panel);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_border_width(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(b, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
            bricks[r][c] = b;
        }
    }
}

BrickBreakerPanel::~BrickBreakerPanel(void) {}

/**
 * The trackball is an ENCODER indev: left/right normally move focus between
 * widgets instead of reaching us. Point every indev at a private group in
 * edit mode while the game is open, then hand them back.
 */
void BrickBreakerPanel::grabInput(bool grab)
{
    if (grab) {
        if (savedCount) return; // already held
        if (!gameGroup) {
            gameGroup = lv_group_create();
            lv_group_add_obj(gameGroup, panel);
        }
        for (lv_indev_t *d = lv_indev_get_next(nullptr); d && savedCount < 4; d = lv_indev_get_next(d)) {
            lv_indev_type_t t = lv_indev_get_type(d);
            if (t != LV_INDEV_TYPE_ENCODER && t != LV_INDEV_TYPE_KEYPAD) continue;
            savedIndev[savedCount] = d;
            savedGroup[savedCount] = lv_indev_get_group(d);
            savedCount++;
            lv_indev_set_group(d, gameGroup);
        }
        lv_group_focus_obj(panel);
        lv_group_set_editing(gameGroup, true); // stop left/right being navigation
    } else {
        for (uint8_t i = 0; i < savedCount; i++) {
            lv_indev_set_group(savedIndev[i], savedGroup[i]);
        }
        savedCount = 0;
    }
}

void BrickBreakerPanel::deactivate(void)
{
    grabInput(false);
    moveDir = 0;
}

void BrickBreakerPanel::activate(void)
{
    lv_obj_update_layout(panel);
    w = lv_obj_get_content_width(panel);
    h = lv_obj_get_content_height(panel);
    if (w < 40 || h < 40) return; // not laid out yet

    brickW = (w - 8) / c_cols;
    brickH = 12;
    originX = (w - brickW * c_cols) / 2;
    originY = STATUS_H + 6;

    lv_obj_set_size(paddle, w / 5, PADDLE_H);

    // Colour the rows once; they never change.
    static const uint32_t colours[c_rows] = {0xe63c3c, 0xeb8c28, 0xe6d23c, 0x46c85a, 0x4696eb};
    for (int r = 0; r < c_rows; r++)
        for (int c = 0; c < c_cols; c++) {
            lv_obj_set_size(bricks[r][c], brickW - 2, brickH - 2);
            lv_obj_set_pos(bricks[r][c], originX + c * brickW, originY + r * brickH);
            lv_obj_set_style_bg_color(bricks[r][c], lv_color_hex(colours[r]), LV_PART_MAIN | LV_STATE_DEFAULT);
        }

    score = 0;
    lives = 3;
    level = 1;
    paddleX = (w - lv_obj_get_width(paddle)) / 2.0f;
    buildLevel();
    state = eReady;
    moveDir = 0;
    grabInput(true);
    updateStatus();
}

void BrickBreakerPanel::buildLevel(void)
{
    bricksLeft = 0;
    for (int r = 0; r < c_rows; r++)
        for (int c = 0; c < c_cols; c++) {
            lv_obj_remove_flag(bricks[r][c], LV_OBJ_FLAG_HIDDEN);
            bricksLeft++;
        }
    resetBall();
}

void BrickBreakerPanel::resetBall(void)
{
    ballX = paddleX + lv_obj_get_width(paddle) / 2.0f;
    ballY = h - PADDLE_H - BALL_D - 2;
    ballVX = ballVY = 0;
}

void BrickBreakerPanel::launchBall(void)
{
    float speed = 2.2f + level * 0.2f;
    ballVX = ((lv_rand(0, 1) == 0) ? -1.0f : 1.0f) * speed * 0.55f;
    ballVY = -speed;
}

bool BrickBreakerPanel::hitBrick(float x, float y)
{
    int c = (int)((x - originX) / brickW);
    int r = (int)((y - originY) / brickH);
    if (c < 0 || c >= c_cols || r < 0 || r >= c_rows) return false;
    if (lv_obj_has_flag(bricks[r][c], LV_OBJ_FLAG_HIDDEN)) return false;

    lv_obj_add_flag(bricks[r][c], LV_OBJ_FLAG_HIDDEN);
    bricksLeft--;
    score += (c_rows - r) * 10;
    return true;
}

void BrickBreakerPanel::step(void)
{
    const int16_t pw = lv_obj_get_width(paddle);
    const int16_t paddleY = h - PADDLE_H - 2;

    ballX += ballVX;
    ballY += ballVY;

    if (ballX < 0) { ballX = 0; ballVX = -ballVX; }
    if (ballX + BALL_D > w) { ballX = w - BALL_D; ballVX = -ballVX; }
    if (ballY < STATUS_H) { ballY = STATUS_H; ballVY = -ballVY; }

    const float cx = ballX + BALL_D / 2.0f;
    const float cy = ballY + BALL_D / 2.0f;

    if (hitBrick(cx, cy + (ballVY > 0 ? BALL_D / 2.0f : -BALL_D / 2.0f))) {
        ballVY = -ballVY;
    } else if (hitBrick(cx + (ballVX > 0 ? BALL_D / 2.0f : -BALL_D / 2.0f), cy)) {
        ballVX = -ballVX;
    }

    // Paddle: contact point across the face sets the outgoing angle.
    if (ballVY > 0 && ballY + BALL_D >= paddleY && ballY + BALL_D <= paddleY + PADDLE_H + 4 &&
        cx >= paddleX && cx <= paddleX + pw) {
        float offset = (cx - (paddleX + pw / 2.0f)) / (pw / 2.0f);
        float speed = sqrtf(ballVX * ballVX + ballVY * ballVY);
        ballVX = speed * offset * 0.85f;
        float vy2 = speed * speed - ballVX * ballVX;
        ballVY = -sqrtf(vy2 > 0.4f ? vy2 : 0.4f);
        ballY = paddleY - BALL_D;
    }

    if (ballY > h) {
        if (--lives == 0) {
            state = eOver;
        } else {
            resetBall();
            state = eReady;
        }
    }

    if (bricksLeft == 0) state = eCleared;
}

void BrickBreakerPanel::updateStatus(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "S%u L%u B%u  gpio:%u key:%u/%lu", score, level, lives,
             (unsigned)gpioEvents, (unsigned)keyEvents, (unsigned long)lastKeyCode);
    lv_label_set_text(status, buf);

    const char *msg = "";
    switch (state) {
    case eReady:   msg = "press ENTER to launch"; break;
    case eCleared: msg = "LEVEL CLEAR - press ENTER"; break;
    case eOver:    msg = "GAME OVER - press ENTER"; break;
    default:       msg = ""; break;
    }
    lv_label_set_text(banner, msg);
    lv_obj_align(banner, LV_ALIGN_CENTER, 0, 0);
}

void BrickBreakerPanel::onKey(uint32_t key)
{
    if (w == 0) return;

    keyEvents++;
    lastKeyCode = key;

    // EncoderInputDriver (ENCODER_TYPE 3) remaps the trackball's horizontal
    // axis onto vertical keys so slider widgets respond to it:
    //   roll left  -> LV_KEY_DOWN     roll right -> LV_KEY_UP
    // Vertical rolls arrive as enc_diff, which lvgl turns into LV_KEY_LEFT/
    // RIGHT while the group is in edit mode. Accept all four.
    switch (key) {
    case LV_KEY_DOWN:
    case LV_KEY_LEFT:
    case LV_KEY_UP:
    case LV_KEY_RIGHT: {
        const int8_t dir = (key == LV_KEY_DOWN || key == LV_KEY_LEFT) ? -1 : 1;
        if (dir != moveDir) paddleSpeed = SPEED_START; // direction change restarts the ramp
        moveDir = dir;
        moveUntil = lv_tick_get() + GLIDE_MS;
        break;
    }
    case LV_KEY_ENTER:
        if (state == eReady) {
            launchBall();
            state = ePlaying;
        } else if (state == eCleared) {
            level++;
            buildLevel();
            state = eReady;
        } else if (state == eOver) {
            activate();
        }
        updateStatus();
        break;
    default:
        break;
    }
}

void BrickBreakerPanel::task_handler(void)
{
    if (w == 0) {
        activate();
        return;
    }

    const uint32_t now = lv_tick_get();

    // The lvgl encoder path swallows left/right as rotation, so read the
    // trackball pins directly too. Polled every call, not on the physics
    // tick, because the detent pulses are short.
#if defined(ARDUINO) && defined(INPUTDRIVER_ENCODER_LEFT) && defined(INPUTDRIVER_ENCODER_RIGHT)
    {
        const bool l = digitalRead(INPUTDRIVER_ENCODER_LEFT);
        const bool r = digitalRead(INPUTDRIVER_ENCODER_RIGHT);
        if (l != tbLast[0]) {
            tbLast[0] = l;
            gpioEvents++;
            if (moveDir != -1) paddleSpeed = SPEED_START;
            moveDir = -1;
            moveUntil = now + GLIDE_MS;
        }
        if (r != tbLast[1]) {
            tbLast[1] = r;
            gpioEvents++;
            if (moveDir != 1) paddleSpeed = SPEED_START;
            moveDir = 1;
            moveUntil = now + GLIDE_MS;
        }
    }
#endif

    if (now - lastStep < 20) return; // ~50 Hz physics
    lastStep = now;

    // Glide the paddle while a roll is still in progress.
    if (moveDir != 0) {
        if (now < moveUntil) {
            paddleSpeed += SPEED_RAMP;
            if (paddleSpeed > SPEED_MAX) paddleSpeed = SPEED_MAX;
            paddleX += moveDir * paddleSpeed;

            const int16_t pw = lv_obj_get_width(paddle);
            if (paddleX < 0) paddleX = 0;
            if (paddleX > w - pw) paddleX = w - pw;
            if (state == eReady) resetBall();
        } else {
            moveDir = 0;
            paddleSpeed = SPEED_START;
        }
    }

    const State before = state;
    if (state == ePlaying) step();

    lv_obj_set_pos(paddle, (int16_t)paddleX, h - PADDLE_H - 2);
    lv_obj_set_pos(ball, (int16_t)ballX, (int16_t)ballY);

    (void)before;
    updateStatus(); // always, so the input counters stay live
}
