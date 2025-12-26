#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <vector>
#include <memory>
#include <sstream>
#include <map>
#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "httplib.h"

// 简单的JSON生成函数
std::string create_json_response(bool success, const std::string& error = "") {
    std::stringstream ss;
    ss << "{";
    ss << "\"success\":" << (success ? "true" : "false");
    if (!error.empty()) {
        ss << ",\"error\":\"" << error << "\"";
    }
    ss << "}";
    return ss.str();
}

std::string create_status_json(bool is_streaming, const std::vector<std::string>& streams = {}) {
    std::stringstream ss;
    ss << "{";
    ss << "\"is_streaming\":" << (is_streaming ? "true" : "false");
    
    if (!streams.empty()) {
        ss << ",\"streams\":[";
        for (size_t i = 0; i < streams.size(); i++) {
            if (i > 0) ss << ",";
            ss << "\"" << streams[i] << "\"";
        }
        ss << "]";
    }
    ss << "}";
    return ss.str();
}

// JPEG编码器类
class JPEGEncoder {
private:
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    
    int last_src_width = 0;
    int last_src_height = 0;
    AVPixelFormat last_src_format = AV_PIX_FMT_NONE;
    int last_dst_width = 0;
    int last_dst_height = 0;
    
public:
    JPEGEncoder(int width = 1280, int height = 720) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) {
            throw std::runtime_error("JPEG codec not found");
        }
        
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            throw std::runtime_error("Could not allocate codec context");
        }
        
        codec_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        codec_ctx->width = width;
        codec_ctx->height = height;
        codec_ctx->time_base = {1, 25};
        codec_ctx->framerate = {25, 1};
        codec_ctx->qmin = 10;
        codec_ctx->qmax = 30;
        
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            throw std::runtime_error("Could not open codec");
        }
        
        frame = av_frame_alloc();
        pkt = av_packet_alloc();
    }
    
    ~JPEGEncoder() {
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
    }
    
    std::vector<uint8_t> encode(AVFrame* input_frame, int target_width = -1, int target_height = -1) {
        std::vector<uint8_t> jpeg_data;
        
        if (!input_frame || !input_frame->data[0]) {
            return jpeg_data;
        }
        
        int encode_width = (target_width > 0) ? target_width : codec_ctx->width;
        int encode_height = (target_height > 0) ? target_height : codec_ctx->height;
        
        bool need_new_sws = false;
        if (!sws_ctx) {
            need_new_sws = true;
        } else if (last_src_width != input_frame->width || 
                   last_src_height != input_frame->height ||
                   last_src_format != (AVPixelFormat)input_frame->format ||
                   last_dst_width != encode_width || 
                   last_dst_height != encode_height) {
            need_new_sws = true;
        }
        
        if (need_new_sws) {
            if (sws_ctx) {
                sws_freeContext(sws_ctx);
                sws_ctx = nullptr;
            }
            
            sws_ctx = sws_getContext(
                input_frame->width, input_frame->height,
                (AVPixelFormat)input_frame->format,
                encode_width, encode_height,
                codec_ctx->pix_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            
            if (!sws_ctx) {
                std::cerr << "Failed to create sws context" << std::endl;
                return jpeg_data;
            }
            
            last_src_width = input_frame->width;
            last_src_height = input_frame->height;
            last_src_format = (AVPixelFormat)input_frame->format;
            last_dst_width = encode_width;
            last_dst_height = encode_height;
        }
        
        if (frame->width != encode_width || frame->height != encode_height) {
            av_frame_unref(frame);
            frame->width = encode_width;
            frame->height = encode_height;
            frame->format = codec_ctx->pix_fmt;
            
            if (av_frame_get_buffer(frame, 32) < 0) {
                std::cerr << "Failed to allocate frame buffer" << std::endl;
                return jpeg_data;
            }
        }
        
        sws_scale(sws_ctx, input_frame->data, input_frame->linesize,
                  0, input_frame->height,
                  frame->data, frame->linesize);
        
        int ret = avcodec_send_frame(codec_ctx, frame);
        if (ret < 0) {
            std::cerr << "Error sending frame to encoder: " << ret << std::endl;
            return jpeg_data;
        }
        
        while (ret >= 0) {
            ret = avcodec_receive_packet(codec_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                std::cerr << "Error receiving packet from encoder: " << ret << std::endl;
                break;
            }
            
            jpeg_data.assign(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
        
        av_frame_unref(frame);
        return jpeg_data;
    }
};

// 视频帧缓冲区
class FrameBuffer {
private:
    std::queue<std::vector<uint8_t>> frames;
    std::mutex mtx;
    size_t max_size = 5;
    std::chrono::steady_clock::time_point last_update;
    
public:
    FrameBuffer() : last_update(std::chrono::steady_clock::now()) {}
    
    void push(const std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock(mtx);
        if (frames.size() >= max_size) {
            frames.pop();
        }
        frames.push(frame);
        last_update = std::chrono::steady_clock::now();
    }
    
    bool get_latest(std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock(mtx);
        if (frames.empty()) {
            return false;
        }
        frame = frames.back();
        return true;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        while (!frames.empty()) {
            frames.pop();
        }
    }
    
    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return frames.empty();
    }
    
    bool is_stale() {
        std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_update).count();
        return elapsed > 5;
    }
};

