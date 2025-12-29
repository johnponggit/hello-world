#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <vector>
#include <memory>
#include <sstream>
#include <iomanip>
#include <map>
#include <cmath>
#include <condition_variable>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
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

std::string create_status_json(bool is_streaming, const std::string& current_url1 = "", 
                               const std::string& current_url2 = "") {
    std::stringstream ss;
    ss << "{";
    ss << "\"is_streaming\":" << (is_streaming ? "true" : "false");
    if (!current_url1.empty()) {
        ss << ",\"current_url1\":\"" << current_url1 << "\"";
    }
    if (!current_url2.empty()) {
        ss << ",\"current_url2\":\"" << current_url2 << "\"";
    }
    ss << "}";
    return ss.str();
}

// YUV帧结构
struct YUVFrame {
    int width = 0;
    int height = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    std::vector<uint8_t> y_plane;
    std::vector<uint8_t> u_plane;
    std::vector<uint8_t> v_plane;
    int y_stride = 0;
    int uv_stride = 0;
    int64_t pts = 0; // 时间戳
    int64_t timestamp = 0; // 系统时间戳
    
    YUVFrame() = default;
    
    YUVFrame(AVFrame* frame) {
        if (!frame) return;
        
        width = frame->width;
        height = frame->height;
        format = (AVPixelFormat)frame->format;
        pts = frame->pts;
        timestamp = av_gettime() / 1000; // 毫秒
        
        // 计算步长
        y_stride = frame->linesize[0];
        uv_stride = frame->linesize[1];
        
        // 复制Y平面
        size_t y_size = y_stride * height;
        y_plane.resize(y_size);
        memcpy(y_plane.data(), frame->data[0], y_size);
        
        // 复制UV平面（对于YUV420P，UV平面高度是Y平面的一半）
        size_t uv_height = height / 2;
        size_t u_size = uv_stride * uv_height;
        size_t v_size = uv_stride * uv_height;
        
        u_plane.resize(u_size);
        v_plane.resize(v_size);
        
        memcpy(u_plane.data(), frame->data[1], u_size);
        memcpy(v_plane.data(), frame->data[2], v_size);
    }
    
    // 转换为AVFrame（用于编码）
    AVFrame* to_avframe() {
        AVFrame* frame = av_frame_alloc();
        if (!frame) return nullptr;
        
        frame->width = width;
        frame->height = height;
        frame->format = format;
        frame->pts = pts;
        
        // 分配缓冲区
        if (av_frame_get_buffer(frame, 32) < 0) {
            av_frame_free(&frame);
            return nullptr;
        }
        
        // 复制数据
        memcpy(frame->data[0], y_plane.data(), y_plane.size());
        memcpy(frame->data[1], u_plane.data(), u_plane.size());
        memcpy(frame->data[2], v_plane.data(), v_plane.size());
        
        return frame;
    }
    
    bool empty() const {
        return width == 0 || height == 0 || y_plane.empty();
    }
};

// JPEG编码器类
class JPEGEncoder {
private:
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    
public:
    JPEGEncoder(int width = 640, int height = 480) {
        // 初始化JPEG编码器
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) {
            throw std::runtime_error("JPEG encoder not found");
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
        codec_ctx->qmin = 2;
        codec_ctx->qmax = 31;
        
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            throw std::runtime_error("Could not open encoder");
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
    
    // 编码YUVFrame为JPEG
    std::vector<uint8_t> encode(YUVFrame& yuv_frame) {
        std::vector<uint8_t> jpeg_data;
        
        if (!codec_ctx || yuv_frame.empty()) {
            return jpeg_data;
        }
        
        // 转换像素格式
        if (!sws_ctx) {
            sws_ctx = sws_getContext(
                yuv_frame.width, yuv_frame.height,
                AV_PIX_FMT_YUV420P,
                codec_ctx->width, codec_ctx->height,
                codec_ctx->pix_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
        }
        
        // 准备输入帧
        AVFrame* input_frame = yuv_frame.to_avframe();
        if (!input_frame) {
            return jpeg_data;
        }
        
        // 准备输出帧
        frame->width = codec_ctx->width;
        frame->height = codec_ctx->height;
        frame->format = codec_ctx->pix_fmt;
        frame->pts = input_frame->pts;
        
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&input_frame);
            return jpeg_data;
        }
        
        // 转换像素格式
        sws_scale(sws_ctx, input_frame->data, input_frame->linesize,
                  0, input_frame->height,
                  frame->data, frame->linesize);
        
        // 编码为JPEG
        int ret = avcodec_send_frame(codec_ctx, frame);
        if (ret < 0) {
            av_frame_free(&input_frame);
            return jpeg_data;
        }
        
        ret = avcodec_receive_packet(codec_ctx, pkt);
        if (ret == 0) {
            jpeg_data.assign(pkt->data, pkt->data + pkt->size);
        }
        
        av_frame_unref(frame);
        av_packet_unref(pkt);
        av_frame_free(&input_frame);
        
        return jpeg_data;
    }
    
    // 编码AVFrame为JPEG
    std::vector<uint8_t> encode(AVFrame* input_frame) {
        std::vector<uint8_t> jpeg_data;
        
        if (!codec_ctx || !input_frame) {
            return jpeg_data;
        }
        
        // 转换像素格式
        if (!sws_ctx) {
            sws_ctx = sws_getContext(
                input_frame->width, input_frame->height,
                (AVPixelFormat)input_frame->format,
                codec_ctx->width, codec_ctx->height,
                codec_ctx->pix_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
        }
        
        // 准备输出帧
        frame->width = codec_ctx->width;
        frame->height = codec_ctx->height;
        frame->format = codec_ctx->pix_fmt;
        frame->pts = input_frame->pts;
        
        if (av_frame_get_buffer(frame, 0) < 0) {
            return jpeg_data;
        }
        
        // 转换像素格式
        sws_scale(sws_ctx, input_frame->data, input_frame->linesize,
                  0, input_frame->height,
                  frame->data, frame->linesize);
        
        // 编码为JPEG
        int ret = avcodec_send_frame(codec_ctx, frame);
        if (ret < 0) {
            return jpeg_data;
        }
        
        ret = avcodec_receive_packet(codec_ctx, pkt);
        if (ret == 0) {
            jpeg_data.assign(pkt->data, pkt->data + pkt->size);
        }
        
        av_frame_unref(frame);
        av_packet_unref(pkt);
        
        return jpeg_data;
    }
};

// YUV帧缓冲区
class YUVFrameBuffer {
private:
    std::queue<YUVFrame> frames;
    std::mutex mtx;
    std::condition_variable cv;
    size_t max_size = 2;  // 减小缓冲区大小，降低延迟
    std::atomic<bool> has_new_frame{false};
    
public:
    void push(const YUVFrame& frame) {
        std::unique_lock<std::mutex> lock(mtx);
        
        // 如果缓冲区已满，丢弃最旧的帧
        if (frames.size() >= max_size) {
            frames.pop();
        }
        
        frames.push(frame);
        has_new_frame = true;
        cv.notify_one();  // 通知等待的线程
    }
    
    bool get_latest(YUVFrame& frame, int timeout_ms = 50) {
        std::unique_lock<std::mutex> lock(mtx);
        
        // 等待新帧到来
        if (frames.empty()) {
            if (timeout_ms > 0) {
                // 等待指定的时间
                auto status = cv.wait_for(lock, std::chrono::milliseconds(timeout_ms));
                if (status == std::cv_status::timeout) {
                    return false;  // 超时返回
                }
            } else {
                return false;
            }
        }
        
        // 获取最新帧
        frame = frames.back();
        
        // 清空旧帧，只保留最新的一帧
        while (frames.size() > 1) {
            frames.pop();
        }
        
        has_new_frame = false;
        return true;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        while (!frames.empty()) {
            frames.pop();
        }
        has_new_frame = false;
        cv.notify_all();  // 通知所有等待的线程
    }
    
    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return frames.empty();
    }
    
    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return frames.size();
    }
};

// RTSP解码器类
class RTSPDecoder {
private:
    std::string rtsp_url;
    std::atomic<bool> running{false};
    std::thread decode_thread;
    
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    int video_stream_idx = -1;
    
    YUVFrameBuffer& frame_buffer;
    std::mutex decoder_mutex;
    
    // 重连相关变量
    int consecutive_eof_count = 0;
    const int MAX_CONSECUTIVE_EOF = 5;
    int64_t last_reconnect_time = 0;
    const int64_t RECONNECT_INTERVAL = 5000; // 5秒
    
public:
    RTSPDecoder(const std::string& url, YUVFrameBuffer& buffer)
        : rtsp_url(url), frame_buffer(buffer) {}
    
    ~RTSPDecoder() {
        stop();
    }
    
    bool start() {
        if (running) return false;
        
        std::lock_guard<std::mutex> lock(decoder_mutex);
        
        // 清理之前的缓冲区
        frame_buffer.clear();
        
        // 初始化FFmpeg
        avformat_network_init();
        
        // 打开RTSP流
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "5000000", 0); // 5秒超时
        
        std::cout << "Connecting to RTSP stream: " << rtsp_url << std::endl;
        
        if (avformat_open_input(&fmt_ctx, rtsp_url.c_str(), nullptr, &options) != 0) {
            std::cerr << "Could not open RTSP stream: " << rtsp_url << std::endl;
            return false;
        }
        
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            std::cerr << "Could not find stream information" << std::endl;
            return false;
        }
        
        // 查找视频流
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_idx = i;
                break;
            }
        }
        
        if (video_stream_idx == -1) {
            std::cerr << "Could not find video stream" << std::endl;
            return false;
        }
        
        // 获取解码器
        AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            std::cerr << "Unsupported codec" << std::endl;
            return false;
        }
        
        codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx, codecpar);
        
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cerr << "Could not open codec" << std::endl;
            return false;
        }
        
        std::cout << "RTSP stream connected successfully" << std::endl;
        std::cout << "Video codec: " << avcodec_get_name(codecpar->codec_id) << std::endl;
        std::cout << "Resolution: " << codecpar->width << "x" << codecpar->height << std::endl;
        std::cout << "Pixel format: " << av_get_pix_fmt_name((AVPixelFormat)codecpar->format) << std::endl;
        
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
    }
    
    bool is_running() const {
        return running;
    }
    
    std::string get_url() const {
        return rtsp_url;
    }
    
