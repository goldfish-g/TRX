#include <trx/game/camera/head.h>

#include <trx/game/camera/vars.h>
#include <trx/game/clock.h>
#include <trx/game/collision/los.h>
#include <trx/game/interpolation.h>
#include <trx/game/items/anim.h>
#include <trx/game/lara.h>
#include <trx/game/lara/const.h>
#include <trx/game/matrix.h>
#include <trx/game/rooms.h>
#include <trx/core/utils.h>
#include <trx/game/const.h>

#include <math.h>

// How far behind Lara the player stands inside the game, how far above her
// hips the player's eyes are, which puts them a little over her head, and
// how much room is left to a wall.
#define M_FOLLOW_DISTANCE 1400.0
#define M_FOLLOW_EYE_RISE 400.0
#define M_FOLLOW_MARGIN 180.0
// With less room than this behind her the player is raised a little more,
// by up to this much, so as to look over her head instead of into it.
#define M_FOLLOW_MIN_DISTANCE 600.0
#define M_FOLLOW_OVERHEAD_RISE 60.0
// How quickly the player is brought to where the rules put them, as shares
// of the way per second: round behind her, along with her, and back out to
// the full distance.
#define M_FOLLOW_TURN_RATE 2.5
#define M_FOLLOW_MOVE_RATE 5.0
#define M_FOLLOW_BACK_RATE 2.0

// Where the player stands inside the game. The game's own camera cuts and
// swings for a screen: it looks down on a ledge, jumps to fixed spots and
// circles in a fight, all of which throw a player about who is standing in
// the world. So the player follows Lara by rules of their own: behind her,
// level, turning after her and moving with her gently, no further back than
// there is a clear line to her, and just over her head where a wall is at
// her back.
static struct {
    bool is_valid;
    // Where Lara's hips are taken to be, evened out over time.
    double anchor[3];
    double yaw;
    double distance;
    double time;
    XYZ_32 pos;
    int16_t room_num;
} m_Follow = {};

static int16_t m_HeadRoom = NO_ROOM;

// The shorter way round from one angle to another, in the game's units of
// 65536 to the turn.
static double M_AngleBetween(const double from, const double to)
{
    double turn = fmod(to - from, 65536.0);
    if (turn >= 32768.0) {
        turn -= 65536.0;
    } else if (turn < -32768.0) {
        turn += 65536.0;
    }
    return turn;
}

// Where Lara's hips are. The game keeps her own position on the ground and
// moves it in one step where she climbs, vaults or pulls herself up, with
// the animation carrying her body across the gap. Her hips are where the
// body is, and move the whole way.
static XYZ_32 M_GetLaraHips(const ITEM *const lara)
{
    ANIM_FRAME *frames[2] = { nullptr, nullptr };
    int32_t rate = 0;
    const int32_t frac = Item_GetFrames(lara, frames, &rate);
    if (frames[0] == nullptr) {
        return lara->pos;
    }
    double offset[3] = {
        frames[0]->offset.x,
        frames[0]->offset.y,
        frames[0]->offset.z,
    };
    if (frac != 0 && rate != 0 && frames[1] != nullptr) {
        const double share = (double)frac / rate;
        offset[0] += (frames[1]->offset.x - offset[0]) * share;
        offset[1] += (frames[1]->offset.y - offset[1]) * share;
        offset[2] += (frames[1]->offset.z - offset[2]) * share;
    }
    // The offset is in her own axes, which turn with her.
    const double angle = lara->rot.y * M_PI / 32768.0;
    return (XYZ_32) {
        .x = lara->pos.x
            + (int32_t)(offset[0] * cos(angle) + offset[2] * sin(angle)),
        .y = lara->pos.y + (int32_t)offset[1],
        .z = lara->pos.z
            + (int32_t)(-offset[0] * sin(angle) + offset[2] * cos(angle)),
    };
}

// Whether the given spot is in the open: between a floor and a ceiling, with
// a little to spare. Also gives the room it is in.
static bool M_IsInTheOpen(const XYZ_32 pos, int16_t *const room_num)
{
    const SECTOR *const sector = Room_GetSector(pos, room_num);
    if (sector == nullptr) {
        return false;
    }
    const int32_t floor = Room_GetHeight(sector, pos);
    const int32_t ceiling = Room_GetCeiling(sector, pos);
    return floor != NO_HEIGHT && ceiling != NO_HEIGHT && pos.y < floor - 40
        && pos.y > ceiling + 40;
}

