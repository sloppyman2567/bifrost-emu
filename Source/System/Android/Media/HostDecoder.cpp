#include "System/Android/Media/HostDecoder.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Android/Media/DecoderProbes.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#ifdef BIFROST_MEDIA_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#endif
namespace arm64emu::yggdrasil {
#ifdef BIFROST_MEDIA_FFMPEG
namespace {
class FFmpegDecoder final : public HostDecoder {
    AVCodecContext* context_ = nullptr;
    AVCodecParserContext* parser_ = nullptr;
    AVBufferRef* device_ = nullptr;
    AVPixelFormat hardware_ = AV_PIX_FMT_NONE;
    SwsContext* conversion_ = nullptr;
    std::deque<AVFrame*> frames_;
    bool reported_ = false, draining_ = false;
    int mode_;
    bool quiet_;
    const char* backend_ = "unselected";
    int64_t offset_ = 0;
    const AVCodec* codec_;
    static AVPixelFormat format(AVCodecContext* ctx, const AVPixelFormat* formats) {
        auto& self = *static_cast<FFmpegDecoder*>(ctx->opaque);
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == self.hardware_) return *p;
        if (self.mode_ == 2) return AV_PIX_FMT_NONE;
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return AV_PIX_FMT_NONE;
    }
    int receive() {
        while (frames_.size() < 32) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) return -ENOMEM;
            int result = avcodec_receive_frame(context_, frame);
            if (result < 0) {
                av_frame_free(&frame);
                return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : -EBADMSG;
            }
            if (frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096) {
                av_frame_free(&frame); return -E2BIG;
            }
            if (frame->hw_frames_ctx) {
                AVFrame* downloaded = av_frame_alloc();
                if (!downloaded) { av_frame_free(&frame); return -ENOMEM; }
                result = av_hwframe_transfer_data(downloaded, frame, 0);
                if (result >= 0) result = av_frame_copy_props(downloaded, frame);
                if (result < 0) { av_frame_free(&frame); av_frame_free(&downloaded); return -EIO; }
                backend_ = av_hwdevice_get_type_name(reinterpret_cast<AVHWDeviceContext*>(device_->data)->type);
                if (!reported_ && !quiet_) {
                    fprintf(stderr, "[media] decoder=%s backend=%s\n", codec_->name,
                            backend_);
                    reported_ = true;
                }
                av_frame_free(&frame); frame = downloaded;
            } else {
                backend_ = "native-cpu";
                if (!reported_ && !quiet_) fprintf(stderr, "[media] decoder=%s backend=native-cpu\n", codec_->name);
                reported_ = true;
            }
            try { frames_.push_back(frame); }
            catch (...) { av_frame_free(&frame); throw; }
        }
        return -ENOBUFS;
    }
    int packet(const uint8_t* data, int size, int64_t pts, int64_t dts) {
        AVPacket* packet = av_packet_alloc();
        if (!packet) return -ENOMEM;
        int result = av_new_packet(packet, size);
        if (result >= 0) {
            std::memcpy(packet->data, data, size);
            packet->pts = pts; packet->dts = dts;
            result = avcodec_send_packet(context_, packet);
            if (result == AVERROR(EAGAIN)) {
                result = receive();
                if (!result) result = avcodec_send_packet(context_, packet);
            }
        }
        av_packet_free(&packet);
        if (result < 0) return -EBADMSG;
        return receive();
    }
