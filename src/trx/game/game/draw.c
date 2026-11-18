#include <trx/config.h>
#include <trx/game/camera.h>
#include <trx/game/cutseq.h>
#include <trx/game/interpolation.h>
#include <trx/game/lua/events/scene.h>
#include <trx/game/output.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/ui/draw.h>

void Game_Draw(const bool draw_overlay)
{
    Interpolation_Interpolate();
    CutSeq_PreDraw();
    SceneCompositor_BeginWorld();
    Camera_Apply();
    Output_FlushPendingFog();
    Room_DrawAllRooms(
        SceneCompositor_IsStereo() ? Camera_GetHeadRoom()
                                   : g_Camera.interp.room_num,
        g_Camera.target.room_num);
    LUA_Scene_Paint();
    if (draw_overlay && !CutSeq_IsActive()) {
        UI_DrawBinocularMask();
    }
    CutSeq_DrawOverlay();
    Output_Overlay_DrawLetterbox();
    SceneCompositor_Flush();
    SceneCompositor_EndWorld();
    if (g_Config.visuals.enable_reflections) {
        Output_Textures_UpdateEnvironmentMap();
    }
}