// 四画面合成器
class QuadCompositor {
private:
    std::vector<std::vector<uint8_t>> cached_frames;
    std::mutex mtx;
    int output_width;
    int output_height;
    int cell_width;
    int cell_height;
    
    // JPEG解码上下文
    AVCodecContext* decode_ctx = nullptr;
    AVCodecParserContext* parser_ctx = nullptr;
    
    // 合成用的缓冲区
    std::vector<uint8_t> composite_buffer;
    
public:
    QuadCompositor(int width = 1280, int height = 720) 
        : output_width(width), output_height(height),
          cell_width(width / 2), cell_height(height / 2) {
        cached_frames.resize(4);
        
        // 初始化JPEG解码器
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (codec) {
            decode_ctx = avcodec_alloc_context3(codec);
            if (decode_ctx) {
                decode_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
                if (avcodec_open2(decode_ctx, codec, nullptr) >= 0) {
                    parser_ctx = av_parser_init(codec->id);
                }
            }
        }
        
        // 初始化合成缓冲区
        composite_buffer.resize(output_width * output_height * 3);
    }
    
    ~QuadCompositor() {
        if (parser_ctx) av_parser_close(parser_ctx);
        if (decode_ctx) avcodec_free_context(&decode_ctx);
    }
    
    void update_frame(int index, const std::vector<uint8_t>& jpeg_data) {
        if (index < 0 || index >= 4) return;
        
        std::lock_guard<std::mutex> lock(mtx);
        cached_frames[index] = jpeg_data;
    }
    
    // 简化的JPEG到RGB转换（实际应用中应该使用libjpeg或FFmpeg进行解码）
    bool decode_jpeg_to_rgb(const std::vector<uint8_t>& jpeg_data, std::vector<uint8_t>& rgb_data, int& width, int& height) {
        if (jpeg_data.empty() || !decode_ctx || !parser_ctx) {
            return false;
        }
        
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        bool success = false;
        
        // 解码JPEG
        uint8_t* data = const_cast<uint8_t*>(jpeg_data.data());
        int data_size = static_cast<int>(jpeg_data.size());
        
        while (data_size > 0) {
            int ret = av_parser_parse2(parser_ctx, decode_ctx, &pkt->data, &pkt->size,
                                      data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                break;
            }
            
            data += ret;
            data_size -= ret;
            
            if (pkt->size) {
                ret = avcodec_send_packet(decode_ctx, pkt);
                if (ret < 0) {
                    break;
                }
                
                while (ret >= 0) {
                    ret = avcodec_receive_frame(decode_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        break;
                    }
                    
                    if (frame->format == AV_PIX_FMT_YUVJ420P) {
                        // 简化的YUVJ420P到RGB转换
                        width = frame->width;
                        height = frame->height;
                        rgb_data.resize(width * height * 3);
                        
                        // 这里应该使用sws_scale进行正确的转换，但为了简化我们使用近似转换
                        // 实际应用中应该使用: sws_scale(yuv_to_rgb_ctx, ...)
                        for (int y = 0; y < height; y++) {
                            for (int x = 0; x < width; x++) {
                                int Y = frame->data[0][y * frame->linesize[0] + x];
                                int U = frame->data[1][(y/2) * frame->linesize[1] + (x/2)];
                                int V = frame->data[2][(y/2) * frame->linesize[2] + (x/2)];
                                
                                // YUV to RGB conversion (简化版)
                                int R = Y + 1.402 * (V - 128);
                                int G = Y - 0.344 * (U - 128) - 0.714 * (V - 128);
                                int B = Y + 1.772 * (U - 128);
                                
                                int idx = (y * width + x) * 3;
                                rgb_data[idx] = std::clamp(R, 0, 255);
                                rgb_data[idx + 1] = std::clamp(G, 0, 255);
                                rgb_data[idx + 2] = std::clamp(B, 0, 255);
                            }
                        }
                        
                        success = true;
                    }
                    
                    av_frame_unref(frame);
                }
            }
        }
        
        av_packet_free(&pkt);
        av_frame_free(&frame);
        
        return success;
    }
    
    std::vector<uint8_t> compose() {
        std::lock_guard<std::mutex> lock(mtx);
        
        // 将合成缓冲区填充为黑色
        std::fill(composite_buffer.begin(), composite_buffer.end(), 0);
        
        // 检查是否有任何活动帧
        bool has_active_frame = false;
        for (int i = 0; i < 4; i++) {
            if (!cached_frames[i].empty()) {
                has_active_frame = true;
                break;
            }
        }
        
        if (!has_active_frame) {
            return {};
        }
        
        // 对每个画面进行合成
        for (int i = 0; i < 4; i++) {
            if (!cached_frames[i].empty()) {
                std::vector<uint8_t> rgb_data;
                int frame_width, frame_height;
                
                if (decode_jpeg_to_rgb(cached_frames[i], rgb_data, frame_width, frame_height)) {
                    // 计算目标位置
                    int start_x = (i % 2) * cell_width;
                    int start_y = (i / 2) * cell_height;
                    
                    // 缩放并复制到合成缓冲区
                    for (int y = 0; y < cell_height; y++) {
                        for (int x = 0; x < cell_width; x++) {
                            // 计算源图像中的位置（进行缩放）
                            int src_x = (x * frame_width) / cell_width;
                            int src_y = (y * frame_height) / cell_height;
                            
                            if (src_x < frame_width && src_y < frame_height) {
                                int src_idx = (src_y * frame_width + src_x) * 3;
                                int dst_idx = ((start_y + y) * output_width + (start_x + x)) * 3;
                                
                                if (src_idx + 2 < rgb_data.size() && dst_idx + 2 < composite_buffer.size()) {
                                    composite_buffer[dst_idx] = rgb_data[src_idx];
                                    composite_buffer[dst_idx + 1] = rgb_data[src_idx + 1];
                                    composite_buffer[dst_idx + 2] = rgb_data[src_idx + 2];
                                }
                            }
                        }
                    }
                }
            }
        }
        
        return composite_buffer;
    }
};

