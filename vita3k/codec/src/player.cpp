// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <codec/state.h>

#include <util/fs.h>
#include <util/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <limits>

namespace {

constexpr int PLAYER_AVIO_BUFFER_SIZE = 64 * 1024;

int read_player_source(void *opaque, uint8_t *buffer, int buffer_size) {
    auto &player = *static_cast<PlayerState *>(opaque);
    if (buffer_size <= 0)
        return AVERROR(EINVAL);
    if (player.source_cursor >= player.current_source.size)
        return AVERROR_EOF;

    const auto count = static_cast<size_t>(std::min<uint64_t>(
        static_cast<uint64_t>(buffer_size), player.current_source.size - player.source_cursor));
    const auto read = player.current_source.read_at(player.source_cursor, std::span(buffer, count));
    if (!read || *read == 0 || *read > count || *read > static_cast<size_t>(std::numeric_limits<int>::max()))
        return AVERROR(EIO);
    player.source_cursor += *read;
    return static_cast<int>(*read);
}

int64_t seek_player_source(void *opaque, int64_t offset, int whence) {
    auto &player = *static_cast<PlayerState *>(opaque);
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE)
        return player.current_source.size <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? static_cast<int64_t>(player.current_source.size)
            : AVERROR(EINVAL);

    if (player.current_source.size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return AVERROR(EINVAL);
    int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = static_cast<int64_t>(player.source_cursor);
    } else if (whence == SEEK_END) {
        base = static_cast<int64_t>(player.current_source.size);
    } else if (whence != SEEK_SET) {
        return AVERROR(EINVAL);
    }

    if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset)
        || (offset < 0 && base < std::numeric_limits<int64_t>::min() - offset))
        return AVERROR(EINVAL);
    const int64_t position = base + offset;
    if (position < 0 || static_cast<uint64_t>(position) > player.current_source.size)
        return AVERROR(EINVAL);
    player.source_cursor = static_cast<uint64_t>(position);
    return position;
}

} // namespace

uint64_t PlayerState::get_framerate_microseconds() {
    AVRational rational = format->streams[video_stream_id]->avg_frame_rate;
    return 1000000ull * rational.den / rational.num;
}

DecoderSize PlayerState::get_size() {
    if (video_context)
        return { { static_cast<uint32_t>(video_context->width), static_cast<uint32_t>(video_context->height) } };

    return {};
}

void PlayerState::pop_video() {
    switch_video(videos_queue.front());
    videos_queue.pop();
}

void PlayerState::free_video() {
    if (video_context)
        avcodec_free_context(&video_context);

    if (audio_context)
        avcodec_free_context(&audio_context);

    if (format)
        avformat_close_input(&format);

    if (avio) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    }

    while (!video_packets.empty()) {
        AVPacket *packet = video_packets.front();
        av_packet_free(&packet);
        video_packets.pop();
    }

    while (!audio_packets.empty()) {
        AVPacket *packet = audio_packets.front();
        av_packet_free(&packet);
        audio_packets.pop();
    }

    video_playing.clear();
    current_source = {};
    source_cursor = 0;
}

void PlayerState::switch_video(PlayerSource source) {
    free_video();
    current_source = std::move(source);

    int error = 0;
    if (current_source.read_at) {
        format = avformat_alloc_context();
        auto *buffer = static_cast<uint8_t *>(av_malloc(PLAYER_AVIO_BUFFER_SIZE));
        if (!format || !buffer) {
            if (format)
                avformat_free_context(format);
            format = nullptr;
            av_free(buffer);
            current_source = {};
            return;
        }
        avio = avio_alloc_context(buffer, PLAYER_AVIO_BUFFER_SIZE, 0, this, read_player_source, nullptr, seek_player_source);
        if (!avio) {
            av_free(buffer);
            avformat_free_context(format);
            format = nullptr;
            current_source = {};
            return;
        }
        format->pb = avio;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        error = avformat_open_input(&format, nullptr, nullptr, nullptr);
    } else {
        error = avformat_open_input(&format, current_source.name.c_str(), nullptr, nullptr);
    }
    if (error < 0) {
        LOG_ERROR("Failed to open video source '{}': {}", current_source.name, codec_error_name(error));
        free_video();
        return;
    }
    video_playing = current_source.name;

    // Load stream info.
    error = avformat_find_stream_info(format, nullptr);
    if (error < 0) {
        LOG_ERROR("Failed to read video stream info for '{}': {}", current_source.name, codec_error_name(error));
        free_video();
        return;
    }

    video_stream_id = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_id = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_id >= 0) {
        AVStream *video_stream = format->streams[video_stream_id];
        const AVCodec *video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
        video_context = avcodec_alloc_context3(video_codec);
        avcodec_parameters_to_context(video_context, video_stream->codecpar);
        avcodec_open2(video_context, video_codec, nullptr);
    }

    if (audio_stream_id >= 0) {
        AVStream *audio_stream = format->streams[audio_stream_id];
        const AVCodec *audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        audio_context = avcodec_alloc_context3(audio_codec);
        avcodec_parameters_to_context(audio_context, audio_stream->codecpar);
        avcodec_open2(audio_context, audio_codec, nullptr);
    }
}

