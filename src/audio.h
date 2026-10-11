// Eight Director-style sound channels mixed in software and sent to the Switch audio output.
// Sounds are the WAV files in romfs:/snd, named after their cast members.
#pragma once

namespace audio {

void init();
void shutdown();

// loops: how many times to play; 0 = until stopped. Sounds whose name contains "loop" default to 0.
void play(int channel, const char* name, int loops = -1);
void queue(int channel, const char* name, int loops = -1); // plays after the current sound ends
void stop(int channel);
void volume(int channel, int v);                 // 0..255
void fadeTo(int channel, int v, int milliseconds);
void fadeOut(int channel, int milliseconds);     // then stops
void fadeIn(int channel, int milliseconds);      // from silence to the current volume
bool busy(int channel);
bool isPlaying(int channel, const char* name);   // "SndC.member = ..." in the scripts

// One extra voice fed with decoded PCM (video sound): mono 16-bit at the given rate.
void streamReset(int rate);
void streamPush(const short* samples, int count);
int streamQueued(); // samples not played yet

void setMuted(bool muted); // everything, as the original menu's mute button
bool muted();

} // namespace audio
