#include "graphics/game/BrickBreakerPanel.h"

#include <cmath>
#include <cstdio>

#ifdef ARDUINO
#include <Arduino.h>
#endif

#ifdef INPUTDRIVER_ENCODER_TYPE
#include "input/EncoderInputDriver.h"
#define HAS_RAW_TRACKBALL 1
#endif

#define PADDLE_H 6
#define BALL_D   7
#define STATUS_H 16
#define TICK_MS  20

#define BALL_SPEED_BASE 4.6f
#define BALL_SPEED_STEP 0.45f
#define BALL_SPEED_MAX  9.0f

// --- pointer acceleration ---------------------------------------------------
// The trackball reports detents, not distance, so a fixed step is either too
// coarse for small corrections or too slow to cross the screen. Scale the step
// by how fast detents are arriving: a slow roll nudges, a flick sweeps.
#define GAIN_MIN     1.6f   // px per detent when easing
#define GAIN_MAX    22.0f   // px per detent at full tilt
#define RATE_LOW     8.0f   // detents/sec below which we stay at GAIN_MIN
#define RATE_HIGH   85.0f   // detents/sec at which we reach GAIN_MAX
#define RATE_DECAY   0.72f  // smoothing on the measured rate

#define AIM_LIMIT    1.30f  // ~75 degrees either side of vertical
#define AIM_PER_DET  0.045f // radians per detent while aiming

static const uint32_t rowBase[5] = {0xe63c3c, 0xeb8c28, 0xe6d23c, 0x46c85a, 0x4696eb};
static const uint8_t rowHp[5] = {3, 3, 2, 2, 1};

