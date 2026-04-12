#include <trx/game/ui/touch_overlay.h>

#include <trx/config.h>
#include <trx/core/colors.h>
#include <trx/game/input/common.h>
#include <trx/game/objects.h>
#include <trx/game/output/const.h>
#include <trx/game/output/textures.h>
#include <trx/game/game/state.h>
#include <trx/game/lara/common.h>
#include <trx/game/ui/draw.h>
#include <trx/game/viewport.h>
#include <trx/version.h>

#include <SDL2/SDL_events.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAX_FINGERS 10
#define DPAD_DEAD_ZONE 0.50f
#define DPAD_DIR_THRESHOLD 0.38f
#define DPAD_THUMB_LIMIT 0.68f
#define ORBIT_ANGLE_STEP 45
#define HIT_GENEROSITY 1.2f
#define FINGER_RADIUS 0.025f

// Sprite index offsets within O_ALPHABET for touch overlay icons.
// Must match mapping_touch.txt and text_autogen.def.
#define TOUCH_SPRITE_JUMP 782
#define TOUCH_SPRITE_CROUCH 783
#define TOUCH_SPRITE_WALK 784
#define TOUCH_SPRITE_RUN 785
#define TOUCH_SPRITE_DRAW_WEAPON 786
#define TOUCH_SPRITE_LOOK 787
#define TOUCH_SPRITE_ACTION 788
#define TOUCH_SPRITE_ROLL 789
#define TOUCH_SPRITE_MENU 790
#define TOUCH_SPRITE_PAUSE 791
#define TOUCH_SPRITE_BULLET 792

typedef enum {
    ANCHOR_BOTTOM_LEFT,
    ANCHOR_BOTTOM_RIGHT,
    ANCHOR_TOP_CENTER,
} TOUCH_ANCHOR;

typedef struct {
    INPUT_ROLE role;
    TOUCH_ANCHOR anchor;
    float offset_x;
    float offset_y;
    float radius;
    uint8_t engine_mask; // bit 0 = TR1, bit 1 = TR2, bit 2 = TR3
    bool is_dpad;
} TOUCH_BUTTON_DEF;

typedef struct {
    float cx;
    float cy;
    float radius;
    bool visible;
    bool active;
    INPUT_ROLE role;
    bool is_dpad;
    int32_t def_index; // index into m_ButtonDefs
} TOUCH_BUTTON;

typedef struct {
    SDL_FingerID id;
    float x;
    float y;
    bool active;
} FINGER_STATE;

// Button layout matching the WebGL implementation.
// Sizes/offsets are fractions of viewport min(width, height).
// clang-format off
static const TOUCH_BUTTON_DEF m_ButtonDefs[] = {
    // D-pad
    { .role = INPUT_ROLE_UP,          .anchor = ANCHOR_BOTTOM_LEFT,  .offset_x = 0.15f, .offset_y = 0.15f, .radius = 0.11f, .engine_mask = 0x7, .is_dpad = true },

    // Main action buttons
    { .role = INPUT_ROLE_JUMP,        .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.10f, .offset_y = 0.10f, .radius = 0.065f, .engine_mask = 0x7 },
    { .role = INPUT_ROLE_ACTION,      .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.21f, .offset_y = 0.21f, .radius = 0.065f, .engine_mask = 0x7 },

    // Orbit buttons around ACTION
    { .role = INPUT_ROLE_SLOW,        .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.32f, .offset_y = 0.21f, .radius = 0.035f, .engine_mask = 0x7 },
    { .role = INPUT_ROLE_LOOK,        .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.29f, .offset_y = 0.13f, .radius = 0.035f, .engine_mask = 0x7 },
    { .role = INPUT_ROLE_ROLL,        .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.21f, .offset_y = 0.10f, .radius = 0.035f, .engine_mask = 0x7 },
    { .role = INPUT_ROLE_DRAW_WEAPON, .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.10f, .offset_y = 0.21f, .radius = 0.035f, .engine_mask = 0x3 },
    // TR3: sprint/crouch replace draw_weapon position
    { .role = INPUT_ROLE_SPRINT,      .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.10f, .offset_y = 0.21f, .radius = 0.035f, .engine_mask = 0x4 },
    { .role = INPUT_ROLE_CROUCH,      .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.13f, .offset_y = 0.29f, .radius = 0.035f, .engine_mask = 0x4 },
    // TR3: draw_weapon above action button
    { .role = INPUT_ROLE_DRAW_WEAPON, .anchor = ANCHOR_BOTTOM_RIGHT, .offset_x = 0.21f, .offset_y = 0.32f, .radius = 0.035f, .engine_mask = 0x4 },

    // Top bar
    { .role = INPUT_ROLE_INVENTORY,   .anchor = ANCHOR_TOP_CENTER,   .offset_x = -0.05f, .offset_y = 0.04f, .radius = 0.03f, .engine_mask = 0x7 },
    { .role = INPUT_ROLE_PAUSE,       .anchor = ANCHOR_TOP_CENTER,   .offset_x = 0.05f, .offset_y = 0.04f, .radius = 0.03f, .engine_mask = 0x7 },
};
// clang-format on

