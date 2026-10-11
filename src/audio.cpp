#include "audio.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <malloc.h>
#include <switch.h>

void logf(const char* fmt, ...); // main.cpp

namespace audio {
namespace {

struct Sound {
    std::string name;
    int rate = 22050;
    std::vector<int16_t> samples; // mono
};

struct Queued {
    const Sound* sound;
    int loops;
};

struct Channel {
    const Sound* sound = nullptr;
    double pos = 0;
    int loops = 1;      // remaining plays, 0 = forever
    float gain = 1;     // current
    float target = 1;   // what volume() last asked for
    float step = 0;     // gain change per output sample while fading
    bool stopAtZero = false;
    std::deque<Queued> queue;
};

const int kChannels = 8, kRate = 48000, kFrames = 1024;
Channel g_ch[kChannels + 1];
std::deque<int16_t> g_stream;
int g_streamRate = 22050;
double g_streamPos = 0;
std::map<std::string, Sound> g_sounds;
Mutex g_lock;
Thread g_thread;
bool g_running = false, g_ok = false, g_muted = false;
float g_master = 1.0f;

const Sound* load(const char* name) {
    auto it = g_sounds.find(name);
    if (it != g_sounds.end()) return it->second.samples.empty() ? nullptr : &it->second;
    Sound s;
    s.name = name;
    std::string path = std::string("romfs:/snd/") + name + ".wav";
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> d(size > 0 ? size : 0);
        if (fread(d.data(), 1, d.size(), f) == d.size() && d.size() > 44 && !memcmp(d.data(), "RIFF", 4)) {
            int channels = 1, bits = 16;
            for (size_t p = 12; p + 8 <= d.size();) {
                uint32_t len;
                memcpy(&len, &d[p + 4], 4);
                if (!memcmp(&d[p], "fmt ", 4) && p + 24 <= d.size()) {
                    uint16_t ch, b;
                    uint32_t rate;
                    memcpy(&ch, &d[p + 10], 2);
                    memcpy(&rate, &d[p + 12], 4);
                    memcpy(&b, &d[p + 22], 2);
                    channels = ch ? ch : 1;
                    bits = b;
                    s.rate = rate;
                } else if (!memcmp(&d[p], "data", 4)) {
                    size_t n = std::min<size_t>(len, d.size() - p - 8);
                    if (bits == 16) {
                        size_t frames = n / (2 * channels);
                        s.samples.resize(frames);
                        for (size_t i = 0; i < frames; ++i) memcpy(&s.samples[i], &d[p + 8 + i * 2 * channels], 2);
                    }
                    break;
                }
                p += 8 + len + (len & 1);
            }
        }
        fclose(f);
    }
    if (s.samples.empty()) logf("sound %s missing or unsupported", name);
    Sound& stored = g_sounds[name] = std::move(s);
    return stored.samples.empty() ? nullptr : &stored;
}

int defaultLoops(const char* name, int loops) {
    if (loops >= 0) return loops;
    return strstr(name, "loop") ? 0 : 1;
}

void start(Channel& c, const Sound* s, int loops) {
    c.sound = s;
    c.pos = 0;
    c.loops = loops;
    c.gain = c.target;
    c.step = 0;
    c.stopAtZero = false;
}

void mix(int16_t* out) {
    static float acc[kFrames];
    memset(acc, 0, sizeof acc);
    mutexLock(&g_lock);
    for (int ci = 1; ci <= kChannels; ++ci) {
        Channel& c = g_ch[ci];
        for (int i = 0; i < kFrames; ++i) {
            if (!c.sound) {
                if (c.queue.empty()) break;
                Queued q = c.queue.front();
                c.queue.pop_front();
                float keep = c.target;
                start(c, q.sound, q.loops);
                c.gain = keep;
            }
            const std::vector<int16_t>& s = c.sound->samples;
            size_t i0 = (size_t)c.pos;
            if (i0 >= s.size()) {
                if (c.loops == 0 || --c.loops > 0) {
                    c.pos = 0;
                    i0 = 0;
                } else {
                    c.sound = nullptr;
                    --i;
                    continue;
                }
            }
            float frac = (float)(c.pos - i0);
            float a = s[i0], b = i0 + 1 < s.size() ? s[i0 + 1] : a;
            acc[i] += (a + (b - a) * frac) * c.gain;
            c.pos += (double)c.sound->rate / kRate;
            if (c.step != 0) {
                c.gain += c.step;
                bool done = c.step > 0 ? c.gain >= c.target : c.gain <= (c.stopAtZero ? 0.0f : c.target);
                if (done) {
                    c.gain = c.stopAtZero ? 0.0f : c.target;
                    c.step = 0;
                    if (c.stopAtZero) {
                        c.sound = nullptr;
                        c.queue.clear();
                        c.stopAtZero = false;
                        c.gain = c.target;
                        break;
                    }
                }
            }
        }
    }
    // streamed voice
    for (int i = 0; i < kFrames && g_stream.size() > 1; ++i) {
        float frac = (float)g_streamPos;
        acc[i] += g_stream[0] + (g_stream[1] - g_stream[0]) * frac;
        g_streamPos += (double)g_streamRate / kRate;
        while (g_streamPos >= 1 && g_stream.size() > 1) {
            g_stream.pop_front();
            g_streamPos -= 1;
        }
    }
    mutexUnlock(&g_lock);
    static bool heard = false;
    if (!heard)
        for (int i = 0; i < kFrames; ++i)
            if (acc[i] != 0) {
                heard = true;
                logf("audio: first non-silent buffer mixed");
                break;
            }
    for (int i = 0; i < kFrames; ++i) {
        float v = g_muted ? 0.0f : acc[i] * 0.6f * g_master;
        int16_t o = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        out[i * 2] = out[i * 2 + 1] = o;
    }
}

int16_t* g_data[2] = {nullptr, nullptr}; // the two output buffers: the audio service reads them until the output is stopped

void threadMain(void*) {
    const size_t bytes = kFrames * 2 * sizeof(int16_t);
    const size_t alloc = (bytes + 0xfff) & ~0xfffu;
    g_data[0] = (int16_t*)memalign(0x1000, alloc);
    g_data[1] = (int16_t*)memalign(0x1000, alloc);
    int16_t** data = g_data;
    AudioOutBuffer buf[2];
    for (int i = 0; i < 2; ++i) {
        memset(data[i], 0, alloc);
        buf[i].next = nullptr;
        buf[i].buffer = data[i];
        buf[i].buffer_size = alloc;
        buf[i].data_size = bytes;
        buf[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&buf[i]);
    }
    while (g_running) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ULL)) || !released) continue;
        mix((int16_t*)released->buffer);
        audoutAppendAudioOutBuffer(released);
    }
}

} // namespace