static void M_UpdateFollow(void)
{
    const ITEM *const lara = Lara_GetItem();
    if (lara == nullptr) {
        m_Follow.is_valid = false;
        return;
    }
    const XYZ_32 hips = M_GetLaraHips(lara);
    const double lara_yaw = lara->interp.result.rot.y;
    const double now = Clock_GetRealTime();
    const double elapsed = fmin(fmax(now - m_Follow.time, 0.0), 0.1);
    m_Follow.time = now;

    // A jump of more than a few steps is the game putting Lara somewhere
    // else, and the player goes with her at once.
    const bool is_far = m_Follow.is_valid
        && (fabs(m_Follow.anchor[0] - hips.x) > 6 * WALL_L
            || fabs(m_Follow.anchor[1] - hips.y) > 6 * WALL_L
            || fabs(m_Follow.anchor[2] - hips.z) > 6 * WALL_L);
    if (!m_Follow.is_valid || is_far) {
        m_Follow.anchor[0] = hips.x;
        m_Follow.anchor[1] = hips.y;
        m_Follow.anchor[2] = hips.z;
        m_Follow.yaw = lara_yaw;
        m_Follow.distance = 0.0;
        m_Follow.is_valid = true;
    } else {
        const double move = 1.0 - exp(-elapsed * M_FOLLOW_MOVE_RATE);
        m_Follow.anchor[0] += (hips.x - m_Follow.anchor[0]) * move;
        m_Follow.anchor[1] += (hips.y - m_Follow.anchor[1]) * move;
        m_Follow.anchor[2] += (hips.z - m_Follow.anchor[2]) * move;
        m_Follow.yaw += M_AngleBetween(m_Follow.yaw, lara_yaw)
            * (1.0 - exp(-elapsed * M_FOLLOW_TURN_RATE));
        m_Follow.yaw = M_AngleBetween(0.0, m_Follow.yaw);
    }

    // Look straight back from her chest to where the player would stand at
    // the full distance. A wall in the way cuts the line short, and the
    // player stands a little before it.
    const double angle = m_Follow.yaw * M_PI / 32768.0;
    int16_t room_num = Room_GetIndexFromPos(hips);
    if (room_num == NO_ROOM) {
        room_num = lara->room_num;
    }
    const GAME_VECTOR start = {
        .x = hips.x,
        .y = hips.y - 200,
        .z = hips.z,
        .room_num = room_num,
    };
    GAME_VECTOR back = {
        .x = start.x - (int32_t)(sin(angle) * M_FOLLOW_DISTANCE),
        .y = start.y,
        .z = start.z - (int32_t)(cos(angle) * M_FOLLOW_DISTANCE),
        .room_num = room_num,
    };
    LOS_Check(&start, &back, false);
    const double dx = back.x - start.x;
    const double dz = back.z - start.z;
    const double allowed = fmax(sqrt(dx * dx + dz * dz) - M_FOLLOW_MARGIN, 0.0);

    // Coming up against a wall the player has to give way at once; moving
    // back out again can take its time, so that the view glides.
    if (allowed < m_Follow.distance) {
        m_Follow.distance = allowed;
    } else {
        m_Follow.distance += (allowed - m_Follow.distance)
            * (1.0 - exp(-elapsed * M_FOLLOW_BACK_RATE));
    }

    // The nearer the player has to come, the more they are raised, up to
    // standing right over her.
    const double squeeze =
        fmin(fmax(1.0 - m_Follow.distance / M_FOLLOW_MIN_DISTANCE, 0.0), 1.0);
    const double rise = M_FOLLOW_EYE_RISE
        + M_FOLLOW_OVERHEAD_RISE * squeeze * squeeze * (3.0 - 2.0 * squeeze);

    // And no higher than there is room for above that spot.
    const GAME_VECTOR below = {
        .x = (int32_t)(hips.x - sin(angle) * m_Follow.distance),
        .y = start.y,
        .z = (int32_t)(hips.z - cos(angle) * m_Follow.distance),
        .room_num = room_num,
    };
    GAME_VECTOR above = below;
    above.y = hips.y - (int32_t)rise - 60;
    LOS_Check(&below, &above, false);
    const double top = fmax(above.y + 60, hips.y - rise);

    // The lines above can miss: a spot can be clear to look at and still
    // sit in a wall or over a ceiling, most of all in the low tunnels Lara
    // swims through. So the spot is tried, and brought in towards her and
    // down to her level until it is in the open. Where she is always is.
    for (int32_t step = 0; step <= 8; step++) {
        const double share = 1.0 - step / 8.0;
        const double distance = m_Follow.distance * share;
        m_Follow.pos = (XYZ_32) {
            .x = (int32_t)(m_Follow.anchor[0] - sin(angle) * distance),
            .y = (int32_t)(m_Follow.anchor[1] + (top - hips.y) * share),
            .z = (int32_t)(m_Follow.anchor[2] - cos(angle) * distance),
        };
        m_Follow.room_num = room_num;
        if (M_IsInTheOpen(m_Follow.pos, &m_Follow.room_num)) {
            return;
        }
    }
    m_Follow.pos = hips;
    m_Follow.room_num = room_num;
}