#define NUM_BUTTON_DEFS (sizeof(m_ButtonDefs) / sizeof(m_ButtonDefs[0]))
#define MAX_BUTTONS NUM_BUTTON_DEFS

static bool m_Visible = false;
static TOUCH_BUTTON m_Buttons[MAX_BUTTONS];
static int32_t m_NumButtons = 0;
static FINGER_STATE m_Fingers[MAX_FINGERS];
static float m_FingerRadius = 0.0f;
static float m_BorderThickness = 0.0f;

// D-pad state
static SDL_FingerID m_DpadFingerId = -1;
static bool m_DpadActive = false;
static float m_DpadThumbX = 0.0f;
static float m_DpadThumbY = 0.0f;

// External functions in touch backend
void Touch_SetPositionState(int32_t position, bool pressed);
INPUT_ROLE Touch_GetPositionRole(int32_t position);

// Position name table: D-pad directions first, then one per non-dpad button def.
// clang-format off
static const char *const m_PosNames[] = {
    "\\{controller lstick up}",    // TOUCH_POS_DPAD_UP
    "\\{controller lstick down}",  // TOUCH_POS_DPAD_DOWN
    "\\{controller lstick left}",  // TOUCH_POS_DPAD_LEFT
    "\\{controller lstick right}", // TOUCH_POS_DPAD_RIGHT
    "Jump",           // m_ButtonDefs[1]
    "Action",         // m_ButtonDefs[2]
    "Walk",           // m_ButtonDefs[3]
    "Look",           // m_ButtonDefs[4]
    "Roll",           // m_ButtonDefs[5]
    "Draw Weapon",    // m_ButtonDefs[6]  (TR1/2)
    "Sprint",         // m_ButtonDefs[7]  (TR3)
    "Crouch",         // m_ButtonDefs[8]  (TR3)
    "Draw Weapon 2",  // m_ButtonDefs[9]  (TR3)
    "Inventory",      // m_ButtonDefs[10]
    "Pause",          // m_ButtonDefs[11]
};
// clang-format on

#define NUM_POSITIONS (int32_t)(sizeof(m_PosNames) / sizeof(m_PosNames[0]))

// Selection mode state (for remap listen phase)
static bool m_SelectionMode = false;
static int32_t m_SelectedPosition = -1;
static bool m_WasVisibleBeforeSelection = false;
static int32_t m_SelectionFingerCount = 0;

static uint8_t M_GetEngineMask(void)
{
    switch (g_TRVersion) {
    case 1:
        return 0x1;
    case 2:
        return 0x2;
    case 3:
        return 0x4;
    default:
        return 0x7;
    }
}