private:
    void decode_loop() {
        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();
        SwsContext* sws_ctx = nullptr;
        int64_t last_frame_time = 0;
        const int64_t min_frame_interval = 40; // 最小帧间隔 40ms (~25fps)
        int frame_count = 0;
        int consecutive_eof_count = 0;  // 连续EOF计数
        const int MAX_CONSECUTIVE_EOF = 5; // 最大连续EOF次数
        int64_t last_reconnect_time = 0;
        const int64_t RECONNECT_INTERVAL = 5000; // 重连间隔 5秒

        while (running) {
            std::unique_lock<std::mutex> lock(decoder_mutex);
            
            if (!fmt_ctx) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            int ret = av_read_frame(fmt_ctx, pkt);
            if (ret < 0) {
                // 处理各种错误情况
                if (ret == AVERROR_EOF) {
                    consecutive_eof_count++;
                    
                    // 如果连续EOF次数过多，尝试重新连接
                    if (consecutive_eof_count >= MAX_CONSECUTIVE_EOF) {
                        int64_t current_time = av_gettime() / 1000;
                        if (current_time - last_reconnect_time >= RECONNECT_INTERVAL) {
                            std::cout << "Too many EOFs, attempting to reconnect RTSP stream: " << rtsp_url << std::endl;
                            
                            // 保存当前设置
                            int video_idx = video_stream_idx;
                            AVCodecParameters* saved_params = nullptr;
                            if (codec_ctx) {
                                saved_params = avcodec_parameters_alloc();
                                avcodec_parameters_from_context(saved_params, codec_ctx);
                            }
                            
                            // 关闭当前连接
                            if (codec_ctx) {
                                avcodec_free_context(&codec_ctx);
                                codec_ctx = nullptr;
                            }
                            if (fmt_ctx) {
                                avformat_close_input(&fmt_ctx);
                                fmt_ctx = nullptr;
                            }
                            
                            // 清空缓冲区
                            frame_buffer.clear();
                            
                            // 重新连接
                            AVDictionary* options = nullptr;
                            av_dict_set(&options, "rtsp_transport", "tcp", 0);
                            av_dict_set(&options, "stimeout", "5000000", 0); // 5秒超时
                            av_dict_set(&options, "reconnect", "1", 0); // 启用重连
                            av_dict_set(&options, "reconnect_at_eof", "1", 0); // EOF时重连
                            av_dict_set(&options, "reconnect_streamed", "1", 0); // 流式重连
                            
                            std::cout << "Reconnecting to RTSP stream..." << std::endl;
                            ret = avformat_open_input(&fmt_ctx, rtsp_url.c_str(), nullptr, &options);
                            av_dict_free(&options);
                            
                            if (ret == 0) {
                                ret = avformat_find_stream_info(fmt_ctx, nullptr);
                                if (ret >= 0) {
                                    // 重新查找视频流
                                    video_stream_idx = -1;
                                    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                                        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                                            video_stream_idx = i;
                                            break;
                                        }
                                    }
                                    
                                    if (video_stream_idx != -1) {
                                        // 重新创建解码器上下文
                                        AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
                                        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
                                        if (codec) {
                                            codec_ctx = avcodec_alloc_context3(codec);
                                            if (codec_ctx) {
                                                avcodec_parameters_to_context(codec_ctx, codecpar);
                                                if (avcodec_open2(codec_ctx, codec, nullptr) >= 0) {
                                                    std::cout << "RTSP stream reconnected successfully" << std::endl;
                                                    consecutive_eof_count = 0;
                                                    last_reconnect_time = current_time;
                                                    av_packet_unref(pkt);
                                                    continue;
                                                } else {
                                                    avcodec_free_context(&codec_ctx);
                                                    codec_ctx = nullptr;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            
                            // 重连失败，恢复原来的设置
                            std::cerr << "Reconnection failed, resetting connection..." << std::endl;
                            if (saved_params) {
                                avcodec_parameters_free(&saved_params);
                            }
                            
                            // 尝试更彻底的重启
                            stop();
                            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                            
                            // 重新初始化
                            avformat_network_init();
                            AVDictionary* retry_options = nullptr;
                            av_dict_set(&retry_options, "rtsp_transport", "tcp", 0);
                            av_dict_set(&retry_options, "stimeout", "10000000", 0); // 10秒超时
                            av_dict_set(&retry_options, "max_delay", "5000000", 0); // 最大延迟
                            
                            if (avformat_open_input(&fmt_ctx, rtsp_url.c_str(), nullptr, &retry_options) == 0) {
                                if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
                                    // 重新查找视频流
                                    video_stream_idx = -1;
                                    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                                        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                                            video_stream_idx = i;
                                            break;
                                        }
                                    }
                                    
                                    if (video_stream_idx != -1) {
                                        AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
                                        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
                                        if (codec) {
                                            codec_ctx = avcodec_alloc_context3(codec);
                                            avcodec_parameters_to_context(codec_ctx, codecpar);
                                            if (avcodec_open2(codec_ctx, codec, nullptr) >= 0) {
                                                std::cout << "RTSP stream successfully reinitialized" << std::endl;
                                                consecutive_eof_count = 0;
                                                last_reconnect_time = current_time;
                                            }
                                        }
                                    }
                                }
                            }
                            av_dict_free(&retry_options);
                        } else {
                            // 还没到重连时间，等待
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    } else {
                        // 正常EOF，尝试seek到开头
                        std::cout << "End of stream reached, seeking to beginning..." << std::endl;
                        av_seek_frame(fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                } else {
                    // 其他错误，重置EOF计数
                    consecutive_eof_count = 0;
                    
                    // 网络错误，短暂等待后继续
                    if (ret == AVERROR(EAGAIN)) {
                        // 资源暂时不可用，短暂等待
                        lock.unlock();
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        lock.lock();
                    } else if (ret == AVERROR_EXIT) {
                        // 解码器退出，需要重新初始化
                        std::cerr << "Decoder exited, attempting to restart..." << std::endl;
                        stop();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        start();
                        break;
                    } else {
                        // 其他错误，等待后重试
                        std::cerr << "Error reading frame (code: " << ret << "), retrying..." << std::endl;
                        lock.unlock();
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        lock.lock();
                    }
                }
                
                av_packet_unref(pkt);
                continue;
            }
            
            // 重置连续EOF计数
            consecutive_eof_count = 0;
            
            if (pkt->stream_index == video_stream_idx) {
                ret = avcodec_send_packet(codec_ctx, pkt);
                if (ret < 0) {
                    // 发送包失败，可能是解码器问题
                    if (ret == AVERROR(EAGAIN)) {
                        // 解码器需要更多输出帧
                        avcodec_receive_frame(codec_ctx, frame);
                        av_frame_unref(frame);
                    } else if (ret == AVERROR_INVALIDDATA) {
                        // 无效数据，跳过
                        std::cerr << "Invalid packet data, skipping..." << std::endl;
                    } else if (ret == AVERROR_EXIT) {
                        // 解码器退出
                        std::cerr << "Decoder exited, attempting to restart..." << std::endl;
                        lock.unlock();
                        stop();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        start();
                        break;
                    }
                    
                    av_packet_unref(pkt);
                    continue;
                }
                
                while (ret >= 0) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        // 解码错误
                        std::cerr << "Error decoding frame, skipping..." << std::endl;
                        break;
                    }
                    
                    // 控制帧率：确保帧间隔不小于最小值
                    int64_t current_time = av_gettime() / 1000; // 毫秒
                    if (current_time - last_frame_time < min_frame_interval) {
                        av_frame_unref(frame);
                        break;
                    }
                    last_frame_time = current_time;
                    
                    // 降低帧率，每3帧取1帧
                    if (++frame_count % 3 != 0) {
                        av_frame_unref(frame);
                        break;
                    }
                    
                    // 转换到YUV420P格式（如果需要）
                    AVFrame* converted_frame = frame;
                    
                    if (frame->format != AV_PIX_FMT_YUV420P) {
                        if (!sws_ctx) {
                            sws_ctx = sws_getContext(
                                frame->width, frame->height,
                                (AVPixelFormat)frame->format,
                                frame->width, frame->height,
                                AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr
                            );
                        }
                        
                        if (sws_ctx) {
                            AVFrame* yuv_frame = av_frame_alloc();
                            yuv_frame->width = frame->width;
                            yuv_frame->height = frame->height;
                            yuv_frame->format = AV_PIX_FMT_YUV420P;
                            yuv_frame->pts = frame->pts;
                            av_frame_get_buffer(yuv_frame, 0);
                            
                            sws_scale(sws_ctx, frame->data, frame->linesize,
                                    0, frame->height,
                                    yuv_frame->data, yuv_frame->linesize);
                            
                            converted_frame = yuv_frame;
                        }
                    }
                    
                    // 存储YUV数据
                    YUVFrame yuv_frame(converted_frame);
                    if (!yuv_frame.empty()) {
                        frame_buffer.push(yuv_frame);
                    }
                    
                    // 清理临时帧
                    if (converted_frame != frame) {
                        av_frame_free(&converted_frame);
                    }
                    
                    av_frame_unref(frame);
                    break;  // 每次只处理一个帧
                }
            }
            
            av_packet_unref(pkt);
            
            // 短暂休眠，避免占用过多CPU
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
        }
        
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
        }
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }
    
};

// 画中画合成器
class PictureInPictureMixer {
private:
    std::mutex mixer_mutex;
    
