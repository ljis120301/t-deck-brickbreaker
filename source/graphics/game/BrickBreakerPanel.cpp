#include "graphics/game/BrickBreakerPanel.h"

#include <cmath>
#include <cstdio>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

#ifdef INPUTDRIVER_ENCODER_TYPE
#include "input/EncoderInputDriver.h"
#define HAS_RAW_TRACKBALL 1
#endif

#define PADDLE_H 7
#define BALL_D   7
#define STATUS_H 16
#define TICK_MS  20

#define WALL_W       6      // hazard-striped side walls
#define WALL_SEG_H  10

// px per SECOND, not per tick. Tying speed to the tick made it depend on how
// fast lvgl happened to be rendering: slow while the panel first painted, then
// abruptly faster once only dirty regions redrew.
#define BALL_SPEED_BASE 150.0f
#define BALL_SPEED_STEP  18.0f
#define BALL_SPEED_MAX  400.0f
#define STEP_DT (TICK_MS / 1000.0f)
// Never advance more than this many fixed steps in one call, so a slow frame
// catches up gradually instead of teleporting the ball through a brick.
#define MAX_CATCHUP 4

// --- pointer acceleration ---------------------------------------------------
// The trackball reports detents, not distance, so a fixed step is either too
// coarse for small corrections or too slow to cross the screen. Scale the step
// by how fast detents are arriving: a slow roll nudges, a flick sweeps.
#define GAIN_MIN     1.6f
#define GAIN_MAX    22.0f
#define RATE_LOW     8.0f
#define RATE_HIGH   85.0f
#define RATE_DECAY   0.72f

#define AIM_LIMIT    1.30f
#define AIM_PER_DET  0.045f

// Two block types, as in the original: barred metal blocks take several hits,
// plain red brick takes one.
static const uint8_t rowHp[5] = {3, 3, 2, 2, 1};

#define METAL_TOP  0xd8d8e2
#define METAL_BOT  0x82828e
#define METAL_EDGE 0x55555f
#define METAL_BAR  0x6a6a76

#define RED_TOP    0xe24a4a
#define RED_BOT    0x8e1c1c
#define RED_EDGE   0x5c1010

#define HAZARD_Y   0xf0c020
#define HAZARD_K   0x1a1a1a

/** Scale an 0xRRGGBB value toward black. f is 0..256. */
static lv_color_t shade(uint32_t rgb, uint16_t f)
{
    const uint32_t r = (((rgb >> 16) & 0xff) * f) >> 8;
    const uint32_t g = (((rgb >> 8) & 0xff) * f) >> 8;
    const uint32_t b = ((rgb & 0xff) * f) >> 8;
    return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/** Flat-styled child object with no padding, border or scrolling. */
static lv_obj_t *plainChild(lv_obj_t *parent, uint32_t colour)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), LV_PART_MAIN | LV_STATE_DEFAULT);
    return o;
}