static void M_ComputeButtonLayout(void)
{
    const int32_t vw = Viewport_GetWidth(VIEWPORT_UI);
    const int32_t vh = Viewport_GetHeight(VIEWPORT_UI);
    if (vw <= 0 || vh <= 0) {
        m_NumButtons = 0;
        return;
    }

    const float ref = (float)(vw < vh ? vw : vh);
    const float btn_scale = g_Config.input.touch_button_scale;
    const float dpad_scale = g_Config.input.touch_dpad_scale;
    const uint8_t engine_mask = M_GetEngineMask();

    m_FingerRadius = FINGER_RADIUS * ref * btn_scale;
    m_BorderThickness = ref * 0.004f * btn_scale;

    m_NumButtons = 0;
    for (int32_t i = 0; i < (int32_t)NUM_BUTTON_DEFS; i++) {
        const TOUCH_BUTTON_DEF *def = &m_ButtonDefs[i];
        if (!(def->engine_mask & engine_mask)) {
            continue;
        }

        const float scale = def->is_dpad ? (dpad_scale * 1.2f) : btn_scale;

        TOUCH_BUTTON *btn = &m_Buttons[m_NumButtons];
        btn->def_index = i;
        btn->role = def->role;
        btn->is_dpad = def->is_dpad;
        btn->radius = def->radius * ref * scale;
        btn->active = false;
        btn->visible = true;

        float ax, ay;
        switch (def->anchor) {
        case ANCHOR_BOTTOM_LEFT:
            ax = 0.0f;
            ay = (float)vh;
            btn->cx = ax + def->offset_x * ref * scale;
            btn->cy = ay - def->offset_y * ref * scale;
            break;
        case ANCHOR_BOTTOM_RIGHT:
            ax = (float)vw;
            ay = (float)vh;
            btn->cx = ax - def->offset_x * ref * scale;
            btn->cy = ay - def->offset_y * ref * scale;
            break;
        case ANCHOR_TOP_CENTER:
            ax = (float)vw / 2.0f;
            ay = 0.0f;
            btn->cx = ax + def->offset_x * ref * scale;
            btn->cy = ay + def->offset_y * ref * scale;
            break;
        }

        m_NumButtons++;
    }
}

// Circle disc fill (dark centre of each button)
static const RGBA_8888 M_DISC_NORMAL = { .r = 30, .g = 30, .b = 30, .a = 120 };
static const RGBA_8888 M_DISC_ACTIVE = { .r = 30, .g = 30, .b = 30, .a = 210 };
// Circle border ring (bright outline)
static const RGBA_8888 M_RING_NORMAL = {
    .r = 255, .g = 255, .b = 255, .a = 120
};
static const RGBA_8888 M_RING_ACTIVE = {
    .r = 255, .g = 255, .b = 255, .a = 220
};
// Icon glyph tint
static const RGBA_8888 M_BORDER_NORMAL = {
    .r = 255, .g = 255, .b = 255, .a = 113
};
static const RGBA_8888 M_BORDER_ACTIVE = {
    .r = 255, .g = 255, .b = 255, .a = 239
};

static int32_t M_GetGlyphIndex(const INPUT_ROLE role)
{
    // clang-format off
    switch (role) {
    case INPUT_ROLE_JUMP:        return TOUCH_SPRITE_JUMP;
    case INPUT_ROLE_SLOW:        return TOUCH_SPRITE_WALK;
    case INPUT_ROLE_SPRINT:      return TOUCH_SPRITE_RUN;
    case INPUT_ROLE_CROUCH:      return TOUCH_SPRITE_CROUCH;
    case INPUT_ROLE_DRAW_WEAPON: return TOUCH_SPRITE_DRAW_WEAPON;
    case INPUT_ROLE_LOOK:        return TOUCH_SPRITE_LOOK;
    case INPUT_ROLE_ACTION: {
        const LARA_INFO *const lara = Lara_GetLaraInfo();
        if (Game_IsPlaying() && lara != nullptr
            && (lara->gun_status == LGS_READY || lara->gun_status == LGS_DRAW
                || lara->gun_status == LGS_UNDRAW)) {
            return TOUCH_SPRITE_BULLET;
        }
        return TOUCH_SPRITE_ACTION;
    }
    case INPUT_ROLE_ROLL:        return TOUCH_SPRITE_ROLL;
    case INPUT_ROLE_INVENTORY:   return TOUCH_SPRITE_MENU;
    case INPUT_ROLE_PAUSE:       return TOUCH_SPRITE_PAUSE;
    default:                     return -1;
    }
    // clang-format on
}

