#pragma once

// How many samples the audio device asks for at a time. A small buffer keeps
// the latency down, as the device fills it from its own thread.
#define AUDIO_SAMPLES 500
