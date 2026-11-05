#pragma once

// How many samples the audio device asks for at a time. Browsers fill it on
// the page's own thread between frames, so it has to outlast a slow frame or
// the sound stutters.
#define AUDIO_SAMPLES 2048