// Draw a touch sprite from O_ALPHABET centered at (cx, cy), scaled to fit
// within a square of side `diameter` pixels.
static void M_DrawTouchSprite(
    const int32_t cx, const int32_t cy, const int32_t z,
    const int32_t glyph_idx, const int32_t diameter, const RGBA_F colors[4])
{
    if (glyph_idx < 0 || diameter <= 0) {
        return;
    }
    const OBJECT *const alphabet = Object_Get(O_ALPHABET);
    if (alphabet == nullptr || !alphabet->loaded) {
        return;
    }
    const int32_t sprite_idx = alphabet->mesh_idx + glyph_idx;
    const SPRITE_TEXTURE *const spr = Output_GetSpriteTexture(sprite_idx);
    if (spr == nullptr) {
        return;
    }
    const int32_t sw = spr->x1 - spr->x0;
    const int32_t sh = spr->y1 - spr->y0;
    if (sw <= 0 || sh <= 0) {
        return;
    }
    const int32_t scale =
        (int32_t)((float)diameter * PHD_ONE / (sw > sh ? sw : sh));
    const int32_t scale_h = scale;
    const int32_t scale_v = scale;
    const int32_t sx =
        cx - (int32_t)((float)scale_h * (spr->x0 + spr->x1) / 2.0f / PHD_ONE);
    const int32_t sy =
        cy - (int32_t)((float)scale_v * (spr->y0 + spr->y1) / 2.0f / PHD_ONE);
    UI_ScheduleDrawScreenSprite(
        sx, sy, z, scale_h, scale_v, sprite_idx, colors);
}

static void M_DrawButton(const TOUCH_BUTTON *const btn, const int32_t z)
{
    if (!btn->visible) {
        return;
    }

    const float opacity = g_Config.input.touch_opacity;
    const RGBA_8888 disc_raw = btn->active ? M_DISC_ACTIVE : M_DISC_NORMAL;
    const RGBA_8888 ring_raw = btn->active ? M_RING_ACTIVE : M_RING_NORMAL;
    const RGBA_8888 border_raw =
        btn->active ? M_BORDER_ACTIVE : M_BORDER_NORMAL;

    const RGBA_F disc_f = {
        .r = disc_raw.r / 255.0f,
        .g = disc_raw.g / 255.0f,
        .b = disc_raw.b / 255.0f,
        .a = (disc_raw.a / 255.0f) * opacity,
    };
    const RGBA_F ring_f = {
        .r = ring_raw.r / 255.0f,
        .g = ring_raw.g / 255.0f,
        .b = ring_raw.b / 255.0f,
        .a = (ring_raw.a / 255.0f) * opacity,
    };
    const RGBA_F border_f = {
        .r = border_raw.r / 255.0f,
        .g = border_raw.g / 255.0f,
        .b = border_raw.b / 255.0f,
        .a = (border_raw.a / 255.0f) * opacity,
    };

    const float cx = btn->cx;
    const float cy = btn->cy;
    const float r = btn->radius;

    // Background disc + border ring (drawn via vector, not sprite)
    UI_ScheduleDrawScreenCircle(cx, cy, 0.0f, r, z, disc_f);
    UI_ScheduleDrawScreenCircle(cx, cy, r - m_BorderThickness, r, z, ring_f);

    // Role icon, scaled to 75% of button diameter for better visibility
    const int32_t pos = btn->def_index + 3;
    const INPUT_ROLE bound_role = Touch_GetPositionRole(pos);
    const int32_t glyph_idx = M_GetGlyphIndex(bound_role);
    const int32_t icon_d = (int32_t)(r * 1.5f);
    const RGBA_F icon_colors[4] = { border_f, border_f, border_f, border_f };
    M_DrawTouchSprite(
        (int32_t)roundf(cx), (int32_t)roundf(cy), z + 1, glyph_idx, icon_d,
        icon_colors);
}

