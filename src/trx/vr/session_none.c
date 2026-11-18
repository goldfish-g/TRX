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

bool VR_Session_BeginFrame(void)
{
    return false;
}

const VR_VIEW *VR_Session_GetView(void)
{
    return nullptr;
}

bool VR_Session_AcquireEye(const int32_t eye, VR_SCREEN *const out_screen)
{
    return false;
}

void VR_Session_ReleaseEye(const int32_t eye)
{
}

bool VR_Session_IsEyeDrawn(const int32_t eye)
{
    return false;
}

bool VR_Session_HasEyes(void)
{
    return false;
}

bool VR_Session_AcquireScreen(const int32_t eye, VR_SCREEN *const out_screen)
{
    return false;
}

VR_MODE VR_Session_GetMode(void)
{
    return VR_MODE_THEATER;
}

void VR_Session_ToggleMode(void)
{
}

bool VR_Session_IsStereo(void)
{
    return false;
}

void VR_Session_SetDrawnEye(const int32_t eye)
{
}

int32_t VR_Session_GetDrawnEye(void)
{
    return -1;
}

void VR_Session_EndFrame(const float aspect_ratio)
{
}