    // 小画面位置和大小配置
    struct PiPSettings {
        int x = 20;              // 小画面左上角X坐标
        int y = 20;              // 小画面左上角Y坐标
        int width = 200;         // 小画面宽度
        int height = 150;        // 小画面高度
        int border_size = 2;     // 边框大小
        float opacity = 0.9f;    // 不透明度
    } pip_settings;
    
    // 编码器
    std::unique_ptr<JPEGEncoder> encoder;
    
    // 用于图像缩放
    SwsContext* main_sws_ctx = nullptr;
    SwsContext* pip_sws_ctx = nullptr;
    
    // 临时帧
    AVFrame* main_frame = nullptr;
    AVFrame* pip_frame = nullptr;
    AVFrame* pip_scaled_frame = nullptr;
    
public:
    PictureInPictureMixer() {
        // 初始化编码器（合成后的画面）
        encoder = std::make_unique<JPEGEncoder>(800, 600);
        
        // 分配临时帧
        main_frame = av_frame_alloc();
        pip_frame = av_frame_alloc();
        pip_scaled_frame = av_frame_alloc();
    }
    
    ~PictureInPictureMixer() {
        if (main_sws_ctx) sws_freeContext(main_sws_ctx);
        if (pip_sws_ctx) sws_freeContext(pip_sws_ctx);
        if (main_frame) av_frame_free(&main_frame);
        if (pip_frame) av_frame_free(&pip_frame);
        if (pip_scaled_frame) av_frame_free(&pip_scaled_frame);
    }
    
    // 合成画中画并编码为JPEG
    std::vector<uint8_t> mix_and_encode(YUVFrame& main_yuv, YUVFrame& pip_yuv) {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        std::vector<uint8_t> result;
        
        if (main_yuv.empty() && pip_yuv.empty()) {
            return result;
        }
        
        // 情况1: 只有主画面
        if (!main_yuv.empty() && pip_yuv.empty()) {
            return encode_single_frame(main_yuv);
        }
        
        // 情况2: 只有小画面
        if (main_yuv.empty() && !pip_yuv.empty()) {
            return encode_single_frame(pip_yuv);
        }
        
        // 情况3: 两个画面都有，进行合成
        try {
            // 准备主画面帧
            if (!prepare_main_frame(main_yuv)) {
                return result;
            }
            
            // 准备小画面帧
            if (!prepare_pip_frame(pip_yuv)) {
                return result;
            }
            
            // 执行画中画合成
            if (!perform_pip_composition()) {
                return result;
            }
            
            // 编码合成后的帧为JPEG
            result = encoder->encode(main_frame);
            
        } catch (const std::exception& e) {
            std::cerr << "Error in mix_and_encode: " << e.what() << std::endl;
        }
        
        return result;
    }
    
    // 编码单帧为JPEG
    std::vector<uint8_t> encode_single_frame(YUVFrame& yuv_frame) {
        if (!encoder || yuv_frame.empty()) {
            return std::vector<uint8_t>();
        }
        
        // 转换到AVFrame并编码
        AVFrame* frame = yuv_frame.to_avframe();
        if (!frame) {
            return std::vector<uint8_t>();
        }
        
        std::vector<uint8_t> result = encoder->encode(frame);
        av_frame_free(&frame);
        return result;
    }
    
    // 更新画中画设置
    void update_pip_settings(int x, int y, int width, int height, 
                           int border_size = 2, float opacity = 0.9f) {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        pip_settings.x = x;
        pip_settings.y = y;
        pip_settings.width = width;
        pip_settings.height = height;
        pip_settings.border_size = border_size;
        pip_settings.opacity = opacity;
        
        // 重置缩放上下文
        if (pip_sws_ctx) {
            sws_freeContext(pip_sws_ctx);
            pip_sws_ctx = nullptr;
        }
        
        std::cout << "PIP settings updated: x=" << x << ", y=" << y 
                  << ", width=" << width << ", height=" << height << std::endl;
    }
    