static void M_DrawDpad(const int32_t z)
{
    // Find the d-pad button
    const TOUCH_BUTTON *dpad = nullptr;
    for (int32_t i = 0; i < m_NumButtons; i++) {
        if (m_Buttons[i].is_dpad) {
            dpad = &m_Buttons[i];
            break;
        }
    }
    if (dpad == nullptr || !dpad->visible) {
        return;
    }

    const float opacity = g_Config.input.touch_opacity;
    const RGBA_F bg_disc_f = {
        .r = M_DISC_NORMAL.r / 255.0f,
        .g = M_DISC_NORMAL.g / 255.0f,
        .b = M_DISC_NORMAL.b / 255.0f,
        .a = (M_DISC_NORMAL.a / 255.0f) * opacity,
    };
    const RGBA_F bg_ring_f = {
        .r = M_RING_NORMAL.r / 255.0f,
        .g = M_RING_NORMAL.g / 255.0f,
        .b = M_RING_NORMAL.b / 255.0f,
        .a = (M_RING_NORMAL.a / 255.0f) * opacity,
    };
    const RGBA_F thumb_f = {
        .r = 200.0f / 255.0f,
        .g = 200.0f / 255.0f,
        .b = 200.0f / 255.0f,
        .a = (180.0f / 255.0f) * opacity,
    };

    const float cx = dpad->cx;
    const float cy = dpad->cy;
    const float r = dpad->radius;

    // D-pad background disc + border ring
    UI_ScheduleDrawScreenCircle(cx, cy, 0.0f, r, z, bg_disc_f);
    UI_ScheduleDrawScreenCircle(cx, cy, r - m_BorderThickness, r, z, bg_ring_f);

    // Deadzone ring
    const float dz_r = r * DPAD_DEAD_ZONE;
    UI_ScheduleDrawScreenCircle(
        cx, cy, dz_r - m_BorderThickness, dz_r, z, bg_ring_f);

    // Thumb indicator: small filled disc at current thumb position
    const float thumb_r = r * 0.25f;
    const float tx = cx + m_DpadThumbX * r;
    const float ty = cy + m_DpadThumbY * r;
    UI_ScheduleDrawScreenCircle(tx, ty, 0.0f, thumb_r, z + 1, thumb_f);
}

// --- Touch event processing ---

static void M_ResetDpad(void)
{
    m_DpadActive = false;
    m_DpadFingerId = -1;
    m_DpadThumbX = 0.0f;
    m_DpadThumbY = 0.0f;
    Touch_SetPositionState(TOUCH_POS_DPAD_UP, false);
    Touch_SetPositionState(TOUCH_POS_DPAD_DOWN, false);
    Touch_SetPositionState(TOUCH_POS_DPAD_LEFT, false);
    Touch_SetPositionState(TOUCH_POS_DPAD_RIGHT, false);
}

static void M_UpdateDpadFromFinger(
    const TOUCH_BUTTON *const dpad, const float fx, const float fy)
{
    const float dx = fx - dpad->cx;
    const float dy = fy - dpad->cy;
    const float dist = sqrtf(dx * dx + dy * dy);
    const float r = dpad->radius;

    if (dist < r * DPAD_DEAD_ZONE) {
        m_DpadThumbX = 0.0f;
        m_DpadThumbY = 0.0f;
        Touch_SetPositionState(TOUCH_POS_DPAD_UP, false);
        Touch_SetPositionState(TOUCH_POS_DPAD_DOWN, false);
        Touch_SetPositionState(TOUCH_POS_DPAD_LEFT, false);
        Touch_SetPositionState(TOUCH_POS_DPAD_RIGHT, false);
        return;
    }

    const float nx = dx / dist;
    const float ny = dy / dist;
    const float clamp =
        (dist < r * DPAD_THUMB_LIMIT) ? dist : r * DPAD_THUMB_LIMIT;
    m_DpadThumbX = (nx * clamp) / r;
    m_DpadThumbY = (ny * clamp) / r;

    Touch_SetPositionState(TOUCH_POS_DPAD_UP, ny < -DPAD_DIR_THRESHOLD);
    Touch_SetPositionState(TOUCH_POS_DPAD_DOWN, ny > DPAD_DIR_THRESHOLD);
    Touch_SetPositionState(TOUCH_POS_DPAD_LEFT, nx < -DPAD_DIR_THRESHOLD);
    Touch_SetPositionState(TOUCH_POS_DPAD_RIGHT, nx > DPAD_DIR_THRESHOLD);
}

