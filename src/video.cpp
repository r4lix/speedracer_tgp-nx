#include "video.h"

#include <cstdlib>
#include <vector>

#include <glad/glad.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "audio.h"

void logf(const char* fmt, ...); // main.cpp

namespace {
AVCodecContext* openDecoder(AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}
} // namespace

bool Video::open(const char* path) {
    close();
    if (avformat_open_input(&fmt_, path, nullptr, nullptr) < 0) {
        logf("video %s: cannot open", path);
        fmt_ = nullptr;
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        logf("video %s: no stream info", path);
        close();
        return false;
    }
    videoStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (videoStream_ >= 0) video_ = openDecoder(fmt_->streams[videoStream_]);
    if (audioStream_ >= 0) audio_ = openDecoder(fmt_->streams[audioStream_]);
    if (!video_) {
        logf("video %s: no decoder for the picture", path);
        close();
        return false;
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    width_ = video_->width;
    height_ = video_->height;
    clock_ = 0;
    shownPts_ = -1;
    finished_ = false;
    audio::streamReset(audio_ ? audio_->sample_rate : 22050);
    logf("video %s: %dx%d%s", path, width_, height_, audio_ ? " with sound" : "");
    return true;
}

void Video::close() {
    if (sws_) sws_freeContext(sws_);
    sws_ = nullptr;
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (video_) avcodec_free_context(&video_);
    if (audio_) avcodec_free_context(&audio_);
    if (fmt_) avformat_close_input(&fmt_);
    if (texture_) glDeleteTextures(1, &texture_);
    texture_ = 0;
    free(rgba_);
    rgba_ = nullptr;
    videoStream_ = audioStream_ = -1;
    audio::streamReset(22050);
}

bool Video::decodeNext() {
    if (av_read_frame(fmt_, packet_) < 0) return false;
    if (packet_->stream_index == videoStream_ && avcodec_send_packet(video_, packet_) >= 0) {
        while (avcodec_receive_frame(video_, frame_) >= 0) {
            if (frame_->width != width_ || frame_->height != height_ || !rgba_) {
                width_ = frame_->width;
                height_ = frame_->height;
                free(rgba_);
                rgba_ = (unsigned char*)malloc((size_t)width_ * height_ * 4);
                if (sws_) sws_freeContext(sws_);
                sws_ = sws_getContext(width_, height_, (AVPixelFormat)frame_->format, width_, height_, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                      nullptr, nullptr, nullptr);
                if (texture_) glDeleteTextures(1, &texture_);
                glGenTextures(1, &texture_);
                glBindTexture(GL_TEXTURE_2D, texture_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            if (sws_ && rgba_) {
                uint8_t* dst[4] = {rgba_, nullptr, nullptr, nullptr};
                int stride[4] = {width_ * 4, 0, 0, 0};
                sws_scale(sws_, frame_->data, frame_->linesize, 0, height_, dst, stride);
                glBindTexture(GL_TEXTURE_2D, texture_);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, rgba_);
            }
            AVRational tb = fmt_->streams[videoStream_]->time_base;
            if (frame_->best_effort_timestamp != AV_NOPTS_VALUE) shownPts_ = frame_->best_effort_timestamp * av_q2d(tb);
        }
    } else if (packet_->stream_index == audioStream_ && audio_ && avcodec_send_packet(audio_, packet_) >= 0) {
        while (avcodec_receive_frame(audio_, frame_) >= 0) {
            // First channel only (the clips are mono), converted to 16-bit.
            std::vector<short> pcm(frame_->nb_samples);
            const AVSampleFormat f = (AVSampleFormat)frame_->format;
            for (int i = 0; i < frame_->nb_samples; ++i) {
                float v = 0;
                if (f == AV_SAMPLE_FMT_FLTP || f == AV_SAMPLE_FMT_FLT) v = ((const float*)frame_->data[0])[f == AV_SAMPLE_FMT_FLT ? i * frame_->ch_layout.nb_channels : i];
                else if (f == AV_SAMPLE_FMT_S16P || f == AV_SAMPLE_FMT_S16) v = ((const short*)frame_->data[0])[f == AV_SAMPLE_FMT_S16 ? i * frame_->ch_layout.nb_channels : i] / 32768.0f;
                pcm[i] = (short)(v > 1 ? 32767 : v < -1 ? -32768 : v * 32767);
            }
            audio::streamPush(pcm.data(), (int)pcm.size());
        }
    }
    av_packet_unref(packet_);
    return true;
}

void Video::update(double seconds) {
    if (!fmt_ || finished_) return;
    clock_ += seconds;
    // Read ahead until the picture on screen is current; sound packets met on the way are queued.
    int guard = 0;
    while (shownPts_ < clock_ && guard++ < 200)
        if (!decodeNext()) {
            // End of file: let the queued sound drain before reporting the end.
            if (audio::streamQueued() < 2000) finished_ = true;
            break;
        }
}