    // 获取当前设置
    PiPSettings get_settings() {
        std::lock_guard<std::mutex> lock(mixer_mutex);
        return pip_settings;
    }
    
private:
    // 准备主画面帧
    bool prepare_main_frame(YUVFrame& yuv_frame) {
        if (yuv_frame.empty()) {
            return false;
        }
        
        // 确保主画面帧已分配
        if (main_frame->width != 800 || main_frame->height != 600) {
            av_frame_unref(main_frame);
            main_frame->width = 800;
            main_frame->height = 600;
            main_frame->format = AV_PIX_FMT_YUV420P;
            if (av_frame_get_buffer(main_frame, 0) < 0) {
                std::cerr << "Failed to allocate main frame buffer" << std::endl;
                return false;
            }
        }
        
        // 创建或更新缩放上下文
        if (!main_sws_ctx) {
            main_sws_ctx = sws_getContext(
                yuv_frame.width, yuv_frame.height,
                AV_PIX_FMT_YUV420P,
                main_frame->width, main_frame->height,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            if (!main_sws_ctx) {
                std::cerr << "Failed to create main sws context" << std::endl;
                return false;
            }
        }
        
        // 转换为AVFrame
        AVFrame* input_frame = yuv_frame.to_avframe();
        if (!input_frame) {
            return false;
        }
        
        // 执行缩放
        sws_scale(main_sws_ctx, input_frame->data, input_frame->linesize,
                  0, input_frame->height,
                  main_frame->data, main_frame->linesize);
        
        av_frame_free(&input_frame);
        return true;
    }
    
    // 准备小画面帧
    bool prepare_pip_frame(YUVFrame& yuv_frame) {
        if (yuv_frame.empty()) {
            return false;
        }
        
        // 确保小画面缩放帧已分配
        if (pip_scaled_frame->width != pip_settings.width || 
            pip_scaled_frame->height != pip_settings.height) {
            av_frame_unref(pip_scaled_frame);
            pip_scaled_frame->width = pip_settings.width;
            pip_scaled_frame->height = pip_settings.height;
            pip_scaled_frame->format = AV_PIX_FMT_YUV420P;
            if (av_frame_get_buffer(pip_scaled_frame, 0) < 0) {
                std::cerr << "Failed to allocate PIP scaled frame buffer" << std::endl;
                return false;
            }
        }
        
        // 创建或更新缩放上下文
        if (!pip_sws_ctx) {
            pip_sws_ctx = sws_getContext(
                yuv_frame.width, yuv_frame.height,
                AV_PIX_FMT_YUV420P,
                pip_scaled_frame->width, pip_scaled_frame->height,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            if (!pip_sws_ctx) {
                std::cerr << "Failed to create PIP sws context" << std::endl;
                return false;
            }
        }
        
        // 转换为AVFrame
        AVFrame* input_frame = yuv_frame.to_avframe();
        if (!input_frame) {
            return false;
        }
        
        // 执行缩放
        sws_scale(pip_sws_ctx, input_frame->data, input_frame->linesize,
                  0, input_frame->height,
                  pip_scaled_frame->data, pip_scaled_frame->linesize);
        
        av_frame_free(&input_frame);
        return true;
    }
    
    // 执行画中画合成
    bool perform_pip_composition() {
        if (!main_frame || !pip_scaled_frame) {
            std::cerr << "Frames not prepared for composition" << std::endl;
            return false;
        }
        
        // 确保坐标在有效范围内
        int pip_x = std::max(0, std::min(pip_settings.x, main_frame->width - pip_settings.width));
        int pip_y = std::max(0, std::min(pip_settings.y, main_frame->height - pip_settings.height));
        
        // 计算实际的小画面尺寸（考虑边界）
        int actual_pip_width = std::min(pip_settings.width, main_frame->width - pip_x);
        int actual_pip_height = std::min(pip_settings.height, main_frame->height - pip_y);
        
        if (actual_pip_width <= 0 || actual_pip_height <= 0) {
            std::cerr << "Invalid PIP dimensions or position" << std::endl;
            return false;
        }
        
        // 应用不透明度
        float opacity = std::max(0.0f, std::min(1.0f, pip_settings.opacity));
        
        // 合成Y分量（亮度）
        for (int y = 0; y < actual_pip_height; y++) {
            int main_y = pip_y + y;
            int pip_y_idx = y;
            
            // 主画面Y平面行指针
            uint8_t* main_y_line = main_frame->data[0] + main_y * main_frame->linesize[0];
            // 小画面Y平面行指针
            uint8_t* pip_y_line = pip_scaled_frame->data[0] + pip_y_idx * pip_scaled_frame->linesize[0];
            
            // 混合Y分量
            for (int x = 0; x < actual_pip_width; x++) {
                int main_x = pip_x + x;
                int pip_x_idx = x;
                
                // 计算混合后的Y值
                uint8_t main_y_val = main_y_line[main_x];
                uint8_t pip_y_val = pip_y_line[pip_x_idx];
                
                // 线性混合
                uint8_t blended_y = static_cast<uint8_t>(
                    main_y_val * (1.0f - opacity) + pip_y_val * opacity
                );
                
                // 写回主画面
                main_y_line[main_x] = blended_y;
            }
        }
        
        // 合成U/V分量（色度）- 注意：YUV420中UV分量尺寸减半
        int pip_u_x = pip_x / 2;
        int pip_u_y = pip_y / 2;
        int actual_pip_u_width = actual_pip_width / 2;
        int actual_pip_u_height = actual_pip_height / 2;
        
        if (actual_pip_u_width > 0 && actual_pip_u_height > 0) {
            // 合成U分量
            for (int y = 0; y < actual_pip_u_height; y++) {
                int main_u_y = pip_u_y + y;
                int pip_u_y_idx = y;
                
                uint8_t* main_u_line = main_frame->data[1] + main_u_y * main_frame->linesize[1];
                uint8_t* pip_u_line = pip_scaled_frame->data[1] + pip_u_y_idx * pip_scaled_frame->linesize[1];
                
                for (int x = 0; x < actual_pip_u_width; x++) {
                    int main_u_x = pip_u_x + x;
                    int pip_u_x_idx = x;
                    
                    uint8_t main_u_val = main_u_line[main_u_x];
                    uint8_t pip_u_val = pip_u_line[pip_u_x_idx];
                    uint8_t blended_u = static_cast<uint8_t>(
                        main_u_val * (1.0f - opacity) + pip_u_val * opacity
                    );
                    
                    main_u_line[main_u_x] = blended_u;
                }
            }
            
            // 合成V分量
            for (int y = 0; y < actual_pip_u_height; y++) {
                int main_v_y = pip_u_y + y;
                int pip_v_y_idx = y;
                
                uint8_t* main_v_line = main_frame->data[2] + main_v_y * main_frame->linesize[2];
                uint8_t* pip_v_line = pip_scaled_frame->data[2] + pip_v_y_idx * pip_scaled_frame->linesize[2];
                
                for (int x = 0; x < actual_pip_u_width; x++) {
                    int main_v_x = pip_u_x + x;
                    int pip_v_x_idx = x;
                    
                    uint8_t main_v_val = main_v_line[main_v_x];
                    uint8_t pip_v_val = pip_v_line[pip_v_x_idx];
                    uint8_t blended_v = static_cast<uint8_t>(
                        main_v_val * (1.0f - opacity) + pip_v_val * opacity
                    );
                    
                    main_v_line[main_v_x] = blended_v;
                }
            }
        }
        
        // 添加边框（可选）
        if (pip_settings.border_size > 0) {
            add_border_to_pip(pip_x, pip_y, actual_pip_width, actual_pip_height);
        }
        
        return true;
    }
    
    // 为画中画添加边框
    void add_border_to_pip(int pip_x, int pip_y, int pip_width, int pip_height) {
        // 边框颜色（白色，YUV值）
        const uint8_t border_y = 255;
        const uint8_t border_u = 128;
        const uint8_t border_v = 128;
        
        int border_size = std::min(pip_settings.border_size, 
                                 std::min(pip_width, pip_height) / 4);
        
        // 上边框
        for (int y = 0; y < border_size; y++) {
            int actual_y = pip_y + y;
            if (actual_y >= 0 && actual_y < main_frame->height) {
                uint8_t* y_line = main_frame->data[0] + actual_y * main_frame->linesize[0];
                for (int x = pip_x; x < pip_x + pip_width; x++) {
                    if (x >= 0 && x < main_frame->width) {
                        y_line[x] = border_y;
                    }
                }
            }
        }
        
        // 下边框
        for (int y = 0; y < border_size; y++) {
            int actual_y = pip_y + pip_height - 1 - y;
            if (actual_y >= 0 && actual_y < main_frame->height) {
                uint8_t* y_line = main_frame->data[0] + actual_y * main_frame->linesize[0];
                for (int x = pip_x; x < pip_x + pip_width; x++) {
                    if (x >= 0 && x < main_frame->width) {
                        y_line[x] = border_y;
                    }
                }
            }
        }
        
        // 左边框
        for (int x = 0; x < border_size; x++) {
            int actual_x = pip_x + x;
            if (actual_x >= 0 && actual_x < main_frame->width) {
                for (int y = pip_y; y < pip_y + pip_height; y++) {
                    if (y >= 0 && y < main_frame->height) {
                        uint8_t* y_line = main_frame->data[0] + y * main_frame->linesize[0];
                        y_line[actual_x] = border_y;
                    }
                }
            }
        }
        
        // 右边框
        for (int x = 0; x < border_size; x++) {
            int actual_x = pip_x + pip_width - 1 - x;
            if (actual_x >= 0 && actual_x < main_frame->width) {
                for (int y = pip_y; y < pip_y + pip_height; y++) {
                    if (y >= 0 && y < main_frame->height) {
                        uint8_t* y_line = main_frame->data[0] + y * main_frame->linesize[0];
                        y_line[actual_x] = border_y;
                    }
                }
            }
        }
    }
};

// 全局解码器管理器
class DecoderManager {
private:
    // 两个解码器
    std::unique_ptr<RTSPDecoder> decoder1;
    std::unique_ptr<RTSPDecoder> decoder2;
    
    // 两个YUV帧缓冲区
    std::unique_ptr<YUVFrameBuffer> frame_buffer1;
    std::unique_ptr<YUVFrameBuffer> frame_buffer2;
    
    // 合成后的JPEG帧缓冲区
    std::unique_ptr<std::queue<std::vector<uint8_t>>> jpeg_buffer;
    std::mutex jpeg_buffer_mutex;
    
    // 画中画合成器
    std::unique_ptr<PictureInPictureMixer> pip_mixer;
    
    std::mutex manager_mutex;
    std::string current_url1;
    std::string current_url2;
    
    // 合成线程
    std::thread mix_thread;
    std::atomic<bool> mixing{false};
    
    // 性能统计
    std::atomic<int> frames_mixed{0};
    std::atomic<int64_t> last_stat_time{0};
    
public:
    DecoderManager() 
        : frame_buffer1(std::make_unique<YUVFrameBuffer>()),
          frame_buffer2(std::make_unique<YUVFrameBuffer>()),
          jpeg_buffer(std::make_unique<std::queue<std::vector<uint8_t>>>()),
          pip_mixer(std::make_unique<PictureInPictureMixer>()) {
        last_stat_time = av_gettime() / 1000;
    }
    
    ~DecoderManager() {
        stop_all();
        if (mix_thread.joinable()) {
            mixing = false;
            mix_thread.join();
        }
    }
    
    // 同时启动两个流
    bool start_streams(const std::string& rtsp_url1, const std::string& rtsp_url2) {
        std::lock_guard<std::mutex> lock(manager_mutex);
        
        // 如果已经在运行，先停止
        if ((decoder1 && decoder1->is_running()) || (decoder2 && decoder2->is_running())) {
            stop_all();
        }
        
        if (rtsp_url1.empty() || rtsp_url2.empty()) {
            std::cerr << "Both RTSP URLs are required" << std::endl;
            return false;
        }
        
        current_url1 = rtsp_url1;
        current_url2 = rtsp_url2;
        
        // 创建解码器
        decoder1 = std::make_unique<RTSPDecoder>(rtsp_url1, *frame_buffer1);
        decoder2 = std::make_unique<RTSPDecoder>(rtsp_url2, *frame_buffer2);
        
        // 同时启动两个解码器
        bool success1 = decoder1->start();
        bool success2 = decoder2->start();
        
        if (success1 && success2) {
            std::cout << "Both streams started successfully" << std::endl;
            std::cout << "Stream 1: " << rtsp_url1 << std::endl;
            std::cout << "Stream 2: " << rtsp_url2 << std::endl;
            
            // 启动合成线程
            start_mixing();
            return true;
        } else {
            std::cerr << "Failed to start streams" << std::endl;
            if (decoder1) decoder1->stop();
            if (decoder2) decoder2->stop();
            decoder1.reset();
            decoder2.reset();
            current_url1.clear();
            current_url2.clear();
            return false;
        }
    }
    
    // 停止所有流
    void stop_all() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        if (decoder1) {
            decoder1->stop();
            decoder1.reset();
            frame_buffer1->clear();
        }
        if (decoder2) {
            decoder2->stop();
            decoder2.reset();
            frame_buffer2->clear();
        }
        
        // 清空JPEG缓冲区
        {
            std::lock_guard<std::mutex> lock(jpeg_buffer_mutex);
            if (jpeg_buffer) {
                while (!jpeg_buffer->empty()) {
                    jpeg_buffer->pop();
                }
            }
        }
        
        current_url1.clear();
        current_url2.clear();
        std::cout << "All streams stopped" << std::endl;
    }
    
    // 获取混合后的JPEG帧
    bool get_mixed_frame(std::vector<uint8_t>& frame) {
        static std::vector<uint8_t> last_frame;  // 静态变量存储最后一帧
        
        std::lock_guard<std::mutex> lock(jpeg_buffer_mutex);
        
        if (jpeg_buffer && !jpeg_buffer->empty()) {
            frame = jpeg_buffer->front();
            jpeg_buffer->pop();
            last_frame = frame;  // 保存为最后一帧
            return true;
        }
        
        // 如果没有新帧，返回最后一帧
        if (!last_frame.empty()) {
            frame = last_frame;
            return true;
        }
        
        return false;
    }
    
    // 更新画中画设置
    bool update_pip_settings(int x, int y, int width, int height, 
                           int border_size = 2, float opacity = 0.9f) {
        std::lock_guard<std::mutex> lock(manager_mutex);
        if (!pip_mixer) return false;
        
        if (width <= 0 || height <= 0 || width > 800 || height > 600) {
            return false;
        }
        
        if (x < 0 || y < 0 || x + width > 800 || y + height > 600) {
            return false;
        }
        
        pip_mixer->update_pip_settings(x, y, width, height, border_size, opacity);
        return true;
    }
    
    // 获取当前设置
    std::string get_pip_settings_json() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        if (!pip_mixer) return "{}";
        
        auto settings = pip_mixer->get_settings();
        std::stringstream ss;
        ss << "{";
        ss << "\"x\":" << settings.x << ",";
        ss << "\"y\":" << settings.y << ",";
        ss << "\"width\":" << settings.width << ",";
        ss << "\"height\":" << settings.height << ",";
        ss << "\"border_size\":" << settings.border_size << ",";
        ss << "\"opacity\":" << std::fixed << std::setprecision(2) << settings.opacity;
        ss << "}";
        return ss.str();
    }
    
    std::string get_current_url1() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return current_url1;
    }
    
    std::string get_current_url2() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return current_url2;
    }
    
    bool is_streaming() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return decoder1 && decoder1->is_running() && decoder2 && decoder2->is_running();
    }
    