static TOUCH_BUTTON *M_FindDpad(void)
{
    for (int32_t i = 0; i < m_NumButtons; i++) {
        if (m_Buttons[i].is_dpad && m_Buttons[i].visible) {
            return &m_Buttons[i];
        }
    }
    return nullptr;
}

static bool M_IsInDpad(
    const TOUCH_BUTTON *const dpad, const float px, const float py)
{
    const float dx = px - dpad->cx;
    const float dy = py - dpad->cy;
    return dx * dx + dy * dy
        <= dpad->radius * dpad->radius * HIT_GENEROSITY * HIT_GENEROSITY;
}

static void M_SyncButtonStates(void)
{
    // Reset all non-dpad button states
    for (int32_t i = 0; i < m_NumButtons; i++) {
        TOUCH_BUTTON *btn = &m_Buttons[i];
        if (btn->is_dpad) {
            continue;
        }
        btn->active = false;
    }

    // Each active finger activates all buttons whose circle overlaps the
    // finger's contact circle (radius m_FingerRadius), allowing a single
    // finger to press two adjacent buttons at once.
    for (int32_t f = 0; f < MAX_FINGERS; f++) {
        if (!m_Fingers[f].active) {
            continue;
        }
        if (m_DpadActive && m_Fingers[f].id == m_DpadFingerId) {
            continue;
        }
        const float fx = m_Fingers[f].x;
        const float fy = m_Fingers[f].y;
        for (int32_t i = 0; i < m_NumButtons; i++) {
            TOUCH_BUTTON *btn = &m_Buttons[i];
            if (!btn->visible || btn->is_dpad) {
                continue;
            }
            const float dx = fx - btn->cx;
            const float dy = fy - btn->cy;
            const float hit_r = btn->radius * HIT_GENEROSITY + m_FingerRadius;
            if (dx * dx + dy * dy <= hit_r * hit_r) {
                btn->active = true;
            }
        }
    }

    // Propagate button position states to input backend
    for (int32_t i = 0; i < m_NumButtons; i++) {
        TOUCH_BUTTON *btn = &m_Buttons[i];
        if (btn->is_dpad) {
            continue;
        }
        const int32_t pos = btn->def_index + 3;
        Touch_SetPositionState(pos, btn->active);
    }
}

static FINGER_STATE *M_FindFinger(const SDL_FingerID id)
{
    for (int32_t i = 0; i < MAX_FINGERS; i++) {
        if (m_Fingers[i].active && m_Fingers[i].id == id) {
            return &m_Fingers[i];
        }
    }
    return nullptr;
}

static FINGER_STATE *M_AllocFinger(void)
{
    for (int32_t i = 0; i < MAX_FINGERS; i++) {
        if (!m_Fingers[i].active) {
            return &m_Fingers[i];
        }
    }
    return nullptr;
}

// In selection mode, determine which position was tapped.
static int32_t M_HitTestPosition(const float px, const float py)
{
    // Check D-pad directions
    TOUCH_BUTTON *dpad = M_FindDpad();
    if (dpad != nullptr && M_IsInDpad(dpad, px, py)) {
        const float dx = px - dpad->cx;
        const float dy = py - dpad->cy;
        // Pick the dominant direction
        if (fabsf(dy) > fabsf(dx)) {
            return dy < 0 ? TOUCH_POS_DPAD_UP : TOUCH_POS_DPAD_DOWN;
        } else {
            return dx < 0 ? TOUCH_POS_DPAD_LEFT : TOUCH_POS_DPAD_RIGHT;
        }
    }

    // Check regular buttons
    for (int32_t i = 0; i < m_NumButtons; i++) {
        TOUCH_BUTTON *btn = &m_Buttons[i];
        if (!btn->visible || btn->is_dpad) {
            continue;
        }
        const float dx = px - btn->cx;
        const float dy = py - btn->cy;
        const float hit_r = btn->radius * HIT_GENEROSITY;
        if (dx * dx + dy * dy <= hit_r * hit_r) {
            return btn->def_index + 3;
        }
    }
    return -1;
}