/** Scale an 0xRRGGBB value toward black. f is 0..256. */
static lv_color_t shade(uint32_t rgb, uint16_t f)
{
    const uint32_t r = (((rgb >> 16) & 0xff) * f) >> 8;
    const uint32_t g = (((rgb >> 8) & 0xff) * f) >> 8;
    const uint32_t b = ((rgb & 0xff) * f) >> 8;
    return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

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

    for (int r = 0; r < c_rows; r++) {
        for (int c = 0; c < c_cols; c++) {
            lv_obj_t *b = lv_obj_create(panel);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_border_width(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(b, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
            bricks[r][c] = b;

            lv_obj_t *k = lv_line_create(b);
            lv_obj_set_style_line_width(k, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_line_color(k, lv_color_hex(0x14141c), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_add_flag(k, LV_OBJ_FLAG_HIDDEN);
            cracks[r][c] = k;
        }
    }

    for (int i = 0; i < c_aimDots; i++) {
        lv_obj_t *d = lv_obj_create(panel);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(d, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x8080a0), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_size(d, 3, 3);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        aimDots[i] = d;
    }

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
}

BrickBreakerPanel::~BrickBreakerPanel(void) {}

// -------------------------------------------------------------- input capture

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
        lv_group_set_editing(gameGroup, true);
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
    showAim(false);
}

// -------------------------------------------------------------------- drawing

void BrickBreakerPanel::paintBrick(int r, int c)
{
    lv_obj_t *b = bricks[r][c];
    const uint8_t hp = brickHp[r][c];

    if (hp == 0) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);

    // Full health is the row's colour; each hit taken darkens it.
    const uint8_t full = rowHp[r];
    const uint16_t f = (hp >= full) ? 256 : (full == 3 ? (hp == 2 ? 180 : 120) : 150);
    lv_obj_set_style_bg_color(b, shade(rowBase[r], f), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *k = cracks[r][c];
    if (hp >= full) {
        lv_obj_add_flag(k, LV_OBJ_FLAG_HIDDEN);
    } else {
        // One crack after the first hit, a busier one after the second.
        lv_line_set_points(k, (hp == full - 1) ? crackA : crackB, 4);
        lv_obj_remove_flag(k, LV_OBJ_FLAG_HIDDEN);
    }
}

void BrickBreakerPanel::showAim(bool show)
{
    for (int i = 0; i < c_aimDots; i++) {
        if (show) lv_obj_remove_flag(aimDots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(aimDots[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void BrickBreakerPanel::layoutAim(void)
{
    const float sx = sinf(aimAngle);
    const float cy = cosf(aimAngle);
    const float cx0 = ballX + BALL_D / 2.0f;
    const float cy0 = ballY + BALL_D / 2.0f;

    for (int i = 0; i < c_aimDots; i++) {
        const float d = 12.0f + i * 9.0f;
        lv_obj_set_pos(aimDots[i], (int16_t)(cx0 + sx * d - 1.5f), (int16_t)(cy0 - cy * d - 1.5f));
    }
}

// ----------------------------------------------------------------- game logic

void BrickBreakerPanel::resetBall(void)
{
    ballX = paddleX + lv_obj_get_width(paddle) / 2.0f - BALL_D / 2.0f;
    ballY = h - PADDLE_H - BALL_D - 2;
    ballVX = ballVY = 0;
}

void BrickBreakerPanel::launchBall(void)
{
    // px per TICK_MS. 5.0 at level 1 is ~250 px/s, crossing the playfield in
    // about a second; capped so the discrete step stays smaller than a brick.
    float speed = BALL_SPEED_BASE + level * BALL_SPEED_STEP;
    if (speed > BALL_SPEED_MAX) speed = BALL_SPEED_MAX;
    ballVX = speed * sinf(aimAngle);
    ballVY = -speed * cosf(aimAngle);
}

void BrickBreakerPanel::buildLevel(void)
{
    bricksLeft = 0;
    for (int r = 0; r < c_rows; r++)
        for (int c = 0; c < c_cols; c++) {
            brickHp[r][c] = rowHp[r];
            paintBrick(r, c);
            bricksLeft++;
        }
    aimAngle = 0;
    resetBall();
}

void BrickBreakerPanel::activate(void)
{
    lv_obj_update_layout(panel);
    w = lv_obj_get_content_width(panel);
    h = lv_obj_get_content_height(panel);
    if (w < 40 || h < 40) return; // not laid out yet

    brickW = (w - 8) / c_cols;
    brickH = 14;
    originX = (w - brickW * c_cols) / 2;
    originY = STATUS_H + 6;

    lv_obj_set_size(paddle, w / 5, PADDLE_H);

    // Crack geometry, in brick-local coordinates.
    const int bw = brickW - 2, bh = brickH - 2;
    crackA[0] = {(lv_value_precise_t)(bw / 5), (lv_value_precise_t)0};
    crackA[1] = {(lv_value_precise_t)(bw / 2), (lv_value_precise_t)(bh / 2)};
    crackA[2] = {(lv_value_precise_t)(bw / 3), (lv_value_precise_t)(bh * 2 / 3)};
    crackA[3] = {(lv_value_precise_t)(bw * 3 / 5), (lv_value_precise_t)bh};

    crackB[0] = {(lv_value_precise_t)0, (lv_value_precise_t)(bh / 3)};
    crackB[1] = {(lv_value_precise_t)(bw / 2), (lv_value_precise_t)(bh / 2)};
    crackB[2] = {(lv_value_precise_t)(bw * 3 / 4), (lv_value_precise_t)(bh / 5)};
    crackB[3] = {(lv_value_precise_t)bw, (lv_value_precise_t)(bh * 3 / 4)};

    for (int r = 0; r < c_rows; r++)
        for (int c = 0; c < c_cols; c++) {
            lv_obj_set_size(bricks[r][c], brickW - 2, brickH - 2);
            lv_obj_set_pos(bricks[r][c], originX + c * brickW, originY + r * brickH);
        }

    score = 0;
    lives = 3;
    level = 1;
    rateEma = 0;
    paddleX = (w - lv_obj_get_width(paddle)) / 2.0f;
    buildLevel();
    state = eReady;
    grabInput(true);
    updateStatus();
}

bool BrickBreakerPanel::hitBrick(float x, float y)
{
    const int c = (int)((x - originX) / brickW);
    const int r = (int)((y - originY) / brickH);
    if (c < 0 || c >= c_cols || r < 0 || r >= c_rows) return false;
    if (brickHp[r][c] == 0) return false;

    brickHp[r][c]--;
    score += (c_rows - r) * 10;
    if (brickHp[r][c] == 0) bricksLeft--;
    paintBrick(r, c);
    return true;
}

void BrickBreakerPanel::step(void)
{
    const int16_t pw = lv_obj_get_width(paddle);
    const int16_t paddleY = h - PADDLE_H - 2;
    const float prevBottom = ballY + BALL_D;

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

    if (ballVY > 0 && prevBottom <= paddleY && ballY + BALL_D >= paddleY &&
        cx >= paddleX && cx <= paddleX + pw) {
        const float offset = (cx - (paddleX + pw / 2.0f)) / (pw / 2.0f);
        const float speed = sqrtf(ballVX * ballVX + ballVY * ballVY);
        ballVX = speed * offset * 0.85f;
        const float vy2 = speed * speed - ballVX * ballVX;
        ballVY = -sqrtf(vy2 > 0.4f ? vy2 : 0.4f);
        ballY = paddleY - BALL_D;
    }

    if (ballY > h) {
        if (--lives == 0) {
            state = eOver;
        } else {
            aimAngle = 0;
            resetBall();
            state = eReady;
        }
    }

    if (bricksLeft == 0) state = eCleared;
}

void BrickBreakerPanel::updateStatus(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "SCORE %05u   LVL %u   BALLS %u", score, level, lives);
    lv_label_set_text(status, buf);

    const char *msg = "";
    switch (state) {
    case eReady:   msg = "roll to aim - ENTER to fire"; break;
    case eCleared: msg = "LEVEL CLEAR - press ENTER"; break;
    case eOver:    msg = "GAME OVER - press ENTER"; break;
    default:       msg = ""; break;
    }
    lv_label_set_text(banner, msg);
    lv_obj_align(banner, LV_ALIGN_CENTER, 0, state == eReady ? 40 : 0);
}

void BrickBreakerPanel::onKey(uint32_t key)
{
    if (w == 0 || key != LV_KEY_ENTER) return;

    if (state == eReady) {
        launchBall();
        state = ePlaying;
        showAim(false);
    } else if (state == eCleared) {
        level++;
        buildLevel();
        state = eReady;
    } else if (state == eOver) {
        activate();
    }
    updateStatus();
}

// ---------------------------------------------------------------------- input

/**
 * Read the driver's raw detent counters and turn them into motion. Going
 * straight to the counters skips the shared action slot (which overwrites) and
 * the 4/s rate limiter in encoder_read(), both of which drop detents.
 */
void BrickBreakerPanel::pollTrackball(uint32_t now)
{
#ifdef HAS_RAW_TRACKBALL
    const int32_t dx = EncoderInputDriver::detentX;
    const int32_t dy = EncoderInputDriver::detentY;
    EncoderInputDriver::detentX = 0;
    EncoderInputDriver::detentY = 0;

    const float inst = (fabsf((float)dx) + fabsf((float)dy)) * (1000.0f / TICK_MS);
    rateEma = rateEma * RATE_DECAY + inst * (1.0f - RATE_DECAY);

    float t = (rateEma - RATE_LOW) / (RATE_HIGH - RATE_LOW);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const float gain = GAIN_MIN + (GAIN_MAX - GAIN_MIN) * t * t;

    if (state == eReady) {
        // Horizontal roll aims, vertical roll still nudges the paddle so you
        // can reposition before firing.
        if (dx) {
            aimAngle += dx * AIM_PER_DET * (0.5f + t * 2.0f);
            if (aimAngle < -AIM_LIMIT) aimAngle = -AIM_LIMIT;
            if (aimAngle > AIM_LIMIT) aimAngle = AIM_LIMIT;
        }
        if (dy) paddleX += dy * gain;
    } else if (state == ePlaying) {
        if (dx) paddleX += dx * gain;
    }

    const int16_t pw = lv_obj_get_width(paddle);
    if (paddleX < 0) paddleX = 0;
    if (paddleX > w - pw) paddleX = w - pw;
#else
    (void)now;
#endif
}

void BrickBreakerPanel::task_handler(void)
{
    if (w == 0) {
        activate();
        return;
    }

    const uint32_t now = lv_tick_get();
    if (now - lastStep < TICK_MS) return;
    lastStep = now;

    pollTrackball(now);

    if (state == ePlaying) {
        step();
    } else if (state == eReady) {
        resetBall();
        layoutAim();
    }

    showAim(state == eReady);

    lv_obj_set_pos(paddle, (int16_t)paddleX, h - PADDLE_H - 2);
    lv_obj_set_pos(ball, (int16_t)ballX, (int16_t)ballY);

    updateStatus();
}