bool PlayerState::next_packet(int32_t stream_id) {
    std::queue<AVPacket *> &this_queue = stream_id == video_stream_id ? video_packets : audio_packets;
    std::queue<AVPacket *> &other_queue = stream_id != video_stream_id ? video_packets : audio_packets;

    while (true) {
        if (!this_queue.empty()) {
            AVPacket *this_packet = this_queue.front();
            this_queue.pop();

            if (stream_id == video_stream_id) {
                int err = avcodec_send_packet(video_context, this_packet);
                assert(err == 0);
            }

            if (stream_id == audio_stream_id) {
                int err = avcodec_send_packet(audio_context, this_packet);
                assert(err == 0);
            }

            av_packet_free(&this_packet);
            return true;
        }

        AVPacket *packet = av_packet_alloc();
        if (av_read_frame(format, packet) != 0)
            return false;

        if (packet->stream_index == stream_id) {
            this_queue.push(packet);
        } else {
            other_queue.push(packet);
        }
    }
}

std::vector<int16_t> PlayerState::receive_audio() {
    if (audio_stream_id < 0)
        return {};

    if (video_playing.empty())
        return {};

    AVFrame *frame = av_frame_alloc();
    std::vector<int16_t> data;
    while (true) {
        int error = avcodec_receive_frame(audio_context, frame);

        if (error == AVERROR(EAGAIN) && next_packet(audio_stream_id))
            continue;

        if (error != 0) {
            if (videos_queue.empty()) {
                // Stop playing videos or
                video_playing.clear();
                break;
            } else {
                // Play the next video (if there is any).
                switch_video(std::move(videos_queue.front()));
                videos_queue.pop();
                continue;
            }
        }

        LOG_WARN_IF(frame->format != AV_SAMPLE_FMT_FLTP, "Unknown audio format {}.", frame->format);

        last_channels = frame->ch_layout.nb_channels;
        last_sample_count = frame->nb_samples;
        last_sample_rate = frame->sample_rate;

        data.resize(frame->nb_samples * frame->ch_layout.nb_channels);

        for (int a = 0; a < frame->nb_samples; a++) {
            for (int b = 0; b < frame->ch_layout.nb_channels; b++) {
                auto *frame_data = reinterpret_cast<float *>(frame->data[b]);
                float current_sample = frame_data[a];
                int16_t pcm_sample = current_sample * INT16_MAX;

                data[a * frame->ch_layout.nb_channels + b] = pcm_sample;
            }
        }

        break;
    }

    av_frame_free(&frame);
    return data;
}

std::vector<uint8_t> PlayerState::receive_video() {
    if (video_stream_id < 0)
        return {};

    if (video_playing.empty())
        return {};

    AVFrame *frame = av_frame_alloc();
    std::vector<uint8_t> data;
    while (true) {
        int error = avcodec_receive_frame(video_context, frame);

        if (error == AVERROR(EAGAIN) && next_packet(video_stream_id))
            continue;

        if (error != 0) {
            if (videos_queue.empty()) {
                // Stop playing videos or
                video_playing.clear();
                break;
            } else {
                // Play the next video (if there is any).
                switch_video(std::move(videos_queue.front()));
                videos_queue.pop();
                continue;
            }
        }

        last_timestamp = frame->best_effort_timestamp;

        data.resize(H264DecoderState::buffer_size(
            { { static_cast<uint32_t>(video_context->width), static_cast<uint32_t>(video_context->height) } }));
        copy_yuv_data_from_frame(frame, data.data(), frame->width, frame->height, false);

        break;
    }

    av_frame_free(&frame);
    return data;
}

void PlayerState::queue(const std::string &path) {
    if (fs::exists(path)) {
        queue(PlayerSource{ .name = path });
    } else {
        LOG_INFO("Cannot find video: {}", path);
    }
}

void PlayerState::queue(PlayerSource source) {
    LOG_INFO("Queued video: '{}'.", source.name);
    if (video_playing.empty())
        switch_video(std::move(source));
    else
        videos_queue.push(std::move(source));
}

PlayerState::~PlayerState() {
    free_video();

    video_playing.clear();
    videos_queue = {};
}