// RTSP解码器类
class RTSPDecoder {
private:
    std::string rtsp_url;
    std::atomic<bool> running{false};
    std::thread decode_thread;
    int decoder_id;
    
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    int video_stream_idx = -1;
    
    FrameBuffer& frame_buffer;
    JPEGEncoder jpeg_encoder;
    std::mutex decoder_mutex;
    
public:
    RTSPDecoder(int id, const std::string& url, FrameBuffer& buffer)
        : decoder_id(id), rtsp_url(url), frame_buffer(buffer), jpeg_encoder(640, 360) {} // 输出640x360用于合成
    
    ~RTSPDecoder() {
        stop();
    }
    
    bool start() {
        if (running) return false;
        
        std::lock_guard<std::mutex> lock(decoder_mutex);
        
        frame_buffer.clear();
        avformat_network_init();
        
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "5000000", 0);
        
        std::cout << "Decoder " << decoder_id << ": Connecting to RTSP stream: " << rtsp_url << std::endl;
        
        if (avformat_open_input(&fmt_ctx, rtsp_url.c_str(), nullptr, &options) != 0) {
            std::cerr << "Decoder " << decoder_id << ": Could not open RTSP stream" << std::endl;
            av_dict_free(&options);
            return false;
        }
        
        av_dict_free(&options);
        
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            std::cerr << "Decoder " << decoder_id << ": Could not find stream information" << std::endl;
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_idx = i;
                break;
            }
        }
        
        if (video_stream_idx == -1) {
            std::cerr << "Decoder " << decoder_id << ": Could not find video stream" << std::endl;
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            std::cerr << "Decoder " << decoder_id << ": Unsupported codec" << std::endl;
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx, codecpar);
        
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cerr << "Decoder " << decoder_id << ": Could not open codec" << std::endl;
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        std::cout << "Decoder " << decoder_id << ": RTSP stream connected successfully" << std::endl;
        std::cout << "  Video codec: " << avcodec_get_name(codecpar->codec_id) << std::endl;
        std::cout << "  Resolution: " << codecpar->width << "x" << codecpar->height << std::endl;
        
        running = true;
        decode_thread = std::thread(&RTSPDecoder::decode_loop, this);
        
        return true;
    }
    
    void stop() {
        running = false;
        if (decode_thread.joinable()) {
            decode_thread.join();
        }
        
        std::lock_guard<std::mutex> lock(decoder_mutex);
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
            codec_ctx = nullptr;
        }
        if (fmt_ctx) {
            avformat_close_input(&fmt_ctx);
            fmt_ctx = nullptr;
        }
        
        avformat_network_deinit();
        frame_buffer.clear();
    }
    
    bool is_running() const {
        return running;
    }
    
    std::string get_url() const {
        return rtsp_url;
    }
    
    FrameBuffer& get_frame_buffer() {
        return frame_buffer;
    }
    