public:
    FFmpegDecoder(const AVCodec* codec, int mode, bool quiet) : mode_(mode), quiet_(quiet), codec_(codec) {}
    const char* backend() const override { return backend_; }
    ~FFmpegDecoder() override {
        for (auto frame : frames_) av_frame_free(&frame);
        sws_freeContext(conversion_);
        av_parser_close(parser_);
        avcodec_free_context(&context_);
        av_buffer_unref(&device_);
    }
    bool open(unsigned width, unsigned height) {
        context_ = avcodec_alloc_context3(codec_);
        if (!context_) return false;
        context_->width = width; context_->height = height;
        context_->pkt_timebase = AVRational{1, 1000000};
        context_->thread_count = 2;
        // Goldfish VPX C2 expects libvpx's immediate output: a shown frame
        // is fetched right after its decode and carries that work's opaque
        // user_priv, after which the work is released. Frame threading would
        // delay it by one frame, so VPX keeps slice/tile threading only.
        if (codec_->id == AV_CODEC_ID_VP8 || codec_->id == AV_CODEC_ID_VP9)
            context_->thread_type = FF_THREAD_SLICE;
        if (mode_ != 1) {
            for (AVHWDeviceType type : {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VULKAN}) {
                for (int i = 0; const auto* config = avcodec_get_hw_config(codec_, i); ++i) {
                    if (config->device_type != type || !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
                    if (av_hwdevice_ctx_create(&device_, type, nullptr, nullptr, 0) < 0) continue;
                    hardware_ = config->pix_fmt;
                    context_->hw_device_ctx = av_buffer_ref(device_);
                    context_->opaque = this; context_->get_format = format;
                    break;
                }
                if (device_) break;
            }
            if (!device_ && mode_ == 2) return false;
        }
        if (avcodec_open2(context_, codec_, nullptr) < 0) return false;
        if (codec_->id == AV_CODEC_ID_H264 || codec_->id == AV_CODEC_ID_HEVC) {
            parser_ = av_parser_init(codec_->id);
            if (!parser_) return false;
        }
        return true;
    }
    int decode(const uint8_t* data, size_t size, uint64_t pts) override {
        if (draining_ || size > 16 * 1024 * 1024 || size > uint64_t(INT64_MAX - offset_)) return -EINVAL;
        if (!parser_) return packet(data, size, pts, AV_NOPTS_VALUE);
        // av_parser_parse2 requires AV_INPUT_BUFFER_PADDING_SIZE zero bytes.
        std::vector<uint8_t> input(size + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(input.data(), data, size);
        int remaining = size;
        const uint8_t* cursor = input.data();
        unsigned stalled = 0;
        while (remaining) {
            uint8_t* output = nullptr; int length = 0;
            int consumed = av_parser_parse2(parser_, context_, &output, &length, cursor, remaining,
                                           pts, AV_NOPTS_VALUE, offset_);
            if (consumed < 0 || consumed > remaining || (!consumed && !length) ||
                    (!consumed && ++stalled > 2)) return -EBADMSG;
            if (consumed) stalled = 0;
            cursor += consumed; remaining -= consumed;
            offset_ += consumed;
            if (length) {
                int result = packet(output, length, parser_->pts, parser_->dts);
                if (result) return result;
            }
        }
        return 0;
    }
    int drain() override {
        if (draining_) return receive();
        if (parser_) {
            uint8_t* output = nullptr; int length = 0;
            av_parser_parse2(parser_, context_, &output, &length, nullptr, 0,
                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, offset_);
            if (length) { int result = packet(output, length, parser_->pts, parser_->dts); if (result) return result; }
        }
        int result = avcodec_send_packet(context_, nullptr);
        if (result == AVERROR(EAGAIN)) { result = receive(); if (!result) result = avcodec_send_packet(context_, nullptr); }
        if (result < 0 && result != AVERROR_EOF) return -EBADMSG;
        draining_ = true;
        return receive();
    }
    int image(DecodedVideo& output) override {
        if (frames_.empty()) return -EAGAIN;
        AVFrame* frame = frames_.front();
        output.width = frame->width; output.height = frame->height;
        output.pts = frame->pts;
        output.primaries = frame->color_primaries; output.range = frame->color_range;
        output.transfer = frame->color_trc; output.colorspace = frame->colorspace;
        int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frame->width, frame->height, 1);
        if (size <= 0 || size > 24 * 1024 * 1024) return -E2BIG;
        output.yuv.resize(size);
        uint8_t* planes[4]{}; int strides[4]{};
        av_image_fill_arrays(planes, strides, output.yuv.data(), AV_PIX_FMT_YUV420P, frame->width, frame->height, 1);
        conversion_ = sws_getCachedContext(conversion_, frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
                AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!conversion_ || sws_scale(conversion_, frame->data, frame->linesize, 0, frame->height, planes, strides) != frame->height)
            return -EIO;
        frames_.pop_front(); av_frame_free(&frame);
        return 0;
    }
};
}
namespace {
std::unique_ptr<HostDecoder> create_decoder(unsigned codec, unsigned width, unsigned height, int mode, bool quiet) {
    static const AVCodecID codecs[] = {AV_CODEC_ID_VP8, AV_CODEC_ID_VP9, AV_CODEC_ID_H264, AV_CODEC_ID_HEVC};
    if (codec >= 4 || width > 4096 || height > 4096) return nullptr;
    auto implementation = avcodec_find_decoder(codecs[codec]);
    if (!implementation) return nullptr;
    auto decoder = std::make_unique<FFmpegDecoder>(implementation, mode, quiet);
    if (!decoder->open(width, height)) return nullptr;
    return decoder;
}
bool probe_decoder(unsigned codec, int mode, std::string& backend) {
    auto decoder = create_decoder(codec, 64, 64, mode, true);
    if (!decoder) return false;
    const auto& probe = decoder_probes[codec];
    if (decoder->decode(probe.bytes, probe.size, 12345) || decoder->drain()) return false;
    DecodedVideo image;
    if (decoder->image(image) || image.width != 64 || image.height != 64 || image.pts != 12345
            || image.yuv.size() != 6144) return false;
    for (size_t i = 0; i < image.yuv.size(); ++i)
        if (image.yuv[i] != (i < 4096 ? 16 : 128)) return false;
    // The probe must produce exactly one frame, including after EOS.
    DecodedVideo extra;
    if (decoder->image(extra) != -EAGAIN) return false;
    backend = decoder->backend();
    return true;
}
}
bool HostDecoder::available() {
    return avcodec_find_decoder(AV_CODEC_ID_H264) || avcodec_find_decoder(AV_CODEC_ID_HEVC)
        || avcodec_find_decoder(AV_CODEC_ID_VP8) || avcodec_find_decoder(AV_CODEC_ID_VP9);
}
unsigned HostDecoder::routing_mask() {
    unsigned mask = 0;
    for (unsigned codec = 0; codec < 4; ++codec) {
        std::string backend;
        if (probe_decoder(codec, dbg().media_backend == 2 ? 2 : 1, backend)) mask |= 1u << codec;
    }
    return mask;
}
std::unique_ptr<HostDecoder> HostDecoder::create(unsigned codec, unsigned width, unsigned height) {
    return create_decoder(codec, width, height, dbg().media_backend, false);
}
std::string HostDecoder::capabilities_json() {
    std::string out = "{\"schema_version\":1,\"scope\":\"host-decoder\",\"probe\":{\"width\":64,\"height\":64,\"format\":\"yuv420p\",\"pattern\":\"limited-range-black\",\"pts_us\":12345},\"codecs\":[";
    for (unsigned codec = 0; codec < 4; ++codec) {
        if (codec) out += ',';
        std::string cpu_backend, gpu_backend;
        const bool cpu = probe_decoder(codec, 1, cpu_backend);
        const bool gpu = probe_decoder(codec, 2, gpu_backend);
        out += "{\"name\":\"" + std::string(decoder_probes[codec].name) + "\",\"cpu_probe_passed\":"
            + (cpu ? "true" : "false") + ",\"gpu_probe_passed\":" + (gpu ? "true" : "false")
            + ",\"gpu_backend\":\"" + gpu_backend + "\"}";
    }
    return out + "]}";
}
#else
bool HostDecoder::available() { return false; }
unsigned HostDecoder::routing_mask() { return 0; }
std::unique_ptr<HostDecoder> HostDecoder::create(unsigned, unsigned, unsigned) { return nullptr; }
std::string HostDecoder::capabilities_json() {
    return "{\"schema_version\":1,\"scope\":\"host-decoder\",\"disabled_reason\":\"FFmpeg support not built\",\"codecs\":[]}";
}
#endif
}
