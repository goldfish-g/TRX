#include <trx/vr/session.h>

void VR_Session_PrepareSDL(void)
{
}

RESULT VR_Session_Start(void *const window)
{
    return FAIL("this build of TRX has no VR support");
}

void VR_Session_Stop(void)
{
}

void VR_Session_GetScreenSize(
    int32_t *const out_width, int32_t *const out_height)
{
    *out_width = 0;
    *out_height = 0;
}

bool VR_Session_IsActive(void)
{
    return false;
}

bool VR_Session_PacesFrames(void)
{
    return false;
}

bool VR_Session_IsPlayerAway(void)
{
    return false;
}

void VR_Session_Recenter(void)
{
}

bool VR_Session_BeginFrame(VR_SCREEN *const out_screen)
{
    return false;
}

void VR_Session_EndFrame(const float aspect_ratio)
{
}