void init() {
    mutexInit(&g_lock);
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
        logf("audio output unavailable, running silent");
        return;
    }
    g_running = true;
    if (R_FAILED(threadCreate(&g_thread, threadMain, nullptr, nullptr, 0x10000, 0x2B, -2)) || R_FAILED(threadStart(&g_thread))) {
        logf("audio thread failed, running silent");
        g_running = false;
        return;
    }
    g_ok = true;
}

void shutdown() {
    if (!g_ok) return;
    g_running = false;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    audoutStopAudioOut();
    audoutExit();
    free(g_data[0]); // only now: the service no longer reads them
    free(g_data[1]);
    g_data[0] = g_data[1] = nullptr;
    g_ok = false;
}

void play(int ch, const char* name, int loops) {
    const Sound* s = load(name);
    mutexLock(&g_lock);
    Channel& c = g_ch[ch];
    c.queue.clear();
    if (s) start(c, s, defaultLoops(name, loops));
    else c.sound = nullptr;
    mutexUnlock(&g_lock);
}

void queue(int ch, const char* name, int loops) {
    const Sound* s = load(name);
    if (!s) return;
    mutexLock(&g_lock);
    Channel& c = g_ch[ch];
    // A looping sound would never let the queue advance: let it finish its current pass.
    if (c.sound && c.loops == 0) c.loops = 1;
    c.queue.push_back({s, defaultLoops(name, loops)});
    mutexUnlock(&g_lock);
}

void stop(int ch) {
    mutexLock(&g_lock);
    g_ch[ch].sound = nullptr;
    g_ch[ch].queue.clear();
    g_ch[ch].step = 0;
    mutexUnlock(&g_lock);
}

void volume(int ch, int v) {
    mutexLock(&g_lock);
    Channel& c = g_ch[ch];
    c.target = (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0f;
    if (c.step == 0 || !c.stopAtZero) {
        c.gain = c.target;
        c.step = 0;
    }
    mutexUnlock(&g_lock);
}

void fadeTo(int ch, int v, int ms) {
    mutexLock(&g_lock);
    Channel& c = g_ch[ch];
    c.target = (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0f;
    float samples = kRate * (ms > 0 ? ms : 1) / 1000.0f;
    c.step = (c.target - c.gain) / samples;
    c.stopAtZero = false;
    if (c.step == 0) c.gain = c.target;
    mutexUnlock(&g_lock);
}

void fadeOut(int ch, int ms) {
    mutexLock(&g_lock);
    Channel& c = g_ch[ch];
    if (c.sound && c.gain > 0) {
        c.step = -c.gain / (kRate * (ms > 0 ? ms : 1) / 1000.0f);
        c.stopAtZero = true;
    } else {
        c.sound = nullptr;
        c.queue.clear();
    }
    mutexUnlock(&g_lock);
}

void fadeIn(int ch, int ms) {
    mutexLock(&g_lock);
    Channel& c = g_ch[ch];
    c.gain = 0;
    c.step = c.target / (kRate * (ms > 0 ? ms : 1) / 1000.0f);
    c.stopAtZero = false;
    mutexUnlock(&g_lock);
}

bool busy(int ch) {
    mutexLock(&g_lock);
    bool b = g_ch[ch].sound != nullptr || !g_ch[ch].queue.empty();
    mutexUnlock(&g_lock);
    return b;
}

void streamReset(int rate) {
    mutexLock(&g_lock);
    g_stream.clear();
    g_streamRate = rate > 0 ? rate : 22050;
    g_streamPos = 0;
    mutexUnlock(&g_lock);
}

void streamPush(const short* samples, int count) {
    mutexLock(&g_lock);
    g_stream.insert(g_stream.end(), samples, samples + count);
    mutexUnlock(&g_lock);
}

int streamQueued() {
    mutexLock(&g_lock);
    int n = (int)g_stream.size();
    mutexUnlock(&g_lock);
    return n;
}

void setMuted(bool m) { g_muted = m; }
void setMasterVolume(float v) { g_master = v < 0 ? 0 : v > 1 ? 1 : v; }
bool muted() { return g_muted; }

bool isPlaying(int ch, const char* name) {
    mutexLock(&g_lock);
    bool b = g_ch[ch].sound && g_ch[ch].sound->name == name;
    mutexUnlock(&g_lock);
    return b;
}

} // namespace audio
