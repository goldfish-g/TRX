#include <trx/game/clock/common.h>

#include <emscripten.h>

// Sleeping hands control back to the page, even for no time at all, so that
// it can show the frame and deliver input. Asyncify then resumes the game
// where it slept.
void Clock_Delay(const int32_t ms)
{
    emscripten_sleep(ms > 0 ? ms : 0);
}