static void M_HandleFingerDown(const SDL_TouchFingerEvent *const ev)
{
    const int32_t vw = Viewport_GetWidth(VIEWPORT_UI);
    const int32_t vh = Viewport_GetHeight(VIEWPORT_UI);
    const float px = ev->x * vw;
    const float py = ev->y * vh;

    // In selection mode, record which position was tapped
    if (m_SelectionMode) {
        M_ComputeButtonLayout();
        m_SelectionFingerCount++;
        const int32_t pos = M_HitTestPosition(px, py);
        if (pos >= 0) {
            m_SelectedPosition = pos;
        }
        return;
    }

    FINGER_STATE *finger = M_AllocFinger();
    if (finger == nullptr) {
        return;
    }
    finger->id = ev->fingerId;
    finger->x = px;
    finger->y = py;
    finger->active = true;

    // Check if this finger lands on the d-pad
    TOUCH_BUTTON *dpad = M_FindDpad();
    if (dpad != nullptr && !m_DpadActive && M_IsInDpad(dpad, px, py)) {
        m_DpadActive = true;
        m_DpadFingerId = ev->fingerId;
        M_UpdateDpadFromFinger(dpad, px, py);
    }

    M_SyncButtonStates();
}

static void M_HandleFingerMotion(const SDL_TouchFingerEvent *const ev)
{
    if (m_SelectionMode) {
        return;
    }

    const int32_t vw = Viewport_GetWidth(VIEWPORT_UI);
    const int32_t vh = Viewport_GetHeight(VIEWPORT_UI);
    const float px = ev->x * vw;
    const float py = ev->y * vh;

    FINGER_STATE *finger = M_FindFinger(ev->fingerId);
    if (finger == nullptr) {
        return;
    }
    finger->x = px;
    finger->y = py;

    if (m_DpadActive && ev->fingerId == m_DpadFingerId) {
        TOUCH_BUTTON *dpad = M_FindDpad();
        if (dpad != nullptr) {
            M_UpdateDpadFromFinger(dpad, px, py);
        }
    }

    M_SyncButtonStates();
}

static void M_HandleFingerUp(const SDL_TouchFingerEvent *const ev)
{
    if (m_SelectionMode) {
        if (m_SelectionFingerCount > 0) {
            m_SelectionFingerCount--;
        }
        return;
    }

    FINGER_STATE *finger = M_FindFinger(ev->fingerId);
    if (finger != nullptr) {
        finger->active = false;
    }

    if (m_DpadActive && ev->fingerId == m_DpadFingerId) {
        M_ResetDpad();
    }

    M_SyncButtonStates();
}

// --- Public API ---

bool TouchOverlay_HasAnyFingerDown(void)
{
    if (m_SelectionMode) {
        return m_SelectionFingerCount > 0;
    }
    for (int32_t i = 0; i < MAX_FINGERS; i++) {
        if (m_Fingers[i].active) {
            return true;
        }
    }
    return false;
}

void TouchOverlay_Init(void)
{
    m_Visible = false;
    m_NumButtons = 0;
    m_DpadActive = false;
    m_DpadFingerId = -1;
    m_DpadThumbX = 0.0f;
    m_DpadThumbY = 0.0f;
    m_FingerRadius = 0.0f;
    m_BorderThickness = 0.0f;
    memset(m_Fingers, 0, sizeof(m_Fingers));
}

void TouchOverlay_Shutdown(void)
{
    m_Visible = false;
    m_NumButtons = 0;
}

