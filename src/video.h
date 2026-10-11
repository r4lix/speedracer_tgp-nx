// Plays one of the game's FLV clips (Sorenson H.263 video, MP3 sound) through ffmpeg:
// frames go to a GL texture, sound to the audio stream voice.
#pragma once

class Video {
public:
    ~Video() { close(); }
    bool open(const char* path);
    void close();
    void update(double seconds); // advance the clock; decodes whatever became due
    bool isOpen() const { return fmt_ != nullptr; }
    bool finished() const { return finished_; }
    unsigned texture() const { return texture_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool decodeNext(); // one packet; false at end of file
    struct AVFormatContext* fmt_ = nullptr;
    struct AVCodecContext* video_ = nullptr;
    struct AVCodecContext* audio_ = nullptr;
    struct AVFrame* frame_ = nullptr;
    struct AVPacket* packet_ = nullptr;
    struct SwsContext* sws_ = nullptr;
    int videoStream_ = -1, audioStream_ = -1;
    unsigned texture_ = 0;
    int width_ = 0, height_ = 0;
    double clock_ = 0, shownPts_ = -1;
    bool finished_ = false;
    unsigned char* rgba_ = nullptr;
};