private:
    void decode_loop() {
        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();
        unsigned long frame_count = 0;
        
        while (running) {
            {
                std::lock_guard<std::mutex> lock(decoder_mutex);
                
                if (!fmt_ctx || !running) {
                    continue;
                }
                
                int ret = av_read_frame(fmt_ctx, pkt);
                if (ret < 0) {
                    if (ret == AVERROR_EOF) {
                        std::cout << "Decoder " << decoder_id << ": End of stream reached, reconnecting..." << std::endl;
                        av_seek_frame(fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
                        av_packet_unref(pkt);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        continue;
                    }
                    
                    if (ret != AVERROR(EAGAIN)) {
                        std::cerr << "Decoder " << decoder_id << ": Error reading frame: " << ret << ", retrying..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    }
                    av_packet_unref(pkt);
                    continue;
                }
                
                if (pkt->stream_index == video_stream_idx) {
                    ret = avcodec_send_packet(codec_ctx, pkt);
                    if (ret < 0) {
                        av_packet_unref(pkt);
                        continue;
                    }
                    
                    while (ret >= 0) {
                        ret = avcodec_receive_frame(codec_ctx, frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        } else if (ret < 0) {
                            break;
                        }
                        
                        // 降低帧率，编码为JPEG并存入缓冲区
                        if (++frame_count % 3 == 0) {
                            auto jpeg_data = jpeg_encoder.encode(frame, 640, 360);
                            if (!jpeg_data.empty()) {
                                frame_buffer.push(jpeg_data);
                            }
                        }
                        
                        av_frame_unref(frame);
                    }
                }
                
                av_packet_unref(pkt);
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }
};

// 四画面解码器管理器
class QuadDecoderManager {
private:
    std::vector<std::unique_ptr<RTSPDecoder>> decoders;
    std::vector<std::unique_ptr<FrameBuffer>> frame_buffers;
    std::unique_ptr<QuadCompositor> compositor;
    std::mutex manager_mutex;
    std::vector<std::string> stream_urls;
    std::atomic<bool> is_streaming{false};
    std::thread compose_thread;
    std::atomic<bool> composing{false};
    FrameBuffer composite_buffer;
    JPEGEncoder composite_encoder;
    
public:
    QuadDecoderManager() 
        : compositor(std::make_unique<QuadCompositor>(1280, 720)),
          composite_encoder(1280, 720) {
        // 初始化四个解码器和缓冲区
        for (int i = 0; i < 4; i++) {
            frame_buffers.push_back(std::make_unique<FrameBuffer>());
            decoders.push_back(nullptr);
            stream_urls.push_back("");
        }
        
        // 启动合成线程
        composing = true;
        compose_thread = std::thread(&QuadDecoderManager::compose_loop, this);
    }
    
    ~QuadDecoderManager() {
        stop_all();
        composing = false;
        if (compose_thread.joinable()) {
            compose_thread.join();
        }
    }
    
    bool start_all_streams(const std::vector<std::string>& urls) {
        if (urls.size() != 4) {
            return false;
        }
        
        std::cout << "Starting all streams..." << std::endl;
        
        // 先停止所有现有的流
        stop_all();
        
        std::lock_guard<std::mutex> lock(manager_mutex);
     
        bool all_success = true;
        stream_urls = urls;
        
        // 启动所有解码器
        for (int i = 0; i < 4; i++) {
            if (!urls[i].empty()) {
                decoders[i] = std::make_unique<RTSPDecoder>(i, urls[i], *frame_buffers[i]);
                if (!decoders[i]->start()) {
                    std::cerr << "Failed to start stream " << i << ": " << urls[i] << std::endl;
                    decoders[i].reset();
                    all_success = false;
                } else {
                    std::cout << "Started stream " << i << ": " << urls[i] << std::endl;
                }
            }
        }
        
        is_streaming = all_success;
        return all_success;
    }
    
    void stop_all() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        for (int i = 0; i < 4; i++) {
            if (decoders[i]) {
                decoders[i]->stop();
                decoders[i].reset();
                frame_buffers[i]->clear();
            }
        }
        is_streaming = false;
        std::cout << "All streams stopped" << std::endl;
    }
    
    std::vector<uint8_t> get_composite_frame() {
        std::vector<uint8_t> frame;
        composite_buffer.get_latest(frame);
        return frame;
    }
    
    std::vector<std::string> get_stream_urls() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return stream_urls;
    }
    
    bool get_is_streaming() {
        return is_streaming;
    }
    
private:
    void compose_loop() {
        while (composing) {
            std::vector<std::vector<uint8_t>> latest_frames(4);
            
            {
                std::lock_guard<std::mutex> lock(manager_mutex);
                
                // 获取每个缓冲区的最新帧
                for (int i = 0; i < 4; i++) {
                    if (frame_buffers[i] && !frame_buffers[i]->empty()) {
                        frame_buffers[i]->get_latest(latest_frames[i]);
                    }
                }
            }
            
            // 更新合成器
            for (int i = 0; i < 4; i++) {
                if (!latest_frames[i].empty()) {
                    compositor->update_frame(i, latest_frames[i]);
                }
            }
            
            // 合成四画面为RGB
            auto rgb_data = compositor->compose();
            if (!rgb_data.empty()) {
                // 将RGB编码为JPEG
                AVFrame* rgb_frame = av_frame_alloc();
                rgb_frame->width = 1280;
                rgb_frame->height = 720;
                rgb_frame->format = AV_PIX_FMT_RGB24;
                
                if (av_frame_get_buffer(rgb_frame, 32) >= 0) {
                    // 拷贝RGB数据
                    for (int y = 0; y < 720; y++) {
                        memcpy(rgb_frame->data[0] + y * rgb_frame->linesize[0],
                               rgb_data.data() + y * 1280 * 3,
                               1280 * 3);
                    }
                    
                    // 编码为JPEG
                    auto jpeg_data = composite_encoder.encode(rgb_frame);
                    if (!jpeg_data.empty()) {
                        composite_buffer.push(jpeg_data);
                    }
                }
                
                av_frame_free(&rgb_frame);
            } else {
                // 如果没有画面，等待一下
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10fps
        }
    }
};

// HTML页面内容保持不变...
const std::string HTML_PAGE = R"====(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>四画面RTSP视频监控</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        
        body {
            font-family: "Microsoft YaHei", "Segoe UI", sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
            color: #e6e6e6;
        }
        
        .container {
            background: rgba(30, 30, 46, 0.95);
            border-radius: 20px;
            box-shadow: 0 20px 40px rgba(0, 0, 0, 0.3);
            overflow: hidden;
            width: 100%;
            max-width: 1400px;
            animation: fadeIn 0.5s ease;
            border: 1px solid rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(10px);
        }
        
        @keyframes fadeIn {
            from { opacity: 0; transform: translateY(20px); }
            to { opacity: 1; transform: translateY(0); }
        }
        
        .header {
            background: linear-gradient(135deg, #0f3460 0%, #1a1a2e 100%);
            color: white;
            padding: 30px;
            text-align: center;
            position: relative;
            overflow: hidden;
            border-bottom: 1px solid rgba(255, 255, 255, 0.1);
        }
        
        .header h1 {
            font-size: 32px;
            margin-bottom: 10px;
            font-weight: 600;
            position: relative;
            z-index: 1;
            color: #4cc9f0;
            text-shadow: 0 0 10px rgba(76, 201, 240, 0.5);
        }
        
        .header p {
            opacity: 0.9;
            font-size: 16px;
            position: relative;
            z-index: 1;
            color: #a9b7c6;
        }
        
        .content {
            padding: 30px;
        }
        
        .control-panel {
            background: rgba(40, 40, 60, 0.8);
            padding: 25px;
            border-radius: 15px;
            border: 1px solid rgba(255, 255, 255, 0.1);
            box-shadow: 0 5px 15px rgba(0, 0, 0, 0.2);
            margin-bottom: 30px;
        }
        
        .control-panel h2 {
            color: #72efdd;
            margin-bottom: 25px;
            font-size: 24px;
            display: flex;
            align-items: center;
            gap: 10px;
            border-bottom: 2px solid rgba(114, 239, 221, 0.3);
            padding-bottom: 10px;
        }
        
        .stream-inputs {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
            margin-bottom: 30px;
        }
        
        @media (max-width: 768px) {
            .stream-inputs {
                grid-template-columns: 1fr;
            }
        }
        
        .stream-input {
            background: rgba(50, 50, 70, 0.8);
            padding: 20px;
            border-radius: 12px;
            border: 1px solid rgba(255, 255, 255, 0.05);
        }
        
        .stream-input h3 {
            color: #ff9e00;
            margin-bottom: 15px;
            font-size: 18px;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .rtsp-input {
            width: 100%;
            padding: 12px 15px;
            background: rgba(30, 30, 40, 0.8);
            border: 2px solid rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            font-size: 14px;
            color: #e6e6e6;
            transition: all 0.3s ease;
        }
        
        .rtsp-input:focus {
            outline: none;
            border-color: #4cc9f0;
            box-shadow: 0 0 0 3px rgba(76, 201, 240, 0.1);
            background: rgba(30, 30, 40, 0.9);
        }
        
        .rtsp-input::placeholder {
            color: #666677;
        }
        
        .global-controls {
            display: flex;
            gap: 20px;
            justify-content: center;
            flex-wrap: wrap;
        }
        
        .btn {
            padding: 15px 30px;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: 500;
            cursor: pointer;
            transition: all 0.3s ease;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 10px;
            min-width: 200px;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.2);
            background: linear-gradient(135deg, #4361ee 0%, #3a0ca3 100%);
            color: white;
        }
        
        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 12px rgba(0, 0, 0, 0.3);
            filter: brightness(1.1);
        }
        
        .btn-start {
            background: linear-gradient(135deg, #4cc9f0 0%, #4361ee 100%);
        }
        
        .btn-stop {
            background: linear-gradient(135deg, #f72585 0%, #b5179e 100%);
        }
        
        .video-section {
            margin-top: 20px;
        }
        
        .video-container {
            width: 100%;
            background: #000;
            border-radius: 12px;
            overflow: hidden;
            box-shadow: 0 10px 30px rgba(0, 0, 0, 0.4);
            position: relative;
            aspect-ratio: 16/9;
            max-height: 720px;
        }
        
        #video {
            width: 100%;
            height: 100%;
            display: block;
            transition: opacity 0.3s ease;
            object-fit: contain;
        }
        
        .video-placeholder {
            width: 100%;
            height: 100%;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            color: white;
            font-size: 18px;
            background: linear-gradient(135deg, #1e3c72 0%, #2a5298 100%);
            text-align: center;
        }
        
        .video-placeholder i {
            font-size: 64px;
            margin-bottom: 20px;
            opacity: 0.8;
            color: #4cc9f0;
        }
        
        /* 添加网格样式 */
        .grid-overlay {
            position: absolute;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            display: grid;
            grid-template-columns: 1fr 1fr;
            grid-template-rows: 1fr 1fr;
            pointer-events: none;
            z-index: 10;
        }
        
        .grid-cell {
            border: 2px solid rgba(255, 255, 255, 0.3);
            position: relative;
        }
        
        .grid-label {
            position: absolute;
            top: 10px;
            left: 10px;
            background: rgba(0, 0, 0, 0.7);
            color: white;
            padding: 5px 10px;
            border-radius: 5px;
            font-size: 12px;
            font-weight: bold;
            z-index: 20;
            pointer-events: auto;
        }
        
        .status-container {
            margin-top: 20px;
            padding: 20px;
            border-radius: 12px;
            background: rgba(40, 40, 60, 0.8);
            border: 1px solid rgba(255, 255, 255, 0.1);
            box-shadow: 0 5px 15px rgba(0, 0, 0, 0.2);
        }
        
        .status {
            font-size: 16px;
            color: #a9b7c6;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 12px;
            margin-bottom: 15px;
        }
        
        .status-indicator {
            width: 14px;
            height: 14px;
            border-radius: 50%;
            background: #6c757d;
            animation: pulse 2s infinite;
            box-shadow: 0 0 10px rgba(108, 117, 125, 0.3);
        }
        
        .status-indicator.active {
            background: #28a745;
            box-shadow: 0 0 15px rgba(40, 167, 69, 0.5);
        }
        
        @keyframes pulse {
            0%, 100% { transform: scale(1); }
            50% { transform: scale(1.1); }
        }
        
        .streams-info {
            margin-top: 20px;
        }
        
        .stream-info {
            background: rgba(30, 30, 40, 0.8);
            padding: 15px;
            border-radius: 8px;
            border-left: 4px solid #4cc9f0;
            font-size: 14px;
            margin-bottom: 10px;
        }
        
        .stream-info h4 {
            color: #ff9e00;
            margin-bottom: 5px;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        
        .stream-url {
            word-break: break-all;
            color: #a9b7c6;
            font-size: 12px;
            margin-top: 5px;
            padding: 5px;
            background: rgba(0, 0, 0, 0.3);
            border-radius: 4px;
        }
        
        .loading-overlay {
            display: none;
            position: absolute;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background: rgba(0, 0, 0, 0.85);
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            color: white;
            font-size: 18px;
            z-index: 100;
            backdrop-filter: blur(5px);
        }
        
        .spinner {
            width: 50px;
            height: 50px;
            border: 4px solid rgba(255, 255, 255, 0.3);
            border-radius: 50%;
            border-top-color: white;
            animation: spin 1s ease-in-out infinite;
            margin-bottom: 20px;
        }
        
        @keyframes spin {
            to { transform: rotate(360deg); }
        }
        
        canvas {
            display: none;
        }
        
        @media (max-width: 768px) {
            .container {
                border-radius: 15px;
            }
            
            .header {
                padding: 20px;
            }
            
            .header h1 {
                font-size: 24px;
            }
            
            .content {
                padding: 20px;
            }
            
            .global-controls {
                flex-direction: column;
            }
            
            .global-controls .btn {
                width: 100%;
            }
            
            .video-container {
                aspect-ratio: 4/3;
            }
        }
    </style>
    <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css">
</head>
<body>
    <div class="container">
        <div class="header">
            <h1><i class="fas fa-th-large"></i> 四画面RTSP视频监控</h1>
            <p>同时播放四个RTSP视频流 - 四画面合成显示</p>
        </div>
        
        <div class="content">
            <div class="control-panel">
                <h2><i class="fas fa-sliders-h"></i> 控制面板</h2>
                
                <div class="stream-inputs">
                    <!-- 画面1 - 左上 -->
                    <div class="stream-input">
                        <h3><i class="fas fa-video"></i> 画面 1 (左上)</h3>
                        <input type="text" 
                               id="rtspUrl1" 
                               class="rtsp-input" 
                               placeholder="rtsp://username:password@192.168.1.100:554/stream1"
                               value="">
                    </div>
                    
                    <!-- 画面2 - 右上 -->
                    <div class="stream-input">
                        <h3><i class="fas fa-video"></i> 画面 2 (右上)</h3>
                        <input type="text" 
                               id="rtspUrl2" 
                               class="rtsp-input" 
                               placeholder="rtsp://username:password@192.168.1.101:554/stream1"
                               value="">
                    </div>
                    
                    <!-- 画面3 - 左下 -->
                    <div class="stream-input">
                        <h3><i class="fas fa-video"></i> 画面 3 (左下)</h3>
                        <input type="text" 
                               id="rtspUrl3" 
                               class="rtsp-input" 
                               placeholder="rtsp://username:password@192.168.1.102:554/stream1"
                               value="">
                    </div>
                    
                    <!-- 画面4 - 右下 -->
                    <div class="stream-input">
                        <h3><i class="fas fa-video"></i> 画面 4 (右下)</h3>
                        <input type="text" 
                               id="rtspUrl4" 
                               class="rtsp-input" 
                               placeholder="rtsp://username:password@192.168.1.103:554/stream1"
                               value="">
                    </div>
                </div>
                
                <div class="global-controls">
                    <button id="startAllBtn" class="btn btn-start" onclick="startAllStreams()">
                        <i class="fas fa-play-circle"></i> 启动所有画面
                    </button>
                    <button id="stopAllBtn" class="btn btn-stop" onclick="stopAllStreams()">
                        <i class="fas fa-stop-circle"></i> 停止所有画面
                    </button>
                </div>
            </div>
            
            <div class="video-section">
                <div class="video-container">
                    <div id="loading" class="loading-overlay">
                        <div class="spinner"></div>
                        <div>正在连接视频流...</div>
                    </div>
                    <div id="videoPlaceholder" class="video-placeholder">
                        <i class="fas fa-th-large"></i>
                        <div>
                            <h3>等待视频流</h3>
                            <p>请输入四个RTSP地址并点击"启动所有画面"</p>
                        </div>
                    </div>
                    <img id="video" src="" style="display: none;">
                    <!-- 添加网格覆盖层 -->
                    <div class="grid-overlay">
                        <div class="grid-cell">
                            <div class="grid-label">画面 1 (左上)</div>
                        </div>
                        <div class="grid-cell">
                            <div class="grid-label">画面 2 (右上)</div>
                        </div>
                        <div class="grid-cell">
                            <div class="grid-label">画面 3 (左下)</div>
                        </div>
                        <div class="grid-cell">
                            <div class="grid-label">画面 4 (右下)</div>
                        </div>
                    </div>
                </div>
                
                <div class="status-container">
                    <div class="status">
                        <div id="statusIndicator" class="status-indicator"></div>
                        <span id="statusText">准备就绪，请输入四个RTSP地址</span>
                    </div>
                    
                    <div class="streams-info" id="streamsInfo">
                        <!-- 动态显示流状态 -->
                    </div>
                </div>
            </div>
        </div>
    </div>
    
    <canvas id="canvas" style="display:none;"></canvas>
    <a id="download" style="display:none;"></a>
    
    <script>
        let streamInterval = null;
        let isStreaming = false;
        
        function updateStatus(status, isActive = false) {
            const statusText = document.getElementById('statusText');
            const statusIndicator = document.getElementById('statusIndicator');
            
            statusText.textContent = status;
            statusIndicator.className = 'status-indicator' + (isActive ? ' active' : '');
        }
        
        function updateStreamsInfo(streams) {
            const streamsInfo = document.getElementById('streamsInfo');
            streamsInfo.innerHTML = '';
            
            for (let i = 0; i < streams.length; i++) {
                if (streams[i]) {
                    const streamInfo = document.createElement('div');
                    streamInfo.className = 'stream-info';
                    streamInfo.innerHTML = `
                        <h4><i class="fas fa-video"></i> 画面 ${i + 1} (${getPositionText(i)})</h4>
                        <div>状态: <span style="color: #4ade80">已连接</span></div>
                        <div class="stream-url">${streams[i]}</div>
                    `;
                    streamsInfo.appendChild(streamInfo);
                }
            }
        }
        
        function getPositionText(index) {
            switch(index) {
                case 0: return '左上';
                case 1: return '右上';
                case 2: return '左下';
                case 3: return '右下';
                default: return '';
            }
        }
        
        function showLoading(show) {
            const loading = document.getElementById('loading');
            const video = document.getElementById('video');
            const videoPlaceholder = document.getElementById('videoPlaceholder');
            
            if (show) {
                loading.style.display = 'flex';
                video.style.display = 'none';
                videoPlaceholder.style.display = 'none';
            } else {
                loading.style.display = 'none';
            }
        }
        
        function startAllStreams() {
            const urls = [];
            for (let i = 1; i <= 4; i++) {
                urls.push(document.getElementById('rtspUrl' + i).value.trim());
            }
            
            // 检查是否至少有一个URL
            let hasUrl = false;
            for (const url of urls) {
                if (url) {
                    hasUrl = true;
                    break;
                }
            }
            
            if (!hasUrl) {
                alert('请至少输入一个RTSP地址');
                return;
            }
            
            showLoading(true);
            updateStatus('正在连接所有画面...', true);
            
            fetch('/start_all', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded',
                },
                body: urls.map((url, index) => `rtsp_url${index + 1}=${encodeURIComponent(url)}`).join('&')
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    if (!isStreaming) {
                        isStreaming = true;
                        const video = document.getElementById('video');
                        const videoPlaceholder = document.getElementById('videoPlaceholder');
                        
                        video.style.display = 'block';
                        videoPlaceholder.style.display = 'none';
                        startVideoStream();
                    }
                    
                    updateStatus('四画面监控运行中', true);
                    updateStreamsInfo(urls);
                    showLoading(false);
                } else {
                    updateStatus('连接失败: ' + data.error);
                    showLoading(false);
                    alert('无法连接到RTSP流: ' + data.error);
                }
            })
            .catch(error => {
                updateStatus('连接失败');
                showLoading(false);
                alert('请求失败: ' + error.message);
                console.error('Error:', error);
            });
        }
        
        function stopAllStreams() {
            if (!isStreaming) {
                alert('当前没有正在播放的视频流');
                return;
            }
            
            if (confirm('确定要停止所有画面吗？')) {
                fetch('/stop_all', {
                    method: 'POST'
                })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        stopVideoStream();
                        updateStatus('已停止所有画面', false);
                        document.getElementById('streamsInfo').innerHTML = '';
                    }
                })
                .catch(error => {
                    console.error('停止所有流时出错:', error);
                });
            }
        }
        
        function stopVideoStream() {
            if (streamInterval) {
                clearInterval(streamInterval);
                streamInterval = null;
            }
            
            isStreaming = false;
            const video = document.getElementById('video');
            const videoPlaceholder = document.getElementById('videoPlaceholder');
            
            video.style.display = 'none';
            video.src = '';
            videoPlaceholder.style.display = 'flex';
        }
        
        function startVideoStream() {
            if (streamInterval) {
                clearInterval(streamInterval);
            }
            
            updateVideoFrame();
            streamInterval = setInterval(updateVideoFrame, 100);
        }
        
        function updateVideoFrame() {
            if (!isStreaming) return;
            
            const video = document.getElementById('video');
            const timestamp = new Date().getTime();
            video.src = '/video_frame?t=' + timestamp;
        }
        
        // 输入框回车事件
        for (let i = 1; i <= 4; i++) {
            document.getElementById('rtspUrl' + i).addEventListener('keypress', function(e) {
                if (e.key === 'Enter') {
                    startAllStreams();
                }
            });
        }
        
        // 视频加载错误处理
        document.getElementById('video').addEventListener('error', function(e) {
            console.error('视频加载错误:', e);
            if (isStreaming) {
                updateStatus('视频流加载失败，正在重试...', true);
                setTimeout(updateVideoFrame, 1000);
            }
        });
        
        // 页面加载时检查当前状态
        window.onload = function() {
            fetch('/stream_status')
                .then(response => response.json())
                .then(data => {
                    if (data.is_streaming && data.streams) {
                        const streams = data.streams;
                        for (let i = 0; i < Math.min(streams.length, 4); i++) {
                            if (streams[i]) {
                                document.getElementById('rtspUrl' + (i + 1)).value = streams[i];
                            }
                        }
                        
                        if (data.is_streaming) {
                            startVideoStream();
                            isStreaming = true;
                            const video = document.getElementById('video');
                            const videoPlaceholder = document.getElementById('videoPlaceholder');
                            
                            video.style.display = 'block';
                            videoPlaceholder.style.display = 'none';
                            
                            updateStatus('四画面监控运行中', true);
                            updateStreamsInfo(streams);
                        }
                    }
                })
                .catch(error => {
                    console.error('检查流状态时出错:', error);
                });
        };
        
        // 示例URL填充（便于测试）
        function fillExampleUrls() {
            const examples = [
                'rtsp://admin:admin123@192.168.1.100:554/stream1',
                'rtsp://admin:admin123@192.168.1.101:554/stream1',
                'rtsp://admin:admin123@192.168.1.102:554/stream1',
                'rtsp://admin:admin123@192.168.1.103:554/stream1'
            ];
            
            for (let i = 0; i < 4; i++) {
                document.getElementById('rtspUrl' + (i + 1)).value = examples[i];
            }
        }
        
        // 取消注释以下行以启用示例URL自动填充
        // fillExampleUrls();
    </script>