void TouchOverlay_SetVisible(const bool visible)
{
    m_Visible = visible;
    if (!visible) {
        // Release all touch position states
        M_ResetDpad();
        for (int32_t i = 0; i < m_NumButtons; i++) {
            if (!m_Buttons[i].is_dpad) {
                Touch_SetPositionState(m_Buttons[i].def_index + 3, false);
            }
            m_Buttons[i].active = false;
        }
        memset(m_Fingers, 0, sizeof(m_Fingers));
    }
}

bool TouchOverlay_IsVisible(void)
{
    return m_Visible;
}

void TouchOverlay_Draw(void)
{
    if (!m_Visible) {
        return;
    }

    M_ComputeButtonLayout();
    if (m_NumButtons == 0) {
        return;
    }
    M_SyncButtonStates();

    const int32_t z = 0;

    // Draw d-pad
    M_DrawDpad(z);

    // Draw action/orbit/top buttons
    for (int32_t i = 0; i < m_NumButtons; i++) {
        if (!m_Buttons[i].is_dpad) {
            M_DrawButton(&m_Buttons[i], z);
        }
    }
}

bool TouchOverlay_ProcessEvent(const SDL_Event *const event)
{
    if (!m_Visible) {
        return false;
    }

    switch (event->type) {
    case SDL_FINGERDOWN:
        M_HandleFingerDown(&event->tfinger);
        return true;
    case SDL_FINGERMOTION:
        M_HandleFingerMotion(&event->tfinger);
        return true;
    case SDL_FINGERUP:
        M_HandleFingerUp(&event->tfinger);
        return true;
    default:
        return false;
    }
}

// --- Position system API ---

int32_t TouchOverlay_GetPositionCount(void)
{
    return NUM_POSITIONS;
}

uint8_t TouchOverlay_GetPositionEngineMask(const int32_t position)
{
    if (position < 0 || position >= NUM_POSITIONS) {
        return 0;
    }
    if (position < TOUCH_POS_BUTTON_BASE) {
        // D-pad directions inherit the D-pad's engine mask
        return m_ButtonDefs[0].engine_mask;
    }
    // Non-dpad: def_index = position - 3 (positions 4+ map to defs 1+)
    const int32_t def_idx = position - 3;
    if (def_idx < 0 || def_idx >= (int32_t)NUM_BUTTON_DEFS) {
        return 0;
    }
    return m_ButtonDefs[def_idx].engine_mask;
}

INPUT_ROLE TouchOverlay_GetPositionDefaultRole(const int32_t position)
{
    if (position < 0 || position >= NUM_POSITIONS) {
        return (INPUT_ROLE)-1;
    }
    switch (position) {
    case TOUCH_POS_DPAD_UP:    return INPUT_ROLE_UP;
    case TOUCH_POS_DPAD_DOWN:  return INPUT_ROLE_DOWN;
    case TOUCH_POS_DPAD_LEFT:  return INPUT_ROLE_LEFT;
    case TOUCH_POS_DPAD_RIGHT: return INPUT_ROLE_RIGHT;
    default: {
        const int32_t def_idx = position - 3;
        if (def_idx >= 1 && def_idx < (int32_t)NUM_BUTTON_DEFS) {
            return m_ButtonDefs[def_idx].role;
        }
        return (INPUT_ROLE)-1;
    }
    }
}

const char *TouchOverlay_GetPositionName(const int32_t position)
{
    if (position < 0 || position >= NUM_POSITIONS) {
        return nullptr;
    }
    return m_PosNames[position];
}

// --- Selection mode API ---

void TouchOverlay_EnterSelectionMode(void)
{
    m_SelectionMode = true;
    m_SelectedPosition = -1;
    m_SelectionFingerCount = 0;
    m_WasVisibleBeforeSelection = m_Visible;
    m_Visible = true;
    M_ComputeButtonLayout();
}

void TouchOverlay_ExitSelectionMode(void)
{
    m_SelectionMode = false;
    m_SelectedPosition = -1;
    m_Visible = m_WasVisibleBeforeSelection;
}

int32_t TouchOverlay_GetSelectedPosition(void)
{
    const int32_t pos = m_SelectedPosition;
    m_SelectedPosition = -1;
    return pos;
}