// One coordinate of the camera between two ticks, in fractions of a unit.
// Where the game did not go between the two, because the camera cut, the
// place it did take is kept.
static double M_GetExactCoord(
    const int32_t prev, const int32_t current, const int32_t taken,
    const double rate)
{
    const double exact = prev + (current - prev) * rate;
    return fabs(exact - taken) < 2.0 ? exact : taken;
}

bool Camera_IsOnHead(void)
{
    return m_Follow.is_valid && Clock_GetRealTime() - m_Follow.time < 0.25;
}

int16_t Camera_GetHeadRoom(void)
{
    return m_Follow.is_valid && m_HeadRoom != NO_ROOM
        ? m_HeadRoom
        : g_Camera.interp.room_num;
}

// Puts the camera on the head of a player in VR, who stands where the follow
// rules above put them. Which way the player looks is the head's to say, so
// the horizon the player sees stays level with the one they stand on.
void Camera_ApplyHead(const VR_VIEW *const head)
{
    // Both eyes of a frame see the same moment.
    if (VR_Session_GetDrawnEye() <= 0 || !m_Follow.is_valid) {
        M_UpdateFollow();
    }
    const XYZ_16 rot = { .y = (int16_t)(int32_t)m_Follow.yaw };
    Matrix_GenerateW2V(&m_Follow.pos, &rot);

    // The headset counts up and backwards where the camera counts down and
    // forwards, so those two axes change sign going from one to the other.
    const double sign[3] = { 1.0, -1.0, -1.0 };
    double head_rot[3][3];
    double offset[3] = {};
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            // The view undoes the turn of the head, which transposes it.
            head_rot[i][j] = sign[i] * sign[j] * head->rot[j][i];
        }
    }
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t j = 0; j < 3; j++) {
            offset[i] -=
                head_rot[i][j] * sign[j] * head->pos[j] * VR_UNITS_PER_METER;
        }
    }
    Matrix_MoveW2V(head_rot, offset);

    // Finding what is visible starts from the room the head is in, which a
    // lean or a step can make another one than the spot it is carried on.
    m_HeadRoom = m_Follow.room_num;
    int16_t room_num = m_Follow.room_num;
    if (M_IsInTheOpen(g_ViewPos, &room_num)) {
        m_HeadRoom = room_num;
    }
}

void Camera_RefineView(void)
{
    const double rate = Interpolation_GetCameraRate();
    const double pos[3] = {
        M_GetExactCoord(
            g_Camera.interp.prev.pos.x, g_Camera.pos.x,
            g_Camera.interp.result.pos.x, rate),
        M_GetExactCoord(
            g_Camera.interp.prev.pos.y + g_Camera.interp.prev.shift,
            g_Camera.pos.y + g_Camera.shift,
            g_Camera.interp.result.pos.y + g_Camera.interp.result.shift, rate),
        M_GetExactCoord(
            g_Camera.interp.prev.pos.z, g_Camera.pos.z,
            g_Camera.interp.result.pos.z, rate),
    };
    const double target[3] = {
        M_GetExactCoord(
            g_Camera.interp.prev.target.x, g_Camera.target.x,
            g_Camera.interp.result.target.x, rate),
        M_GetExactCoord(
            g_Camera.interp.prev.target.y, g_Camera.target.y,
            g_Camera.interp.result.target.y, rate),
        M_GetExactCoord(
            g_Camera.interp.prev.target.z, g_Camera.target.z,
            g_Camera.interp.result.target.z, rate),
    };
    const double forward[3] = {
        target[0] - pos[0],
        target[1] - pos[1],
        target[2] - pos[2],
    };
    Matrix_RefineW2V(pos, forward);
}