private:
    void start_mixing() {
        if (!mixing) {
            mixing = true;
            mix_thread = std::thread(&DecoderManager::mixing_loop, this);
        }
    }
    
    void mixing_loop() {
        std::vector<uint8_t> last_successful_frame;  // 存储最后一帧成功合成的帧
        int64_t last_mix_time = 0;
        const int64_t target_mix_interval = 33; // 目标合成间隔 33ms (~30fps)
        
        while (mixing) {
            int64_t current_time = av_gettime() / 1000; // 毫秒
            
            // 控制合成帧率
            if (current_time - last_mix_time < target_mix_interval) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            
            last_mix_time = current_time;
            
            YUVFrame frame1, frame2;
            bool has_frame1 = false;
            bool has_frame2 = false;
            
            {
                std::lock_guard<std::mutex> lock(manager_mutex);
                
                // 检查两个解码器是否都在运行
                if (!decoder1 || !decoder1->is_running() || !decoder2 || !decoder2->is_running()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                
                // 尝试获取两个流的帧，设置超时
                has_frame1 = frame_buffer1->get_latest(frame1, 10);
                has_frame2 = frame_buffer2->get_latest(frame2, 10);
            }
            
            std::vector<uint8_t> mixed_frame;
            
            if (has_frame1 && has_frame2) {
                // 合成画中画
                mixed_frame = pip_mixer->mix_and_encode(frame1, frame2);
                
                if (!mixed_frame.empty()) {
                    last_successful_frame = mixed_frame;
                    frames_mixed++;
                    
                    std::lock_guard<std::mutex> lock(jpeg_buffer_mutex);
                    if (jpeg_buffer) {
                        // 限制缓冲区大小，只保留最新的一帧
                        while (!jpeg_buffer->empty()) {
                            jpeg_buffer->pop();
                        }
                        jpeg_buffer->push(mixed_frame);
                    }
                }
            }
            
            // 如果合成失败但有上一次成功的帧，使用上一次的帧
            if (mixed_frame.empty() && !last_successful_frame.empty()) {
                std::lock_guard<std::mutex> lock(jpeg_buffer_mutex);
                if (jpeg_buffer) {
                    while (!jpeg_buffer->empty()) {
                        jpeg_buffer->pop();
                    }
                    jpeg_buffer->push(last_successful_frame);
                }
            }
            
            // 性能统计
            int64_t now = av_gettime() / 1000;
            if (now - last_stat_time >= 5000) { // 每5秒输出一次统计
                std::cout << "合成帧率: " << (frames_mixed * 1000 / (now - last_stat_time)) << " fps" << std::endl;
                frames_mixed = 0;
                last_stat_time = now;
            }
            
            // 短暂休眠，避免占用过多CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

// HTML页面内容
const std::string HTML_PAGE = R"====(
    <!DOCTYPE html>
    <html lang="zh-CN">
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>RTSP画中画视频流播放器</title>
        <style>
            * {
                margin: 0;
                padding: 0;
                box-sizing: border-box;
            }
            
            body {
                font-family: "Microsoft YaHei", "Segoe UI", sans-serif;
                background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
                min-height: 100vh;
                padding: 20px;
            }
            
            .container {
                background: white;
                border-radius: 20px;
                box-shadow: 0 20px 40px rgba(0, 0, 0, 0.1);
                overflow: hidden;
                width: 100%;
                max-width: 1400px;
                margin: 0 auto;
                animation: fadeIn 0.5s ease;
            }
            
            @keyframes fadeIn {
                from { opacity: 0; transform: translateY(20px); }
                to { opacity: 1; transform: translateY(0); }
            }
            
            .header {
                background: linear-gradient(135deg, #4b6cb7 0%, #182848 100%);
                color: white;
                padding: 30px;
                text-align: center;
            }
            
            .header h1 {
                font-size: 32px;
                margin-bottom: 10px;
                font-weight: 600;
            }
            
            .header p {
                opacity: 0.9;
                font-size: 16px;
            }
            
            .content {
                padding: 30px;
                display: grid;
                grid-template-columns: 1fr 400px;
                gap: 30px;
            }
            
            @media (max-width: 1200px) {
                .content {
                    grid-template-columns: 1fr;
                }
            }
            
            .main-content {
                display: flex;
                flex-direction: column;
                gap: 30px;
            }
            
            .video-section {
                background: #f8f9fa;
                padding: 20px;
                border-radius: 15px;
                border: 1px solid #e9ecef;
            }
            
            .video-title {
                color: #495057;
                margin-bottom: 15px;
                font-size: 20px;
                font-weight: 600;
                display: flex;
                align-items: center;
                gap: 10px;
            }
            
            .video-title i {
                color: #4b6cb7;
            }
            
            .video-container {
                width: 100%;
                background: #000;
                border-radius: 12px;
                overflow: hidden;
                box-shadow: 0 10px 30px rgba(0, 0, 0, 0.2);
                position: relative;
                aspect-ratio: 16/9;
            }
            
            #video {
                width: 100%;
                height: 100%;
                object-fit: contain;
                display: block;
            }
            
            .video-placeholder {
                position: absolute;
                top: 0;
                left: 0;
                right: 0;
                bottom: 0;
                display: flex;
                flex-direction: column;
                align-items: center;
                justify-content: center;
                color: white;
                font-size: 18px;
                background: linear-gradient(135deg, #2c3e50 0%, #4ca1af 100%);
                text-align: center;
            }
            
            /* 移除红色预览框和相关元素的样式 */
            .pip-preview-overlay {
                display: none !important;
            }
            
            .pip-preview-info {
                display: none !important;
            }
            
            .control-panel {
                background: #f8f9fa;
                padding: 25px;
                border-radius: 15px;
                border: 1px solid #e9ecef;
            }
            
            .panel-title {
                color: #495057;
                margin-bottom: 20px;
                font-size: 20px;
                font-weight: 600;
                display: flex;
                align-items: center;
                gap: 10px;
            }
            
            .panel-title i {
                color: #4b6cb7;
            }
            
            .input-group {
                margin-bottom: 20px;
            }
            
            .input-group label {
                display: block;
                margin-bottom: 8px;
                color: #495057;
                font-weight: 500;
                font-size: 14px;
                display: flex;
                align-items: center;
                gap: 5px;
            }
            
            .rtsp-input {
                width: 100%;
                padding: 12px 15px;
                border: 2px solid #e9ecef;
                border-radius: 10px;
                font-size: 14px;
                margin-bottom: 10px;
                transition: border-color 0.3s;
            }
            
            .rtsp-input:focus {
                outline: none;
                border-color: #4b6cb7;
                box-shadow: 0 0 0 3px rgba(75, 108, 183, 0.1);
            }
            
            .rtsp-input::placeholder {
                color: #adb5bd;
            }
            
            .btn {
                padding: 12px 20px;
                border: none;
                border-radius: 10px;
                font-size: 14px;
                font-weight: 500;
                cursor: pointer;
                transition: all 0.3s ease;
                width: 100%;
                margin-bottom: 10px;
                display: flex;
                align-items: center;
                justify-content: center;
                gap: 8px;
            }
            
            .btn-start {
                background: linear-gradient(135deg, #28a745 0%, #20c997 100%);
                color: white;
            }
            
            .btn-stop {
                background: linear-gradient(135deg, #dc3545 0%, #e83e8c 100%);
                color: white;
            }
            
            .btn-apply {
                background: linear-gradient(135deg, #007bff 0%, #0056b3 100%);
                color: white;
            }
            
            .btn:hover {
                opacity: 0.9;
                transform: translateY(-2px);
                box-shadow: 0 5px 15px rgba(0, 0, 0, 0.1);
            }
            
            .pip-controls {
                background: linear-gradient(135deg, #f8f9fa 0%, #e9ecef 100%);
                padding: 25px;
                border-radius: 15px;
                border: 2px solid #4b6cb7;
                box-shadow: 0 5px 15px rgba(75, 108, 183, 0.1);
            }
            
            .pip-controls .panel-title {
                color: #4b6cb7;
            }
            
            .control-section {
                margin-bottom: 25px;
                padding-bottom: 15px;
                border-bottom: 1px solid #dee2e6;
            }
            
            .control-section:last-child {
                border-bottom: none;
                margin-bottom: 0;
                padding-bottom: 0;
            }
            
            .control-section-title {
                color: #495057;
                margin-bottom: 15px;
                font-size: 16px;
                font-weight: 600;
                display: flex;
                align-items: center;
                gap: 8px;
            }
            
            .slider-group {
                margin-bottom: 15px;
            }
            
            .slider-container {
                display: flex;
                align-items: center;
                gap: 15px;
            }
            
            .slider-container label {
                min-width: 80px;
                color: #495057;
                font-weight: 500;
                font-size: 14px;
                display: flex;
                align-items: center;
                gap: 5px;
            }
            
            input[type="range"] {
                flex: 1;
                height: 8px;
                -webkit-appearance: none;
                background: linear-gradient(90deg, #4b6cb7 0%, #e9ecef 100%);
                border-radius: 4px;
                outline: none;
            }
            
            input[type="range"]::-webkit-slider-thumb {
                -webkit-appearance: none;
                width: 22px;
                height: 22px;
                background: #4b6cb7;
                border-radius: 50%;
                cursor: pointer;
                border: 3px solid white;
                box-shadow: 0 2px 5px rgba(0, 0, 0, 0.2);
            }
            
            .slider-value {
                min-width: 40px;
                text-align: center;
                font-weight: 600;
                color: #4b6cb7;
                background: white;
                padding: 4px 8px;
                border-radius: 6px;
                border: 1px solid #dee2e6;
            }
            
            .preset-buttons {
                display: grid;
                grid-template-columns: repeat(3, 1fr);
                gap: 10px;
                margin-top: 10px;
            }
            
            .btn-preset {
                background: #6c757d;
                color: white;
                padding: 10px;
                font-size: 12px;
                transition: all 0.3s ease;
            }
            
            .btn-preset:hover {
                background: #5a6268;
                transform: translateY(-1px);
            }
            
            .preset-label {
                display: block;
                font-size: 10px;
                margin-top: 2px;
                opacity: 0.9;
            }
            
            .pip-visual-preview {
                background: #2c3e50;
                border-radius: 10px;
                padding: 15px;
                margin-top: 20px;
                position: relative;
                overflow: hidden;
            }
            
            .preview-title {
                color: white;
                margin-bottom: 10px;
                font-size: 14px;
                text-align: center;
            }
            
            .preview-container {
                width: 100%;
                height: 150px;
                background: #34495e;
                border-radius: 8px;
                position: relative;
                overflow: hidden;
                border: 2px solid #4b6cb7;
            }
            
            .preview-main {
                position: absolute;
                top: 0;
                left: 0;
                right: 0;
                bottom: 0;
                background: linear-gradient(135deg, #3498db 0%, #2c3e50 100%);
            }
            
            .preview-pip {
                position: absolute;
                background: linear-gradient(135deg, #e74c3c 0%, #c0392b 100%);
                border: 2px solid white;
                box-shadow: 0 0 0 1px rgba(0, 0, 0, 0.3);
                transition: all 0.3s ease;
            }
            
            .preview-label {
                position: absolute;
                color: white;
                font-size: 12px;
                padding: 2px 6px;
                background: rgba(0, 0, 0, 0.5);
                border-radius: 3px;
            }
            
            .status-container {
                display: grid;
                grid-template-columns: 1fr 1fr;
                gap: 20px;
                margin-top: 20px;
            }
            
            @media (max-width: 768px) {
                .status-container {
                    grid-template-columns: 1fr;
                }
            }
            
            .status-box {
                background: #e7f5ff;
                padding: 20px;
                border-radius: 12px;
                border: 1px solid #d0ebff;
            }
            
            .status-title {
                font-size: 16px;
                color: #495057;
                margin-bottom: 10px;
                display: flex;
                align-items: center;
                gap: 8px;
            }
            
            .status-indicator {
                display: inline-block;
                width: 12px;
                height: 12px;
                border-radius: 50%;
                background: #6c757d;
                margin-right: 10px;
            }
            
            .status-indicator.active {
                background: #28a745;
                box-shadow: 0 0 10px rgba(40, 167, 69, 0.5);
                animation: pulse 1.5s infinite;
            }
            
            @keyframes pulse {
                0% { opacity: 1; }
                50% { opacity: 0.5; }
                100% { opacity: 1; }
            }
            
            .stream-url {
                margin-top: 10px;
                padding: 10px;
                background: white;
                border-radius: 8px;
                font-size: 12px;
                color: #1864ab;
                word-break: break-all;
            }
            
            .loading-overlay {
                display: none;
                position: absolute;
                top: 0;
                left: 0;
                right: 0;
                bottom: 0;
                background: rgba(0, 0, 0, 0.85);
                flex-direction: column;
                align-items: center;
                justify-content: center;
                color: white;
                font-size: 18px;
                z-index: 100;
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
            
            .info-panel {
                margin-top: 30px;
                padding: 25px;
                background: linear-gradient(135deg, #f8f9fa 0%, #e9ecef 100%);
                border-radius: 15px;
                border-left: 5px solid #4b6cb7;
                grid-column: 1 / -1;
            }
            
            .info-panel h3 {
                color: #495057;
                margin-bottom: 15px;
                font-size: 20px;
                display: flex;
                align-items: center;
                gap: 10px;
            }
            
            .info-list {
                list-style: none;
            }
            
            .info-list li {
                margin: 12px 0;
                padding: 12px 15px;
                background: white;
                border-radius: 8px;
                font-size: 14px;
                color: #6c757d;
                border: 1px solid #dee2e6;
                display: flex;
                align-items: flex-start;
                gap: 10px;
            }
            
            .info-list li::before {
                content: "✓";
                color: #28a745;
                font-weight: bold;
            }
            
            /* 图标样式 */
            .material-icons {
                font-family: 'Material Icons';
                font-weight: normal;
                font-style: normal;
                font-size: 24px;
                line-height: 1;
                letter-spacing: normal;
                text-transform: none;
                display: inline-block;
                white-space: nowrap;
                word-wrap: normal;
                direction: ltr;
                -webkit-font-smoothing: antialiased;
            }
            
            .material-icons-outlined {
                font-family: 'Material Icons Outlined';
                font-weight: normal;
                font-style: normal;
                font-size: 24px;
                line-height: 1;
                letter-spacing: normal;
                text-transform: none;
                display: inline-block;
                white-space: nowrap;
                word-wrap: normal;
                direction: ltr;
                -webkit-font-smoothing: antialiased;
            }
        </style>
        <!-- Material Icons -->
        <link href="https://fonts.googleapis.com/icon?family=Material+Icons|Material+Icons+Outlined" rel="stylesheet">
    </head>
    <body>
        <div class="container">
            <div class="header">
                <h1>🎥 RTSP画中画视频流播放器</h1>
                <p>双流同步播放 • 实时画中画合成 • 直观可视化控制</p>
            </div>
            
            <div class="content">
                <div class="main-content">
                    <div class="video-section">
                        <div class="video-title">
                            <span class="material-icons-outlined">videocam</span>
                            <span>视频预览画面</span>
                        </div>
                        <div class="video-container">
                            <div id="loading" class="loading-overlay">
                                <div class="spinner"></div>
                                <div>正在连接视频流...</div>
                            </div>
                            <div id="videoPlaceholder" class="video-placeholder">
                                <div style="margin-bottom: 20px;">
                                    <span class="material-icons-outlined" style="font-size: 48px;">videocam_off</span>
                                </div>
                                <h3 style="margin-bottom: 10px;">等待视频流</h3>
                                <p>请输入两个RTSP地址并开始播放</p>
                            </div>
                            <img id="video" src="" style="display: none;">
                            <!-- 移除红色预览框元素 -->
                            <!-- <div id="pipPreviewOverlay" class="pip-preview-overlay"></div> -->
                            <!-- <div id="pipPreviewInfo" class="pip-preview-info"></div> -->
                        </div>
                        
                        <div class="status-container">
                            <div class="status-box">
                                <div class="status-title">
                                    <span class="material-icons-outlined">info</span>
                                    <span>播放状态</span>
                                </div>
                                <div id="statusText">未连接</div>
                                <div class="status-indicator" id="statusIndicator"></div>
                            </div>
                            
                            <div class="status-box">
                                <div class="status-title">
                                    <span class="material-icons-outlined">link</span>
                                    <span>流地址状态</span>
                                </div>
                                <div id="currentStreams" class="stream-url">
                                    <div><strong>主画面:</strong> <span id="streamUrl1">未连接</span></div>
                                    <div><strong>小画面:</strong> <span id="streamUrl2">未连接</span></div>
                                </div>
                            </div>
                        </div>
                    </div>
                    
                    <div class="info-panel">
                        <h3><span class="material-icons-outlined">help_outline</span> 使用说明</h3>
                        <ul class="info-list">
                            <li>必须同时输入两个RTSP地址才能开始播放</li>
                            <li>两个视频流将同步开始和停止</li>
                            <li>主画面作为背景，小画面作为画中画叠加显示</li>
                            <li>右侧可实时调整画中画的位置和大小，立即生效</li>
                            <li>修改画中画设置后，视频画面会实时更新</li>
                            <li>支持预设位置快速切换</li>
                            <li>右侧控制面板提供画中画位置的可视化预览</li>
                        </ul>
                    </div>
                </div>
                
                <div class="right-panel">
                    <div class="pip-controls">
                        <div class="panel-title">
                            <span class="material-icons">picture_in_picture_alt</span>
                            <span>画中画控制</span>
                        </div>
                        
                        <div class="pip-visual-preview">
                            <div class="preview-title">画中画位置预览</div>
                            <div class="preview-container">
                                <div class="preview-main"></div>
                                <div id="previewPip" class="preview-pip"></div>
                                <div class="preview-label" style="top: 5px; left: 5px;">主画面</div>
                                <div id="pipPreviewLabel" class="preview-label" style="bottom: 5px; right: 5px;">小画面</div>
                            </div>
                        </div>
                        
                        <div class="control-section">
                            <div class="control-section-title">
                                <span class="material-icons-outlined">straighten</span>
                                <span>位置与尺寸</span>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">horizontal_distribute</span>
                                        <span>位置 X:</span>
                                    </label>
                                    <input type="range" id="pipX" min="0" max="600" value="20" step="10">
                                    <span id="pipXValue" class="slider-value">20</span>
                                </div>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">vertical_distribute</span>
                                        <span>位置 Y:</span>
                                    </label>
                                    <input type="range" id="pipY" min="0" max="400" value="20" step="10">
                                    <span id="pipYValue" class="slider-value">20</span>
                                </div>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">width_normal</span>
                                        <span>宽度:</span>
                                    </label>
                                    <input type="range" id="pipWidth" min="50" max="400" value="200" step="10">
                                    <span id="pipWidthValue" class="slider-value">200</span>
                                </div>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">height</span>
                                        <span>高度:</span>
                                    </label>
                                    <input type="range" id="pipHeight" min="50" max="300" value="150" step="10">
                                    <span id="pipHeightValue" class="slider-value">150</span>
                                </div>
                            </div>
                        </div>
                        
                        <div class="control-section">
                            <div class="control-section-title">
                                <span class="material-icons-outlined">apps</span>
                                <span>预设位置</span>
                            </div>
                            
                            <div class="preset-buttons">
                                <button class="btn btn-preset" onclick="setPipPreset('top-left')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">north_west</span>
                                    <span class="preset-label">左上</span>
                                </button>
                                <button class="btn btn-preset" onclick="setPipPreset('top-center')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">north</span>
                                    <span class="preset-label">上中</span>
                                </button>
                                <button class="btn btn-preset" onclick="setPipPreset('top-right')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">north_east</span>
                                    <span class="preset-label">右上</span>
                                </button>
                                <button class="btn btn-preset" onclick="setPipPreset('center-left')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">west</span>
                                    <span class="preset-label">左中</span>
                                </button>
                                <button class="btn btn-preset" onclick="setPipPreset('center')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">center_focus_strong</span>
                                    <span class="preset-label">居中</span>
                                </button>
                                <button class="btn btn-preset" onclick="setPipPreset('center-right')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">east</span>
                                    <span class="preset-label">右中</span>
                                </button>
                                <button class="btn btn-preset" onclick="setPipPreset('bottom-left')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">south_west</span>
                                    <span class="preset-label">左下</span>
                                </button>
                                <button class="btn btn-preset" onclick="setPipPreset('bottom-center')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">south</span>
                                    <span class="preset-label">下中</span>
                                </button>
                                <button class="btn btn-preset" onclick="setPipPreset('bottom-right')">
                                    <span class="material-icons-outlined" style="font-size: 20px;">south_east</span>
                                    <span class="preset-label">右下</span>
                                </button>
                            </div>
                        </div>
                        
                        <button id="applyPipBtn" class="btn btn-apply" onclick="applyPipSettings()">
                            <span class="material-icons">check_circle</span>
                            <span>立即应用画中画设置</span>
                        </button>
                    </div>
                    
                    <div class="control-panel">
                        <div class="panel-title">
                            <span class="material-icons">settings</span>
                            <span>视频流控制</span>
                        </div>
                        
                        <div class="input-group">
                            <label>
                                <span class="material-icons-outlined" style="font-size: 18px;">videocam</span>
                                <span>主画面RTSP地址</span>
                            </label>
                            <input type="text" 
                                   id="rtspUrl1" 
                                   class="rtsp-input" 
                                   placeholder="rtsp://username:password@ip:port/path"
                                   value="rtsp://192.168.1.100:554/stream1">
                        </div>
                        
                        <div class="input-group">
                            <label>
                                <span class="material-icons-outlined" style="font-size: 18px;">picture_in_picture</span>
                                <span>小画面RTSP地址</span>
                            </label>
                            <input type="text" 
                                   id="rtspUrl2" 
                                   class="rtsp-input" 
                                   placeholder="rtsp://username:password@ip:port/path"
                                   value="rtsp://192.168.1.101:554/stream2">
                        </div>
                        
                        <button id="startBtn" class="btn btn-start" onclick="startStreams()">
                            <span class="material-icons">play_arrow</span>
                            <span>开始播放双流</span>
                        </button>
                        <button id="stopBtn" class="btn btn-stop" onclick="stopStreams()">
                            <span class="material-icons">stop</span>
                            <span>停止播放</span>
                        </button>
                        
                        <div class="control-section" style="margin-top: 20px;">
                            <div class="control-section-title">
                                <span class="material-icons-outlined">speed</span>
                                <span>性能信息</span>
                            </div>
                            <div id="performanceInfo" style="font-size: 12px; color: #6c757d; padding: 10px; background: white; border-radius: 8px; border: 1px solid #dee2e6;">
                                帧率: -- fps<br>
                                延迟: -- ms<br>
                                状态: 等待连接...
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
        
        <script>
            let streamInterval = null;
            let isStreaming = false;
            let currentStreamUrl1 = '';
            let currentStreamUrl2 = '';
            let retryCount = 0;
            const maxRetries = 3;
            let lastFrameTime = 0;
            let frameCount = 0;
            let fps = 0;
            
            // 初始化滑块事件
            const sliders = ['pipX', 'pipY', 'pipWidth', 'pipHeight'];
            sliders.forEach(sliderId => {
                const slider = document.getElementById(sliderId);
                const valueDisplay = document.getElementById(sliderId + 'Value');
                
                slider.addEventListener('input', function(e) {
                    valueDisplay.textContent = e.target.value;
                    updatePipPreview();
                    // 移除视频预览框更新
                });
                
                slider.addEventListener('change', function(e) {
                    // 滑块释放时更新预览
                    updatePipPreview();
                    // 移除视频预览框更新
                });
            });
            
            // 更新画中画预览（仅右侧控制面板的预览）
            function updatePipPreview() {
                const x = parseInt(document.getElementById('pipX').value);
                const y = parseInt(document.getElementById('pipY').value);
                const width = parseInt(document.getElementById('pipWidth').value);
                const height = parseInt(document.getElementById('pipHeight').value);
                
                const previewPip = document.getElementById('previewPip');
                const previewContainer = document.querySelector('.preview-container');
                
                // 计算预览尺寸比例
                const containerWidth = previewContainer.clientWidth;
                const containerHeight = previewContainer.clientHeight;
                
                // 800x600 是实际视频尺寸，预览区域是等比例缩小
                const scaleX = containerWidth / 800;
                const scaleY = containerHeight / 600;
                
                const previewX = x * scaleX;
                const previewY = y * scaleY;
                const previewWidth = width * scaleX;
                const previewHeight = height * scaleY;
                
                previewPip.style.left = previewX + 'px';
                previewPip.style.top = previewY + 'px';
                previewPip.style.width = previewWidth + 'px';
                previewPip.style.height = previewHeight + 'px';
                
                // 更新预览标签位置和文本
                const pipPreviewLabel = document.getElementById('pipPreviewLabel');
                pipPreviewLabel.textContent = `${width}×${height}`;
                pipPreviewLabel.style.left = (previewX + 5) + 'px';
                pipPreviewLabel.style.top = (previewY + 5) + 'px';
                pipPreviewLabel.style.right = 'auto';
                pipPreviewLabel.style.bottom = 'auto';
            }
            
            // 移除更新视频预览方框的函数
            
            function updateStatus(status, isActive = false) {
                const statusText = document.getElementById('statusText');
                const statusIndicator = document.getElementById('statusIndicator');
                const streamUrl1 = document.getElementById('streamUrl1');
                const streamUrl2 = document.getElementById('streamUrl2');
                
                statusText.textContent = status;
                statusIndicator.className = 'status-indicator' + (isActive ? ' active' : '');
                
                if (currentStreamUrl1) {
                    streamUrl1.textContent = currentStreamUrl1;
                } else {
                    streamUrl1.textContent = '未连接';
                }
                if (currentStreamUrl2) {
                    streamUrl2.textContent = currentStreamUrl2;
                } else {
                    streamUrl2.textContent = '未连接';
                }
            }
            
            function updatePerformanceInfo(fps, latency) {
                const perfInfo = document.getElementById('performanceInfo');
                const now = performance.now();
                const currentLatency = lastFrameTime ? (now - lastFrameTime).toFixed(0) : '--';
                
                perfInfo.innerHTML = `
                    帧率: ${fps} fps<br>
                    延迟: ${currentLatency} ms<br>
                    状态: ${isStreaming ? '播放中' : '已停止'}
                `;
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
            
            function startStreams() {
                const rtspUrl1 = document.getElementById('rtspUrl1').value.trim();
                const rtspUrl2 = document.getElementById('rtspUrl2').value.trim();
                
                if (!rtspUrl1 || !rtspUrl2) {
                    alert('必须输入两个RTSP地址');
                    return;
                }
                
                if (isStreaming) {
                    alert('已经在播放中，请先停止当前播放');
                    return;
                }
                
                showLoading(true);
                updateStatus('正在连接视频流...', true);
                
                fetch('/start_streams', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: 'rtsp_url1=' + encodeURIComponent(rtspUrl1) + 
                          '&rtsp_url2=' + encodeURIComponent(rtspUrl2)
                })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        currentStreamUrl1 = rtspUrl1;
                        currentStreamUrl2 = rtspUrl2;
                        isStreaming = true;
                        
                        // 显示视频元素
                        const video = document.getElementById('video');
                        const videoPlaceholder = document.getElementById('videoPlaceholder');
                        
                        video.style.display = 'block';
                        videoPlaceholder.style.display = 'none';
                        
                        // 开始获取视频帧
                        startVideoStream();
                        
                        updateStatus('正在播放', true);
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
            
            function startVideoStream() {
                if (streamInterval) {
                    clearInterval(streamInterval);
                }
                
                // 立即显示第一帧
                updateVideoFrame();
                
                // 每33ms更新一次帧（约30fps）
                streamInterval = setInterval(updateVideoFrame, 33);
                
                // 每100ms更新性能信息
                setInterval(() => {
                    if (isStreaming) {
                        updatePerformanceInfo(fps);
                    }
                }, 100);
            }
            
            function updateVideoFrame() {
                if (!isStreaming) return;
                
                const video = document.getElementById('video');
                const timestamp = new Date().getTime();
                
                // 更新帧率统计
                frameCount++;
                const now = Date.now();
                if (now - lastFrameTime >= 1000) {
                    fps = frameCount;
                    frameCount = 0;
                    lastFrameTime = now;
                }
                
                video.src = '/video_frame?t=' + timestamp + '&r=' + Math.random();
                
                // 添加加载超时处理
                const loadTimeout = setTimeout(() => {
                    if (video.complete === false || video.naturalWidth === 0) {
                        retryCount++;
                        if (retryCount < maxRetries) {
                            console.log('Frame load timeout, retrying...');
                            updateVideoFrame();
                        } else {
                            console.error('Failed to load frame after ' + maxRetries + ' retries');
                        }
                    }
                    clearTimeout(loadTimeout);
                }, 2000);
                
                // 图片加载成功后更新性能统计
                video.onload = function() {
                    retryCount = 0;
                    lastFrameTime = performance.now();
                };
            }
            
            function stopStreams() {
                if (!isStreaming) {
                    alert('当前没有正在播放的视频流');
                    return;
                }
                
                if (confirm('确定要停止播放吗？')) {
                    fetch('/stop_streams', {
                        method: 'POST'
                    })
                    .then(response => response.json())
                    .then(data => {
                        if (data.success) {
                            resetVideoDisplay();
                        }
                    })
                    .catch(error => {
                        console.error('停止流时出错:', error);
                        resetVideoDisplay();
                    });
                }
            }
            
            function resetVideoDisplay() {
                if (streamInterval) {
                    clearInterval(streamInterval);
                    streamInterval = null;
                }
                
                isStreaming = false;
                currentStreamUrl1 = '';
                currentStreamUrl2 = '';
                retryCount = 0;
                fps = 0;
                
                const video = document.getElementById('video');
                const videoPlaceholder = document.getElementById('videoPlaceholder');
                
                video.style.display = 'none';
                video.src = '';
                videoPlaceholder.style.display = 'flex';
                
                updateStatus('已停止播放', false);
                updatePerformanceInfo(0, 0);
            }
            
            function applyPipSettings() {
                const x = parseInt(document.getElementById('pipX').value);
                const y = parseInt(document.getElementById('pipY').value);
                const width = parseInt(document.getElementById('pipWidth').value);
                const height = parseInt(document.getElementById('pipHeight').value);
                
                fetch('/update_pip_settings', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: `x=${x}&y=${y}&width=${width}&height=${height}`
                })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        // 显示成功提示
                        const applyBtn = document.getElementById('applyPipBtn');
                        const originalText = applyBtn.innerHTML;
                        applyBtn.innerHTML = '<span class="material-icons">check</span><span>设置已应用</span>';
                        applyBtn.style.background = 'linear-gradient(135deg, #28a745 0%, #20c997 100%)';
                        
                        setTimeout(() => {
                            applyBtn.innerHTML = originalText;
                            applyBtn.style.background = 'linear-gradient(135deg, #007bff 0%, #0056b3 100%)';
                        }, 2000);
                    } else {
                        alert('更新失败: ' + data.error);
                    }
                })
                .catch(error => {
                    alert('请求失败: ' + error.message);
                    console.error('Error:', error);
                });
            }
            
            function setPipPreset(preset) {
                let x, y, width = 200, height = 150;
                
                switch(preset) {
                    case 'top-left':
                        x = 20; y = 20;
                        break;
                    case 'top-center':
                        x = 300; y = 20;
                        break;
                    case 'top-right':
                        x = 580; y = 20;
                        break;
                    case 'center-left':
                        x = 20; y = 225;
                        break;
                    case 'center':
                        x = 300; y = 225;
                        break;
                    case 'center-right':
                        x = 580; y = 225;
                        break;
                    case 'bottom-left':
                        x = 20; y = 430;
                        break;
                    case 'bottom-center':
                        x = 300; y = 430;
                        break;
                    case 'bottom-right':
                        x = 580; y = 430;
                        break;
                }
                
                // 更新滑块值
                document.getElementById('pipX').value = x;
                document.getElementById('pipY').value = y;
                document.getElementById('pipWidth').value = width;
                document.getElementById('pipHeight').value = height;
                
                // 更新显示值
                document.getElementById('pipXValue').textContent = x;
                document.getElementById('pipYValue').textContent = y;
                document.getElementById('pipWidthValue').textContent = width;
                document.getElementById('pipHeightValue').textContent = height;
                
                // 更新预览
                updatePipPreview();
                
                // 立即应用
                setTimeout(applyPipSettings, 100);
            }
            
            // 窗口大小变化时更新右侧预览
            window.addEventListener('resize', function() {
                updatePipPreview();
            });
            
            // 输入框回车事件
            document.getElementById('rtspUrl1').addEventListener('keypress', function(e) {
                if (e.key === 'Enter') {
                    startStreams();
                }
            });
            
            document.getElementById('rtspUrl2').addEventListener('keypress', function(e) {
                if (e.key === 'Enter') {
                    startStreams();
                }
            });
            
            // 视频加载错误处理
            document.getElementById('video').addEventListener('error', function(e) {
                console.error('视频加载错误:', e);
                retryCount++;
                if (isStreaming && retryCount < maxRetries) {
                    setTimeout(updateVideoFrame, 100);
                } else {
                    console.error('视频流加载失败');
                }
            });
            
            // 页面加载时检查当前状态
            window.onload = function() {
                fetch('/stream_status')
                    .then(response => response.json())
                    .then(data => {
                        if (data.is_streaming && data.current_url1 && data.current_url2) {
                            document.getElementById('rtspUrl1').value = data.current_url1;
                            document.getElementById('rtspUrl2').value = data.current_url2;
                            currentStreamUrl1 = data.current_url1;
                            currentStreamUrl2 = data.current_url2;
                            isStreaming = true;
                            
                            const video = document.getElementById('video');
                            const videoPlaceholder = document.getElementById('videoPlaceholder');
                            
                            video.style.display = 'block';
                            videoPlaceholder.style.display = 'none';
                            startVideoStream();
                            updateStatus('正在播放', true);
                        }
                        
                        // 获取画中画设置
                        fetch('/get_pip_settings')
                            .then(response => response.json())
                            .then(settings => {
                                if (settings) {
                                    document.getElementById('pipX').value = settings.x;
                                    document.getElementById('pipY').value = settings.y;
                                    document.getElementById('pipWidth').value = settings.width;
                                    document.getElementById('pipHeight').value = settings.height;
                                    
                                    document.getElementById('pipXValue').textContent = settings.x;
                                    document.getElementById('pipYValue').textContent = settings.y;
                                    document.getElementById('pipWidthValue').textContent = settings.width;
                                    document.getElementById('pipHeightValue').textContent = settings.height;
                                    
                                    updatePipPreview();
                                }
                            });
                    })
                    .catch(error => {
                        console.error('检查状态时出错:', error);
                    });
                
                // 初始化预览
                updatePipPreview();
            };
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
    
    DecoderManager decoder_manager;
    
    std::cout << "RTSP Picture-in-Picture Stream Viewer Server" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "Server starting on port " << port << std::endl;
    std::cout << "Open http://localhost:" << port << " in your browser" << std::endl;
    std::cout << std::endl;
    std::cout << "Available endpoints:" << std::endl;
    std::cout << "  GET  /                  - Web interface" << std::endl;
    std::cout << "  GET  /video_frame       - Get mixed video frame" << std::endl;
    std::cout << "  POST /start_streams     - Start both RTSP streams" << std::endl;
    std::cout << "  POST /stop_streams      - Stop both streams" << std::endl;
    std::cout << "  GET  /stream_status     - Get streams status" << std::endl;
    std::cout << "  POST /update_pip_settings - Update PIP settings" << std::endl;
    std::cout << "  GET  /get_pip_settings  - Get current PIP settings" << std::endl;
    std::cout << std::endl;
    std::cout << "Note: Both RTSP streams must be started and stopped together." << std::endl;
    std::cout << std::endl;
    
    // 创建HTTP服务器
    httplib::Server server;
    
    // 主页
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(HTML_PAGE, "text/html");
    });
    
    // 获取混合视频帧
    server.Get("/video_frame", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        std::vector<uint8_t> frame;
        if (decoder_manager.get_mixed_frame(frame)) {
            res.set_content(reinterpret_cast<char*>(frame.data()), frame.size(), "image/jpeg");
        } else {
            // 返回空白图像
            res.set_content(reinterpret_cast<const char*>(BLANK_JPEG), sizeof(BLANK_JPEG), "image/jpeg");
        }
    });
    
    // 同时开始两个流
    server.Post("/start_streams", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        std::string rtsp_url1 = req.get_param_value("rtsp_url1");
        std::string rtsp_url2 = req.get_param_value("rtsp_url2");
        
        if (rtsp_url1.empty() || rtsp_url2.empty()) {
            res.set_content(create_json_response(false, "Both RTSP URLs are required"), "application/json");
            return;
        }
        
        if (decoder_manager.start_streams(rtsp_url1, rtsp_url2)) {
            res.set_content(create_json_response(true), "application/json");
        } else {
            res.set_content(create_json_response(false, "Failed to connect to RTSP streams"), "application/json");
        }
    });
    
    // 同时停止两个流
    server.Post("/stop_streams", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        decoder_manager.stop_all();
        res.set_content(create_json_response(true), "application/json");
    });
    
    // 获取流状态
    server.Get("/stream_status", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        res.set_content(create_status_json(
            decoder_manager.is_streaming(),
            decoder_manager.get_current_url1(),
            decoder_manager.get_current_url2()
        ), "application/json");
    });
    
    // 更新画中画设置
    server.Post("/update_pip_settings", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        std::string x_str = req.get_param_value("x");
        std::string y_str = req.get_param_value("y");
        std::string width_str = req.get_param_value("width");
        std::string height_str = req.get_param_value("height");
        
        if (x_str.empty() || y_str.empty() || width_str.empty() || height_str.empty()) {
            res.set_content(create_json_response(false, "All parameters (x, y, width, height) are required"), "application/json");
            return;
        }
        
        try {
            int x = std::stoi(x_str);
            int y = std::stoi(y_str);
            int width = std::stoi(width_str);
            int height = std::stoi(height_str);
            
            if (decoder_manager.update_pip_settings(x, y, width, height)) {
                res.set_content(create_json_response(true), "application/json");
            } else {
                res.set_content(create_json_response(false, "Invalid PIP settings"), "application/json");
            }
        } catch (const std::exception& e) {
            res.set_content(create_json_response(false, "Invalid parameter format"), "application/json");
        }
    });
    
    // 获取当前画中画设置
    server.Get("/get_pip_settings", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        res.set_content(decoder_manager.get_pip_settings_json(), "application/json");
    });
    
    // 设置HTTP服务器选项
    server.set_keep_alive_max_count(100);
    server.set_read_timeout(10, 0); // 10秒读超时
    server.set_write_timeout(10, 0); // 10秒写超时
    
    // 启动服务器
    std::cout << "Press Ctrl+C to stop the server" << std::endl;
    
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }
    
    return 0;
}