BrickBreakerPanel::BrickBreakerPanel(lv_obj_t *panel) : panel(panel)
{
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x20202a), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(panel, lv_color_hex(0x0c0c12), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    // --- status bar ---------------------------------------------------------
    status = lv_label_create(panel);
    lv_obj_set_style_text_color(status, lv_color_hex(0xe6d23c), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(status, 4, 2);
    lv_label_set_text(status, "");

    hiLabel = lv_label_create(panel);
    lv_obj_set_style_text_color(hiLabel, lv_color_hex(0x8fb8e0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(hiLabel, "");

    livesLabel = lv_label_create(panel);
    lv_obj_set_style_text_color(livesLabel, lv_color_hex(0xc8c8d2), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(livesLabel, "");

    banner = lv_label_create(panel);
    lv_obj_set_style_text_color(banner, lv_color_hex(0xe6d23c), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(banner, "");

    // --- hazard walls -------------------------------------------------------
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < c_wallSegs; i++)
            wallSeg[side][i] = plainChild(panel, (i & 1) ? HAZARD_K : HAZARD_Y);

    // --- bricks -------------------------------------------------------------
    for (int r = 0; r < c_rows; r++) {
        const bool metal = rowHp[r] > 1;
        for (int c = 0; c < c_cols; c++) {
            lv_obj_t *b = lv_obj_create(panel);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(b, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(b, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(b, lv_color_hex(metal ? METAL_EDGE : RED_EDGE),
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
            bricks[r][c] = b;

            // The barred face that marks a block as taking more than one hit.
            if (metal)
                for (int i = 0; i < c_bars; i++)
                    bars[r][c][i] = plainChild(b, METAL_BAR);

            lv_obj_t *k = lv_line_create(b);
            lv_obj_set_style_line_width(k, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_line_color(k, lv_color_hex(0x101018), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_add_flag(k, LV_OBJ_FLAG_HIDDEN);
            cracks[r][c] = k;
        }
    }

    for (int i = 0; i < c_aimDots; i++) {
        lv_obj_t *d = plainChild(panel, 0x8080a0);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_size(d, 3, 3);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        aimDots[i] = d;
    }

    // --- paddle and ball ----------------------------------------------------
    paddle = lv_obj_create(panel);
    lv_obj_remove_flag(paddle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(paddle, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(paddle, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(paddle, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(paddle, lv_color_hex(0x102a55), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(paddle, lv_color_hex(0x6fa8ff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(paddle, lv_color_hex(0x1c46a0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(paddle, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);

    ball = lv_obj_create(panel);
    lv_obj_remove_flag(ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ball, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ball, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ball, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(ball, lv_color_hex(0x9a9ab0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ball, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_size(ball, BALL_D, BALL_D);

    // lvgl owns this tree. If it is torn down, every pointer we cached becomes
    // dangling, so stop using them rather than reading freed memory.
    lv_obj_add_event_cb(panel, onPanelDeleted, LV_EVENT_DELETE, this);

    loadHiScore();
}

void BrickBreakerPanel::onPanelDeleted(lv_event_t *e)
{
    auto *self = static_cast<BrickBreakerPanel *>(lv_event_get_user_data(e));
    if (!self) return;
    self->dead = true;
    self->savedCount = 0; // the indev groups went with the tree
    self->w = self->h = 0;
}

BrickBreakerPanel::~BrickBreakerPanel(void) {}

// ------------------------------------------------------------------ hi score

void BrickBreakerPanel::loadHiScore(void)
{
#ifdef ARDUINO
    // Own NVS namespace, so this never collides with Meshtastic's own keys.
    Preferences p;
    if (p.begin("brickbreak", true)) {
        hiScore = p.getUInt("hi", 0);
        p.end();
    }
#endif
}

void BrickBreakerPanel::saveHiScore(void)
{
#ifdef ARDUINO
    Preferences p;
    if (p.begin("brickbreak", false)) {
        p.putUInt("hi", hiScore);
        p.end();
    }
#endif
}

// -------------------------------------------------------------- input capture

void BrickBreakerPanel::grabInput(bool grab)
{
    if (grab) {
        if (savedCount) return;
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
        for (uint8_t i = 0; i < savedCount; i++) lv_indev_set_group(savedIndev[i], savedGroup[i]);
        savedCount = 0;
    }
}

void BrickBreakerPanel::deactivate(void)
{
    if (dead) return;
    grabInput(false);
    showAim(false);
}

// -------------------------------------------------------------------- drawing

void BrickBreakerPanel::buildWalls(void)
{
    for (int side = 0; side < 2; side++) {
        const int16_t x = side ? (w - WALL_W) : 0;
        for (int i = 0; i < c_wallSegs; i++) {
            lv_obj_t *seg = wallSeg[side][i];
            const int16_t y = STATUS_H + i * WALL_SEG_H;
            if (y >= h) {
                lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_remove_flag(seg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(seg, WALL_W, (y + WALL_SEG_H > h) ? (h - y) : WALL_SEG_H);
            lv_obj_set_pos(seg, x, y);
            // Offset the phase on the right wall so the stripes read as diagonal.
            const bool dark = ((i + side) & 1) != 0;
            lv_obj_set_style_bg_color(seg, lv_color_hex(dark ? HAZARD_K : HAZARD_Y),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void BrickBreakerPanel::paintBrick(int r, int c)
{
    lv_obj_t *b = bricks[r][c];
    const uint8_t hp = brickHp[r][c];
    const uint8_t full = rowHp[r];
    const bool metal = full > 1;

    if (hp == 0) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);

    // Each hit taken darkens the face.
    const uint16_t f = (hp >= full) ? 256 : (full == 3 ? (hp == 2 ? 190 : 135) : 160);
    lv_obj_set_style_bg_color(b, shade(metal ? METAL_TOP : RED_TOP, f), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(b, shade(metal ? METAL_BOT : RED_BOT, f), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *k = cracks[r][c];
    if (hp >= full) {
        lv_obj_add_flag(k, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_line_set_points(k, (hp == full - 1) ? crackA : crackB, 4);
        lv_obj_remove_flag(k, LV_OBJ_FLAG_HIDDEN);
    }
}

void BrickBreakerPanel::showAim(bool show)
{
    if (aimVisible == (int8_t)show) return;
    aimVisible = (int8_t)show;
    for (int i = 0; i < c_aimDots; i++) {
        if (show) lv_obj_remove_flag(aimDots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(aimDots[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void BrickBreakerPanel::layoutAim(void)
{
    const float sx = sinf(aimAngle);
    const float cyf = cosf(aimAngle);
    const float cx0 = ballX + BALL_D / 2.0f;
    const float cy0 = ballY + BALL_D / 2.0f;

    for (int i = 0; i < c_aimDots; i++) {
        const float d = 12.0f + i * 9.0f;
        lv_obj_set_pos(aimDots[i], (int16_t)(cx0 + sx * d - 1.5f), (int16_t)(cy0 - cyf * d - 1.5f));
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
    if (dead || !panel) return;
    lv_obj_update_layout(panel);
    const int16_t mw = lv_obj_get_content_width(panel);
    const int16_t mh = lv_obj_get_content_height(panel);
    // Leave w/h at zero until the measurement is usable, so task_handler keeps
    // retrying instead of running with a zero brick pitch and dividing by it.
    if (mw < 40 || mh < 40) return;
    w = mw;
    h = mh;

    playLeft = WALL_W;
    playRight = w - WALL_W;
    const int16_t playW = playRight - playLeft;

    brickW = (playW - 4) / c_cols;
    brickH = 14;
    originX = playLeft + (playW - brickW * c_cols) / 2;
    originY = STATUS_H + 6;

    lv_obj_set_size(paddle, playW / 5, PADDLE_H);
    buildWalls();

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
            lv_obj_set_size(bricks[r][c], bw, bh);
            lv_obj_set_pos(bricks[r][c], originX + c * brickW, originY + r * brickH);

            for (int i = 0; i < c_bars; i++) {
                if (!bars[r][c][i]) continue;
                lv_obj_set_size(bars[r][c][i], 2, bh - 4);
                lv_obj_set_pos(bars[r][c][i], (bw * (i + 1)) / (c_bars + 1) - 1, 1);
            }
        }

    score = 0;
    lives = 3;
    level = 1;
    rateEma = 0;
    shownScore = shownHi = 0xffffffffu;
    shownLives = shownLevel = 0xff;
    shownState = -1;
    aimVisible = -1;
    paddleX = playLeft + (playW - lv_obj_get_width(paddle)) / 2.0f;
    buildLevel();
    state = eReady;
    grabInput(true);
    updateStatus();
}

bool BrickBreakerPanel::hitBrick(float x, float y)
{
    if (brickW <= 0 || brickH <= 0) return false;

    // (int) truncates toward zero, so a ball just left of the grid gives -0.06
    // -> 0 and registers a phantom hit on column 0. Reject before dividing.
    const float fx = x - originX;
    const float fy = y - originY;
    if (fx < 0 || fy < 0) return false;

    const int c = (int)(fx / brickW);
    const int r = (int)(fy / brickH);
    if (c >= c_cols || r >= c_rows) return false;
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

    ballX += ballVX * STEP_DT;
    ballY += ballVY * STEP_DT;

    if (ballX < playLeft) { ballX = playLeft; ballVX = -ballVX; }
    if (ballX + BALL_D > playRight) { ballX = playRight - BALL_D; ballVX = -ballVX; }
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
        // Preserve |v| exactly, but never let the ball go so flat that it
        // skims sideways forever.
        const float minVy = speed * 0.25f;
        float vy2 = speed * speed - ballVX * ballVX;
        if (vy2 < minVy * minVy) vy2 = minVy * minVy;
        ballVY = -sqrtf(vy2);
        ballY = paddleY - BALL_D;
    }

    if (score > hiScore) hiScore = score;

    if (ballY > h) {
        if (lives > 0) lives--;
        if (lives == 0) {
            state = eOver;
            saveHiScore();
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
    // A null deref here takes the whole node down, not just the game.
    if (!status || !hiLabel || !livesLabel || !banner) return;

    char buf[24];

    // Only touch the labels when a value actually changes; otherwise every tick
    // invalidates three text areas for nothing.
    if (score != shownScore) {
        shownScore = score;
        snprintf(buf, sizeof(buf), "SCORE %05lu", (unsigned long)score);
        lv_label_set_text(status, buf);
    }
    if (hiScore != shownHi) {
        shownHi = hiScore;
        snprintf(buf, sizeof(buf), "HI %05lu", (unsigned long)hiScore);
        lv_label_set_text(hiLabel, buf);
        lv_obj_align(hiLabel, LV_ALIGN_TOP_MID, 0, 2);
    }
    if (lives != shownLives || level != shownLevel) {
        shownLives = lives;
        shownLevel = level;
        snprintf(buf, sizeof(buf), "L%u  x%u", level, lives);
        lv_label_set_text(livesLabel, buf);
        lv_obj_align(livesLabel, LV_ALIGN_TOP_RIGHT, -4, 2);
    }

    // lv_label_set_text reallocates and invalidates, and lv_obj_align forces a
    // layout pass. Doing both every tick costs more than the whole game does.
    if ((int8_t)state != shownState) {
        shownState = (int8_t)state;
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
}

// ---------------------------------------------------------------------- input

void BrickBreakerPanel::onKey(uint32_t key)
{
    if (dead || w == 0 || key != LV_KEY_ENTER) return;

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
    if (paddleX < playLeft) paddleX = playLeft;
    if (paddleX > playRight - pw) paddleX = playRight - pw;
#else
    (void)now;
#endif
}

void BrickBreakerPanel::task_handler(void)
{
    if (dead) return;
    if (w == 0) {
        activate();
        return;
    }

    const uint32_t now = lv_tick_get();
    uint32_t elapsed = now - lastStep;
    if (elapsed < TICK_MS) return;
    if (elapsed > TICK_MS * MAX_CATCHUP) elapsed = TICK_MS * MAX_CATCHUP;
    lastStep = now;

    pollTrackball(now);

    if (state == ePlaying) {
        // Run whole fixed steps so the ball covers the same ground per second
        // whatever the render rate, while each step stays small enough for
        // collision tests to stay honest.
        for (uint32_t t = 0; t < elapsed / TICK_MS && state == ePlaying; t++) step();
    } else if (state == eReady) {
        resetBall();
        layoutAim();
    }

    showAim(state == eReady);

    lv_obj_set_pos(paddle, (int16_t)paddleX, h - PADDLE_H - 2);
    lv_obj_set_pos(ball, (int16_t)ballX, (int16_t)ballY);

    updateStatus();
}