</body>
</html>
)====";

// 空白JPEG图像（用于无视频时显示）
const uint8_t BLANK_JPEG[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x01, 0x00,
    0x01, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10,
    0x00, 0x00, 0x01, 0x3f, 0x00, 0xff, 0xd9
};

int main(int argc, char* argv[]) {
    int port = 38080;
    
    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }
    
    avformat_network_init();
    
    QuadDecoderManager decoder_manager;
    
    std::cout << "==========================================" << std::endl;
    std::cout << "    四画面RTSP视频监控服务器 v2.0" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "服务器端口: " << port << std::endl;
    std::cout << "访问地址: http://localhost:" << port << std::endl;
    std::cout << std::endl;
    std::cout << "支持功能:" << std::endl;
    std::cout << "  √ 同时显示四个RTSP视频流" << std::endl;
    std::cout << "  √ 四画面合成显示（左上、左下、右上、右下）" << std::endl;
    std::cout << "  √ 统一控制：全部启动/全部停止" << std::endl;
    std::cout << "  √ 实时合成，网格布局" << std::endl;
    std::cout << "  √ 支持1280x720输入流，合成1280x720输出" << std::endl;
    std::cout << std::endl;
    std::cout << "HTTP接口:" << std::endl;
    std::cout << "  GET  /              - 四画面Web界面" << std::endl;
    std::cout << "  GET  /video_frame   - 获取四画面合成帧" << std::endl;
    std::cout << "  POST /start_all     - 启动所有画面" << std::endl;
    std::cout << "  POST /stop_all      - 停止所有画面" << std::endl;
    std::cout << "  GET  /stream_status - 获取所有画面状态" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << std::endl;
    
    httplib::Server server;
    
    // 主页
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(HTML_PAGE, "text/html");
    });
    
    // 获取四画面合成视频帧
    server.Get("/video_frame", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        auto frame = decoder_manager.get_composite_frame();
        if (!frame.empty()) {
            res.set_content(reinterpret_cast<char*>(frame.data()), frame.size(), "image/jpeg");
        } else {
            res.set_content(reinterpret_cast<const char*>(BLANK_JPEG), sizeof(BLANK_JPEG), "image/jpeg");
        }
    });
    
    // 启动所有画面
    server.Post("/start_all", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        std::vector<std::string> urls(4);
        
        for (int i = 0; i < 4; i++) {
            urls[i] = req.get_param_value("rtsp_url" + std::to_string(i + 1));
        }
        
        std::cout << "Received start_all request with URLs:" << std::endl;
        for (int i = 0; i < 4; i++) {
            std::cout << "  Stream " << (i + 1) << " (" << (i == 0 ? "左上" : i == 1 ? "右上" : i == 2 ? "左下" : "右下") 
                      << "): " << urls[i] << std::endl;
        }

        // 检查是否至少有一个URL
        bool has_url = false;
        for (const auto& url : urls) {
            if (!url.empty()) {
                has_url = true;
                break;
            }
        }
        
        if (!has_url) {
            res.set_content(create_json_response(false, "At least one RTSP URL is required"), "application/json");
            return;
        }
        
        if (decoder_manager.start_all_streams(urls)) {
            res.set_content(create_json_response(true), "application/json");
        } else {
            res.set_content(create_json_response(false, "Failed to connect to one or more RTSP streams"), "application/json");
        }
    });
    
    // 停止所有画面
    server.Post("/stop_all", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        decoder_manager.stop_all();
        res.set_content(create_json_response(true), "application/json");
    });
    
    // 获取所有画面状态
    server.Get("/stream_status", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        auto urls = decoder_manager.get_stream_urls();
        res.set_content(create_status_json(decoder_manager.get_is_streaming(), urls), "application/json");
    });
    
    server.set_keep_alive_max_count(100);
    server.set_read_timeout(10, 0);
    server.set_write_timeout(10, 0);
    server.set_exception_handler([](const auto& req, auto& res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (std::exception &e) {
            res.set_content(create_json_response(false, std::string("Server error: ") + e.what()), "application/json");
        } catch (...) {
            res.set_content(create_json_response(false, "Unknown server error"), "application/json");
        }
    });
    
    std::cout << "服务器启动中..." << std::endl;
    
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "错误: 无法在端口 " << port << " 启动服务器" << std::endl;
        return 1;
    }
    
    std::cout << "服务器已启动，按 Ctrl+C 停止" << std::endl;
    
    return 0;
}