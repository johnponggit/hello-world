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
#include <algorithm>

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

std::string create_status_json(bool is_streaming, const std::string& current_url = "") {
    std::stringstream ss;
    ss << "{";
    ss << "\"is_streaming\":" << (is_streaming ? "true" : "false");
    if (!current_url.empty()) {
        ss << ",\"current_url\":\"" << current_url << "\"";
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
    
    // 复制构造函数
    YUVFrame(const YUVFrame& other) {
        width = other.width;
        height = other.height;
        format = other.format;
        y_stride = other.y_stride;
        uv_stride = other.uv_stride;
        pts = other.pts;
        timestamp = other.timestamp;
        
        y_plane = other.y_plane;
        u_plane = other.u_plane;
        v_plane = other.v_plane;
    }
    
    // 转换为AVFrame（用于编码）- 修改为const方法
    AVFrame* to_avframe() const {
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
    
    // 编码YUVFrame为JPEG - 修改为接受const引用
    std::vector<uint8_t> encode(const YUVFrame& yuv_frame) {
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
                        lock.unlock();
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

// 模糊处理器
class BlurProcessor {
private:
    std::mutex processor_mutex;
    
    // 模糊区域设置
    struct BlurSettings {
        int x = 100;            // 模糊区域左上角X坐标（对于圆形，是圆心X坐标）
        int y = 100;            // 模糊区域左上角Y坐标（对于圆形，是圆心Y坐标）
        int width = 200;        // 模糊区域宽度（对于圆形，是直径）
        int height = 150;       // 模糊区域高度（对于圆形，是直径）
        int blur_radius = 5;    // 模糊半径
        int border_size = 2;    // 边框大小
        bool enabled = true;    // 是否启用模糊
        std::string shape = "circle"; // 模糊形状: "circle" 或 "rectangle"
    } blur_settings;
    
    // 编码器
    std::unique_ptr<JPEGEncoder> encoder;
    
    // 用于图像缩放
    SwsContext* sws_ctx = nullptr;
    
    // 临时帧
    AVFrame* output_frame = nullptr;
    
    // 高斯模糊内核
    std::vector<float> gaussian_kernel;
    
public:
    BlurProcessor() {
        // 初始化编码器（处理后的画面）
        encoder = std::make_unique<JPEGEncoder>(800, 600);
        
        // 分配临时帧
        output_frame = av_frame_alloc();
        
        // 预计算高斯模糊内核
        generate_gaussian_kernel(5);
    }
    
    ~BlurProcessor() {
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (output_frame) av_frame_free(&output_frame);
    }
    
    // 处理主画面：应用模糊并编码为JPEG
    std::vector<uint8_t> process_and_encode(YUVFrame& input_yuv) {
        std::lock_guard<std::mutex> lock(processor_mutex);
        std::vector<uint8_t> result;
        
        if (input_yuv.empty()) {
            return result;
        }
        
        try {
            // 准备输出帧
            if (!prepare_output_frame(input_yuv)) {
                return result;
            }
            
            // 如果启用模糊，对选定区域应用模糊
            if (blur_settings.enabled) {
                apply_blur_to_frame();
            }
            
            // 编码处理后的帧为JPEG
            YUVFrame output_yuv(output_frame);
            if (!output_yuv.empty()) {
                result = encoder->encode(output_yuv);
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error in process_and_encode: " << e.what() << std::endl;
        }
        
        return result;
    }
    
    // 更新模糊设置
    void update_blur_settings(int x, int y, int width, int height,
                             int blur_radius = 5, int border_size = 2,
                             bool enabled = true, const std::string& shape = "circle") {
        std::lock_guard<std::mutex> lock(processor_mutex);
        blur_settings.x = x;
        blur_settings.y = y;
        blur_settings.width = width;
        blur_settings.height = height;
        blur_settings.blur_radius = blur_radius;
        blur_settings.border_size = border_size;
        blur_settings.enabled = enabled;
        blur_settings.shape = shape;
        
        // 重新生成高斯模糊内核
        generate_gaussian_kernel(blur_radius);
        
        std::cout << "Blur settings updated:" << std::endl;
        std::cout << "  Shape: " << shape << std::endl;
        std::cout << "  Position: (" << x << "," << y << ") " 
                  << width << "x" << height << std::endl;
        std::cout << "  Blur radius: " << blur_radius << std::endl;
        std::cout << "  Enabled: " << (enabled ? "true" : "false") << std::endl;
    }
    
    // 获取当前设置
    BlurSettings get_settings() {
        std::lock_guard<std::mutex> lock(processor_mutex);
        return blur_settings;
    }
    
private:
    // 准备输出帧
    bool prepare_output_frame(YUVFrame& input_yuv) {
        if (input_yuv.empty()) {
            return false;
        }
        
        // 确保输出帧已分配
        if (output_frame->width != 800 || output_frame->height != 600) {
            av_frame_unref(output_frame);
            output_frame->width = 800;
            output_frame->height = 600;
            output_frame->format = AV_PIX_FMT_YUV420P;
            if (av_frame_get_buffer(output_frame, 0) < 0) {
                std::cerr << "Failed to allocate output frame buffer" << std::endl;
                return false;
            }
        }
        
        // 创建或更新缩放上下文
        if (!sws_ctx) {
            sws_ctx = sws_getContext(
                input_yuv.width, input_yuv.height,
                AV_PIX_FMT_YUV420P,
                output_frame->width, output_frame->height,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            if (!sws_ctx) {
                std::cerr << "Failed to create sws context" << std::endl;
                return false;
            }
        }
        
        // 转换为AVFrame
        AVFrame* input_frame = input_yuv.to_avframe();
        if (!input_frame) {
            return false;
        }
        
        // 执行缩放
        sws_scale(sws_ctx, input_frame->data, input_frame->linesize,
                  0, input_frame->height,
                  output_frame->data, output_frame->linesize);
        
        av_frame_free(&input_frame);
        return true;
    }
    
    // 生成高斯模糊内核
    void generate_gaussian_kernel(int radius) {
        int size = 2 * radius + 1;
        gaussian_kernel.resize(size * size);
        
        float sigma = radius / 2.0f;
        float sum = 0.0f;
        
        for (int y = -radius; y <= radius; y++) {
            for (int x = -radius; x <= radius; x++) {
                float value = exp(-(x * x + y * y) / (2 * sigma * sigma));
                gaussian_kernel[(y + radius) * size + (x + radius)] = value;
                sum += value;
            }
        }
        
        // 归一化
        for (int i = 0; i < size * size; i++) {
            gaussian_kernel[i] /= sum;
        }
    }
    
    // 对帧的选定区域应用高斯模糊效果
    void apply_blur_to_frame() {
        if (!output_frame) return;
        
        // 根据形状选择不同的模糊方法
        if (blur_settings.shape == "circle") {
            apply_circular_blur_to_frame();
        } else {
            apply_rectangular_blur_to_frame();
        }
        
        // 添加边框（可选）
        if (blur_settings.border_size > 0) {
            add_border_to_region();
        }
    }
    
    // 对圆形区域应用模糊
    void apply_circular_blur_to_frame() {
        // 计算圆形参数
        int center_x = blur_settings.x + blur_settings.width / 2;
        int center_y = blur_settings.y + blur_settings.height / 2;
        int radius = std::min(blur_settings.width, blur_settings.height) / 2;
        
        // 确保参数在有效范围内
        center_x = std::max(radius, std::min(center_x, output_frame->width - radius));
        center_y = std::max(radius, std::min(center_y, output_frame->height - radius));
        radius = std::min(radius, std::min(center_x, center_y));
        radius = std::min(radius, std::min(output_frame->width - center_x, output_frame->height - center_y));
        
        if (radius <= 0) return;
        
        // 复制原始数据用于模糊处理
        std::vector<uint8_t> y_copy(output_frame->linesize[0] * output_frame->height);
        memcpy(y_copy.data(), output_frame->data[0], y_copy.size());
        
        std::vector<uint8_t> u_copy(output_frame->linesize[1] * (output_frame->height / 2));
        memcpy(u_copy.data(), output_frame->data[1], u_copy.size());
        
        std::vector<uint8_t> v_copy(output_frame->linesize[2] * (output_frame->height / 2));
        memcpy(v_copy.data(), output_frame->data[2], v_copy.size());
        
        // 对Y平面应用高斯模糊（圆形区域）
        apply_circular_gaussian_blur(y_copy.data(), output_frame->data[0], 
                                    output_frame->linesize[0], output_frame->height,
                                    center_x, center_y, radius, blur_settings.blur_radius);
        
        // 对U平面应用高斯模糊（注意YUV420中UV平面尺寸减半）
        int uv_center_x = center_x / 2;
        int uv_center_y = center_y / 2;
        int uv_radius = radius / 2;
        
        apply_circular_gaussian_blur(u_copy.data(), output_frame->data[1],
                                    output_frame->linesize[1], output_frame->height / 2,
                                    uv_center_x, uv_center_y, uv_radius, blur_settings.blur_radius / 2);
        
        apply_circular_gaussian_blur(v_copy.data(), output_frame->data[2],
                                    output_frame->linesize[2], output_frame->height / 2,
                                    uv_center_x, uv_center_y, uv_radius, blur_settings.blur_radius / 2);
    }
    
    // 对矩形区域应用模糊
    void apply_rectangular_blur_to_frame() {
        // 确保模糊区域在有效范围内
        int x = std::max(0, std::min(blur_settings.x, output_frame->width - 1));
        int y = std::max(0, std::min(blur_settings.y, output_frame->height - 1));
        int width = std::min(blur_settings.width, output_frame->width - x);
        int height = std::min(blur_settings.height, output_frame->height - y);
        int radius = std::max(1, blur_settings.blur_radius);
        
        if (width <= 0 || height <= 0) return;
        
        // 复制原始数据用于模糊处理
        std::vector<uint8_t> y_copy(output_frame->linesize[0] * output_frame->height);
        memcpy(y_copy.data(), output_frame->data[0], y_copy.size());
        
        std::vector<uint8_t> u_copy(output_frame->linesize[1] * (output_frame->height / 2));
        memcpy(u_copy.data(), output_frame->data[1], u_copy.size());
        
        std::vector<uint8_t> v_copy(output_frame->linesize[2] * (output_frame->height / 2));
        memcpy(v_copy.data(), output_frame->data[2], v_copy.size());
        
        // 对Y平面应用高斯模糊
        apply_rectangular_gaussian_blur(y_copy.data(), output_frame->data[0], 
                                       output_frame->linesize[0], output_frame->height,
                                       x, y, width, height, radius);
        
        // 对U平面应用高斯模糊（注意YUV420中UV平面尺寸减半）
        int uv_x = x / 2;
        int uv_y = y / 2;
        int uv_width = width / 2;
        int uv_height = height / 2;
        
        apply_rectangular_gaussian_blur(u_copy.data(), output_frame->data[1],
                                       output_frame->linesize[1], output_frame->height / 2,
                                       uv_x, uv_y, uv_width, uv_height, radius / 2);
        
        apply_rectangular_gaussian_blur(v_copy.data(), output_frame->data[2],
                                       output_frame->linesize[2], output_frame->height / 2,
                                       uv_x, uv_y, uv_width, uv_height, radius / 2);
    }
    
    // 应用圆形高斯模糊
    void apply_circular_gaussian_blur(uint8_t* src, uint8_t* dst, int stride, int height,
                                     int center_x, int center_y, int radius, int blur_radius) {
        int kernel_size = 2 * blur_radius + 1;
        
        // 只重新生成内核如果模糊半径改变了
        static int last_blur_radius = 0;
        if (blur_radius != last_blur_radius) {
            generate_gaussian_kernel(blur_radius);
            last_blur_radius = blur_radius;
        }
        
        // 计算圆形区域的边界框
        int start_y = std::max(0, center_y - radius);
        int end_y = std::min(height, center_y + radius);
        int start_x = std::max(0, center_x - radius);
        int end_x = std::min(stride, center_x + radius);
        
        for (int y = start_y; y < end_y; y++) {
            for (int x = start_x; x < end_x; x++) {
                // 检查像素是否在圆形内
                int dx = x - center_x;
                int dy = y - center_y;
                float distance = sqrt(dx * dx + dy * dy);
                
                if (distance > radius) {
                    continue; // 跳过圆形外的像素
                }
                
                float sum = 0.0f;
                float weight_sum = 0.0f;
                
                // 应用卷积核
                for (int ky = -blur_radius; ky <= blur_radius; ky++) {
                    int ny = y + ky;
                    if (ny < 0 || ny >= height) continue;
                    
                    for (int kx = -blur_radius; kx <= blur_radius; kx++) {
                        int nx = x + kx;
                        if (nx < 0 || nx >= stride) continue;
                        
                        // 检查卷积核中的像素是否也在圆形内
                        int ndx = nx - center_x;
                        int ndy = ny - center_y;
                        float ndistance = sqrt(ndx * ndx + ndy * ndy);
                        
                        if (ndistance > radius) {
                            continue; // 跳过圆形外的像素
                        }
                        
                        float weight = gaussian_kernel[(ky + blur_radius) * kernel_size + (kx + blur_radius)];
                        sum += src[ny * stride + nx] * weight;
                        weight_sum += weight;
                    }
                }
                
                if (weight_sum > 0) {
                    // 确保值在有效范围内
                    uint8_t value = static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(sum / weight_sum))));
                    dst[y * stride + x] = value;
                }
            }
        }
    }
    
    // 应用矩形高斯模糊
    void apply_rectangular_gaussian_blur(uint8_t* src, uint8_t* dst, int stride, int height,
                                        int region_x, int region_y, int region_width, int region_height,
                                        int blur_radius) {
        int kernel_size = 2 * blur_radius + 1;
        
        // 只重新生成内核如果模糊半径改变了
        static int last_blur_radius = 0;
        if (blur_radius != last_blur_radius) {
            generate_gaussian_kernel(blur_radius);
            last_blur_radius = blur_radius;
        }
        
        for (int y = region_y; y < region_y + region_height; y++) {
            for (int x = region_x; x < region_x + region_width; x++) {
                if (y >= height || x >= stride) continue;
                
                float sum = 0.0f;
                
                // 应用卷积核
                for (int ky = -blur_radius; ky <= blur_radius; ky++) {
                    int ny = y + ky;
                    if (ny < 0 || ny >= height) continue;
                    
                    for (int kx = -blur_radius; kx <= blur_radius; kx++) {
                        int nx = x + kx;
                        if (nx < 0 || nx >= stride) continue;
                        
                        float weight = gaussian_kernel[(ky + blur_radius) * kernel_size + (kx + blur_radius)];
                        sum += src[ny * stride + nx] * weight;
                    }
                }
                
                // 确保值在有效范围内
                uint8_t value = static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(sum))));
                dst[y * stride + x] = value;
            }
        }
    }
    
    // 为区域添加边框
    void add_border_to_region() {
        if (!output_frame) return;
        
        // 边框颜色（蓝色，YUV值）
        const uint8_t border_y = 41;    // 蓝色对应的Y值
        const uint8_t border_u = 240;   // 蓝色对应的U值
        const uint8_t border_v = 110;   // 蓝色对应的V值
        
        int border_size = std::min(blur_settings.border_size, 5);
        
        if (blur_settings.shape == "circle") {
            // 圆形边框
            int center_x = blur_settings.x + blur_settings.width / 2;
            int center_y = blur_settings.y + blur_settings.height / 2;
            int radius = std::min(blur_settings.width, blur_settings.height) / 2;
            
            // 绘制圆形边框
            for (int angle = 0; angle < 360; angle++) {
                float rad = angle * M_PI / 180.0f;
                int border_x = center_x + static_cast<int>((radius + border_size) * cos(rad));
                int border_y_pos = center_y + static_cast<int>((radius + border_size) * sin(rad));
                
                if (border_x >= 0 && border_x < output_frame->width && 
                    border_y_pos >= 0 && border_y_pos < output_frame->height) {
                    output_frame->data[0][border_y_pos * output_frame->linesize[0] + border_x] = border_y;
                }
            }
        } else {
            // 矩形边框
            int x = blur_settings.x;
            int y = blur_settings.y;
            int width = blur_settings.width;
            int height = blur_settings.height;
            
            // 上边框
            for (int by = 0; by < border_size; by++) {
                int actual_y = y + by;
                if (actual_y >= 0 && actual_y < output_frame->height) {
                    uint8_t* y_line = output_frame->data[0] + actual_y * output_frame->linesize[0];
                    for (int bx = x; bx < x + width; bx++) {
                        if (bx >= 0 && bx < output_frame->width) {
                            y_line[bx] = border_y;
                        }
                    }
                }
            }
            
            // 下边框
            for (int by = 0; by < border_size; by++) {
                int actual_y = y + height - 1 - by;
                if (actual_y >= 0 && actual_y < output_frame->height) {
                    uint8_t* y_line = output_frame->data[0] + actual_y * output_frame->linesize[0];
                    for (int bx = x; bx < x + width; bx++) {
                        if (bx >= 0 && bx < output_frame->width) {
                            y_line[bx] = border_y;
                        }
                    }
                }
            }
            
            // 左边框
            for (int bx = 0; bx < border_size; bx++) {
                int actual_x = x + bx;
                if (actual_x >= 0 && actual_x < output_frame->width) {
                    for (int by = y; by < y + height; by++) {
                        if (by >= 0 && by < output_frame->height) {
                            uint8_t* y_line = output_frame->data[0] + by * output_frame->linesize[0];
                            y_line[actual_x] = border_y;
                        }
                    }
                }
            }
            
            // 右边框
            for (int bx = 0; bx < border_size; bx++) {
                int actual_x = x + width - 1 - bx;
                if (actual_x >= 0 && actual_x < output_frame->width) {
                    for (int by = y; by < y + height; by++) {
                        if (by >= 0 && by < output_frame->height) {
                            uint8_t* y_line = output_frame->data[0] + by * output_frame->linesize[0];
                            y_line[actual_x] = border_y;
                        }
                    }
                }
            }
        }
    }
};

// 全局解码器管理器
class DecoderManager {
private:
    // 一个解码器
    std::unique_ptr<RTSPDecoder> decoder;
    
    // YUV帧缓冲区
    std::unique_ptr<YUVFrameBuffer> frame_buffer;
    
    // 处理后的JPEG帧缓冲区
    std::unique_ptr<std::queue<std::vector<uint8_t>>> jpeg_buffer;
    std::mutex jpeg_buffer_mutex;
    
    // 模糊处理器
    std::unique_ptr<BlurProcessor> blur_processor;
    
    std::mutex manager_mutex;
    std::string current_url;
    
    // 处理线程
    std::thread process_thread;
    std::atomic<bool> processing{false};
    
    // 性能统计
    std::atomic<int> frames_processed{0};
    std::atomic<int64_t> last_stat_time{0};
    
public:
    DecoderManager() 
        : frame_buffer(std::make_unique<YUVFrameBuffer>()),
          jpeg_buffer(std::make_unique<std::queue<std::vector<uint8_t>>>()),
          blur_processor(std::make_unique<BlurProcessor>()) {
        last_stat_time = av_gettime() / 1000;
    }
    
    ~DecoderManager() {
        stop_all();
        if (process_thread.joinable()) {
            processing = false;
            process_thread.join();
        }
    }
    
    // 启动流
    bool start_stream(const std::string& rtsp_url) {
        std::lock_guard<std::mutex> lock(manager_mutex);
        
        // 如果已经在运行，先停止
        if (decoder && decoder->is_running()) {
            stop_all();
        }
        
        if (rtsp_url.empty()) {
            std::cerr << "RTSP URL is required" << std::endl;
            return false;
        }
        
        current_url = rtsp_url;
        
        // 创建解码器
        decoder = std::make_unique<RTSPDecoder>(rtsp_url, *frame_buffer);
        
        // 启动解码器
        bool success = decoder->start();
        
        if (success) {
            std::cout << "Stream started successfully" << std::endl;
            std::cout << "Stream URL: " << rtsp_url << std::endl;
            
            // 启动处理线程
            start_processing();
            return true;
        } else {
            std::cerr << "Failed to start stream" << std::endl;
            decoder.reset();
            current_url.clear();
            return false;
        }
    }
    
    // 停止所有流
    void stop_all() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        if (decoder) {
            decoder->stop();
            decoder.reset();
            frame_buffer->clear();
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
        
        current_url.clear();
        std::cout << "Stream stopped" << std::endl;
    }
    
    // 获取处理后的JPEG帧
    bool get_processed_frame(std::vector<uint8_t>& frame) {
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
    
    // 更新模糊设置
    bool update_blur_settings(int x, int y, int width, int height,
                             int blur_radius = 5, int border_size = 2,
                             bool enabled = true, const std::string& shape = "circle") {
        std::lock_guard<std::mutex> lock(manager_mutex);
        if (!blur_processor) return false;
        
        if (width <= 0 || height <= 0 || width > 800 || height > 600) {
            return false;
        }
        
        if (x < 0 || y < 0 || x + width > 800 || y + height > 600) {
            return false;
        }
        
        blur_processor->update_blur_settings(x, y, width, height,
                                           blur_radius, border_size, enabled, shape);
        return true;
    }
    
    // 获取当前设置
    std::string get_blur_settings_json() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        if (!blur_processor) return "{}";
        
        auto settings = blur_processor->get_settings();
        std::stringstream ss;
        ss << "{";
        ss << "\"x\":" << settings.x << ",";
        ss << "\"y\":" << settings.y << ",";
        ss << "\"width\":" << settings.width << ",";
        ss << "\"height\":" << settings.height << ",";
        ss << "\"blur_radius\":" << settings.blur_radius << ",";
        ss << "\"border_size\":" << settings.border_size << ",";
        ss << "\"enabled\":" << (settings.enabled ? "true" : "false") << ",";
        ss << "\"shape\":\"" << settings.shape << "\"";
        ss << "}";
        return ss.str();
    }
    
    std::string get_current_url() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return current_url;
    }
    
    bool is_streaming() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return decoder && decoder->is_running();
    }
    
private:
    void start_processing() {
        if (!processing) {
            processing = true;
            process_thread = std::thread(&DecoderManager::processing_loop, this);
        }
    }
    
    void processing_loop() {
        std::vector<uint8_t> last_successful_frame;  // 存储最后一帧成功处理的帧
        int64_t last_process_time = 0;
        const int64_t target_process_interval = 33; // 目标处理间隔 33ms (~30fps)
        
        while (processing) {
            int64_t current_time = av_gettime() / 1000; // 毫秒
            
            // 控制处理帧率
            if (current_time - last_process_time < target_process_interval) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            
            last_process_time = current_time;
            
            YUVFrame frame;
            bool has_frame = false;
            
            {
                std::lock_guard<std::mutex> lock(manager_mutex);
                
                // 检查解码器是否在运行
                if (!decoder || !decoder->is_running()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                
                // 尝试获取帧，设置超时
                has_frame = frame_buffer->get_latest(frame, 10);
            }
            
            std::vector<uint8_t> processed_frame;
            
            if (has_frame) {
                // 处理帧：应用模糊
                processed_frame = blur_processor->process_and_encode(frame);
                
                if (!processed_frame.empty()) {
                    last_successful_frame = processed_frame;
                    frames_processed++;
                    
                    std::lock_guard<std::mutex> lock(jpeg_buffer_mutex);
                    if (jpeg_buffer) {
                        // 限制缓冲区大小，只保留最新的一帧
                        while (!jpeg_buffer->empty()) {
                            jpeg_buffer->pop();
                        }
                        jpeg_buffer->push(processed_frame);
                    }
                }
            }
            
            // 如果处理失败但有上一次成功的帧，使用上一次的帧
            if (processed_frame.empty() && !last_successful_frame.empty()) {
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
                std::cout << "处理帧率: " << (frames_processed * 1000 / (now - last_stat_time)) << " fps" << std::endl;
                frames_processed = 0;
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
        <title>RTSP视频流模糊处理器</title>
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
                grid-template-columns: 2fr 1fr;
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
                cursor: crosshair;
            }
            
            #video {
                width: 100%;
                height: 100%;
                object-fit: contain;
                display: block;
            }
            
            .click-indicator {
                position: absolute;
                width: 20px;
                height: 20px;
                border-radius: 50%;
                background: rgba(255, 107, 107, 0.8);
                border: 2px solid white;
                pointer-events: none;
                transform: translate(-50%, -50%);
                animation: pulse 1.5s ease-out;
                display: none;
                z-index: 10;
            }
            
            @keyframes pulse {
                0% { transform: translate(-50%, -50%) scale(1); opacity: 1; }
                70% { transform: translate(-50%, -50%) scale(2); opacity: 0.7; }
                100% { transform: translate(-50%, -50%) scale(2.5); opacity: 0; }
            }
            
            .click-hint {
                position: absolute;
                bottom: 15px;
                left: 0;
                right: 0;
                text-align: center;
                color: white;
                font-size: 14px;
                background: rgba(0, 0, 0, 0.6);
                padding: 8px 15px;
                border-radius: 20px;
                margin: 0 auto;
                width: fit-content;
                display: none;
                z-index: 5;
                pointer-events: none;
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
            
            .status-container {
                margin-top: 20px;
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
                animation: pulseIndicator 1.5s infinite;
            }
            
            @keyframes pulseIndicator {
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
            
            .blur-controls {
                background: linear-gradient(135deg, #f8f9fa 0%, #e9ecef 100%);
                padding: 25px;
                border-radius: 15px;
                border: 2px solid #4b6cb7;
                box-shadow: 0 5px 15px rgba(75, 108, 183, 0.1);
            }
            
            .blur-controls .panel-title {
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
            
            .shape-selector {
                display: flex;
                gap: 15px;
                margin: 15px 0;
            }
            
            .shape-option {
                flex: 1;
                padding: 10px;
                border: 2px solid #dee2e6;
                border-radius: 10px;
                text-align: center;
                cursor: pointer;
                transition: all 0.3s ease;
                background: white;
            }
            
            .shape-option:hover {
                border-color: #4b6cb7;
                background: #f8f9fa;
            }
            
            .shape-option.selected {
                border-color: #4b6cb7;
                background: #4b6cb7;
                color: white;
            }
            
            .shape-icon {
                font-size: 24px;
                margin-bottom: 5px;
                display: block;
            }
            
            .checkbox-group {
                display: flex;
                align-items: center;
                gap: 10px;
                margin: 10px 0;
            }
            
            .checkbox-group input[type="checkbox"] {
                width: 18px;
                height: 18px;
            }
            
            .checkbox-group label {
                color: #495057;
                font-weight: 500;
                cursor: pointer;
                display: flex;
                align-items: center;
                gap: 5px;
            }
            
            .preview-container {
                background: #2c3e50;
                border-radius: 10px;
                padding: 15px;
                margin-top: 20px;
                position: relative;
                overflow: hidden;
                height: 200px;
            }
            
            .preview-title {
                color: white;
                margin-bottom: 10px;
                font-size: 14px;
                text-align: center;
            }
            
            .preview-area {
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
            
            .preview-blur {
                position: absolute;
                background: linear-gradient(135deg, #3498db 0%, #2c3e50 100%);
                border: 2px solid #ff6b6b;
                box-shadow: 0 0 0 1px rgba(0, 0, 0, 0.3);
                transition: all 0.3s ease;
                filter: blur(3px);
            }
            
            .preview-blur.circular {
                border-radius: 50%;
            }
            
            .preview-label {
                position: absolute;
                color: white;
                font-size: 10px;
                padding: 2px 4px;
                background: rgba(0, 0, 0, 0.7);
                border-radius: 3px;
                white-space: nowrap;
            }
            
            .info-panel {
                margin-top: 30px;
                padding: 25px;
                background: linear-gradient(135deg, #f8f9fa 0%, #e9ecef 100%);
                border-radius: 15px;
                border-left: 5px solid #4b6cb7;
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
            
            .auto-apply-note {
                font-size: 12px;
                color: #28a745;
                padding: 10px;
                background: #e7f5ff;
                border-radius: 8px;
                text-align: center;
                margin-top: 10px;
                display: flex;
                align-items: center;
                justify-content: center;
                gap: 5px;
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
                <h1>🎥 RTSP视频流模糊处理器</h1>
                <p>单路视频流 • 点击画面定位模糊区域 • 自动应用设置 • 实时预览</p>
            </div>
            
            <div class="content">
                <div class="main-content">
                    <div class="video-section">
                        <div class="video-title">
                            <span class="material-icons-outlined">videocam</span>
                            <span>视频预览画面</span>
                            <span style="font-size: 14px; color: #ff6b6b; margin-left: auto;">
                                <span class="material-icons-outlined" style="font-size: 16px;">touch_app</span>
                                点击画面定位模糊区域 (自动应用设置)
                            </span>
                        </div>
                        <div class="video-container" id="videoContainer">
                            <div id="loading" class="loading-overlay">
                                <div class="spinner"></div>
                                <div>正在连接视频流...</div>
                            </div>
                            <div id="videoPlaceholder" class="video-placeholder">
                                <div style="margin-bottom: 20px;">
                                    <span class="material-icons-outlined" style="font-size: 48px;">videocam_off</span>
                                </div>
                                <h3 style="margin-bottom: 10px;">等待视频流</h3>
                                <p>请输入RTSP地址并开始播放</p>
                            </div>
                            <img id="video" src="" style="display: none;">
                            <div id="clickIndicator" class="click-indicator"></div>
                            <div id="clickHint" class="click-hint">点击画面设置模糊区域中心位置 (自动应用)</div>
                        </div>
                        
                        <div class="status-container">
                            <div class="status-box">
                                <div class="status-title">
                                    <span class="material-icons-outlined">info</span>
                                    <span>播放状态</span>
                                </div>
                                <div id="statusText">未连接</div>
                                <div class="status-indicator" id="statusIndicator"></div>
                                <div id="currentStream" class="stream-url">
                                    <strong>当前流:</strong> <span id="streamUrl">未连接</span>
                                </div>
                            </div>
                        </div>
                    </div>
                    
                    <div class="info-panel">
                        <h3><span class="material-icons-outlined">help_outline</span> 使用说明</h3>
                        <ul class="info-list">
                            <li>输入RTSP地址开始播放视频流</li>
                            <li><strong>点击视频画面</strong>来定位模糊区域的中心位置 (自动应用设置)</li>
                            <li>在右侧控制面板中调整模糊区域的大小和模糊程度</li>
                            <li>选择模糊形状：圆形或矩形</li>
                            <li>调整模糊半径来控制模糊的程度</li>
                            <li>可以启用或禁用模糊效果</li>
                            <li>所有设置修改后立即生效</li>
                            <li>右侧提供模糊区域的可视化预览</li>
                            <li>使用高斯模糊算法实现平滑的模糊效果</li>
                        </ul>
                    </div>
                </div>
                
                <div class="right-panel">
                    <div class="blur-controls">
                        <div class="panel-title">
                            <span class="material-icons">blur_on</span>
                            <span>模糊控制</span>
                        </div>
                        
                        <div class="preview-container">
                            <div class="preview-title">模糊区域预览</div>
                            <div class="preview-area">
                                <div class="preview-main"></div>
                                <div id="previewBlur" class="preview-blur"></div>
                                <div class="preview-label" style="top: 5px; left: 5px;">主画面</div>
                            </div>
                        </div>
                        
                        <div class="control-section">
                            <div class="control-section-title">
                                <span class="material-icons-outlined">crop_square</span>
                                <span>区域设置</span>
                            </div>
                            
                            <div class="shape-selector">
                                <div id="shapeCircle" class="shape-option selected" onclick="selectShape('circle')">
                                    <span class="shape-icon">●</span>
                                    <span>圆形</span>
                                </div>
                                <div id="shapeRectangle" class="shape-option" onclick="selectShape('rectangle')">
                                    <span class="shape-icon">■</span>
                                    <span>矩形</span>
                                </div>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">horizontal_distribute</span>
                                        <span>位置 X:</span>
                                    </label>
                                    <input type="range" id="blurX" min="0" max="600" value="100" step="10">
                                    <span id="blurXValue" class="slider-value">100</span>
                                </div>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">vertical_distribute</span>
                                        <span>位置 Y:</span>
                                    </label>
                                    <input type="range" id="blurY" min="0" max="450" value="100" step="10">
                                    <span id="blurYValue" class="slider-value">100</span>
                                </div>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">width_normal</span>
                                        <span>宽度:</span>
                                    </label>
                                    <input type="range" id="blurWidth" min="50" max="400" value="200" step="10">
                                    <span id="blurWidthValue" class="slider-value">200</span>
                                </div>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">height</span>
                                        <span>高度:</span>
                                    </label>
                                    <input type="range" id="blurHeight" min="50" max="300" value="150" step="10">
                                    <span id="blurHeightValue" class="slider-value">150</span>
                                </div>
                            </div>
                            
                            <div class="slider-group">
                                <div class="slider-container">
                                    <label>
                                        <span class="material-icons-outlined" style="font-size: 18px;">blur_on</span>
                                        <span>模糊半径:</span>
                                    </label>
                                    <input type="range" id="blurRadius" min="1" max="10" value="5" step="1">
                                    <span id="blurRadiusValue" class="slider-value">5</span>
                                </div>
                            </div>
                            
                            <div class="checkbox-group">
                                <input type="checkbox" id="blurEnabled" checked>
                                <label for="blurEnabled">
                                    <span class="material-icons-outlined" style="font-size: 18px;">visibility</span>
                                    <span>启用模糊</span>
                                </label>
                            </div>
                        </div>
                        
                        <div class="auto-apply-note">
                            <span class="material-icons-outlined" style="font-size: 14px;">autorenew</span>
                            <span>所有设置已启用自动应用</span>
                        </div>
                    </div>
                    
                    <div class="control-panel">
                        <div class="panel-title">
                            <span class="material-icons">settings</span>
                            <span>视频流控制</span>
                        </div>
                        
                        <div class="input-group">
                            <label>
                                <span class="material-icons-outlined" style="font-size: 18px;">videocam</span>
                                <span>RTSP地址</span>
                            </label>
                            <input type="text" 
                                   id="rtspUrl" 
                                   class="rtsp-input" 
                                   placeholder="rtsp://username:password@ip:port/path"
                                   value="rtsp://192.168.1.100:554/stream">
                        </div>
                        
                        <button id="startBtn" class="btn btn-start" onclick="startStream()">
                            <span class="material-icons">play_arrow</span>
                            <span>开始播放</span>
                        </button>
                        <button id="stopBtn" class="btn btn-stop" onclick="stopStream()">
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
            let currentStreamUrl = '';
            let retryCount = 0;
            const maxRetries = 3;
            let lastFrameTime = 0;
            let frameCount = 0;
            let fps = 0;
            let currentShape = 'circle';
            let videoDisplayWidth = 0;
            let videoDisplayHeight = 0;
            let videoOffsetX = 0;
            let videoOffsetY = 0;
            let applyDebounceTimer = null; // 防抖定时器
            let pageJustLoaded = true; // 标记页面是否刚刚加载
            
            // 形状选择
            function selectShape(shape) {
                currentShape = shape;
                
                // 更新UI
                document.getElementById('shapeCircle').classList.remove('selected');
                document.getElementById('shapeRectangle').classList.remove('selected');
                document.getElementById('shape' + shape.charAt(0).toUpperCase() + shape.slice(1)).classList.add('selected');
                
                // 更新预览并自动应用
                updatePreviewAndApply();
            }
            
            // 初始化滑块事件 - 修改为自动应用
            const sliders = ['blurX', 'blurY', 'blurWidth', 'blurHeight', 'blurRadius'];
            sliders.forEach(sliderId => {
                const slider = document.getElementById(sliderId);
                const valueDisplay = document.getElementById(sliderId + 'Value');
                
                slider.addEventListener('input', function(e) {
                    valueDisplay.textContent = e.target.value;
                    updatePreview();
                    // 滑块拖动时实时更新预览，但不立即应用（使用防抖）
                    debounceApplySettings();
                });
                
                slider.addEventListener('change', function(e) {
                    valueDisplay.textContent = e.target.value;
                    updatePreview();
                    // 滑块释放时立即应用设置
                    applyBlurSettings();
                });
            });
            
            // 初始化复选框事件 - 修改为自动应用
            document.getElementById('blurEnabled').addEventListener('change', function() {
                updatePreview();
                // 复选框改变时立即应用
                debounceApplySettings(300);
            });
            
            // 防抖函数 - 防止频繁发送请求
            function debounceApplySettings(delay = 300) {
                if (applyDebounceTimer) {
                    clearTimeout(applyDebounceTimer);
                }
                applyDebounceTimer = setTimeout(() => {
                    applyBlurSettings();
                }, delay);
            }
            
            // 更新视频显示尺寸
            function updateVideoDisplaySize() {
                const video = document.getElementById('video');
                const videoContainer = document.getElementById('videoContainer');
                
                if (!video || !videoContainer) return;
                
                // 获取视频容器的尺寸
                const containerWidth = videoContainer.clientWidth;
                const containerHeight = videoContainer.clientHeight;
                
                // 获取视频的原始尺寸
                const videoWidth = video.videoWidth || 800;
                const videoHeight = video.videoHeight || 600;
                
                // 计算缩放比例
                const widthRatio = containerWidth / videoWidth;
                const heightRatio = containerHeight / videoHeight;
                const scale = Math.min(widthRatio, heightRatio);
                
                // 计算显示尺寸
                videoDisplayWidth = videoWidth * scale;
                videoDisplayHeight = videoHeight * scale;
                
                // 计算偏移量（居中显示）
                videoOffsetX = (containerWidth - videoDisplayWidth) / 2;
                videoOffsetY = (containerHeight - videoDisplayHeight) / 2;
            }
            
            // 处理视频点击事件 - 修改为自动应用
            function handleVideoClick(event) {
                if (!isStreaming) {
                    alert('请先开始播放视频流');
                    return;
                }
                
                const videoContainer = document.getElementById('videoContainer');
                const rect = videoContainer.getBoundingClientRect();
                
                // 计算点击位置相对于视频容器
                const clickX = event.clientX - rect.left;
                const clickY = event.clientY - rect.top;
                
                // 显示点击指示器
                showClickIndicator(clickX, clickY);
                
                // 计算点击位置在原始视频坐标（800x600）中的位置
                const originalX = Math.round((clickX - videoOffsetX) * (800 / videoDisplayWidth));
                const originalY = Math.round((clickY - videoOffsetY) * (600 / videoDisplayHeight));
                
                // 确保坐标在有效范围内
                const validX = Math.max(0, Math.min(800, originalX));
                const validY = Math.max(0, Math.min(600, originalY));
                
                // 根据形状计算新的位置
                const width = parseInt(document.getElementById('blurWidth').value);
                const height = parseInt(document.getElementById('blurHeight').value);
                
                let newX, newY;
                
                if (currentShape === 'circle') {
                    // 对于圆形，点击位置是圆心
                    // 左上角位置 = 圆心位置 - 半径
                    const radius = Math.min(width, height) / 2;
                    newX = validX - radius;
                    newY = validY - radius;
                } else {
                    // 对于矩形，点击位置是矩形中心
                    // 左上角位置 = 中心位置 - 宽度/2 和 高度/2
                    newX = validX - width / 2;
                    newY = validY - height / 2;
                }
                
                // 确保位置在有效范围内
                newX = Math.max(0, Math.min(800 - width, newX));
                newY = Math.max(0, Math.min(600 - height, newY));
                
                // 更新滑块值
                document.getElementById('blurX').value = Math.round(newX);
                document.getElementById('blurY').value = Math.round(newY);
                document.getElementById('blurXValue').textContent = Math.round(newX);
                document.getElementById('blurYValue').textContent = Math.round(newY);
                
                // 更新预览
                updatePreview();
                
                // 显示提示信息
                showClickMessage(`已设置模糊区域到 (${Math.round(newX)}, ${Math.round(newY)})，正在自动应用...`);
                
                // 自动应用设置（无需手动点击按钮）
                applyBlurSettings();
            }
            
            // 显示点击指示器
            function showClickIndicator(x, y) {
                const indicator = document.getElementById('clickIndicator');
                indicator.style.left = x + 'px';
                indicator.style.top = y + 'px';
                indicator.style.display = 'block';
                
                // 重置动画
                indicator.style.animation = 'none';
                setTimeout(() => {
                    indicator.style.animation = 'pulse 1.5s ease-out';
                }, 10);
                
                // 3秒后隐藏指示器
                setTimeout(() => {
                    indicator.style.display = 'none';
                }, 1500);
            }
            
            // 显示点击提示消息
            function showClickMessage(message) {
                const hint = document.getElementById('clickHint');
                hint.textContent = message;
                hint.style.display = 'block';
                
                // 2秒后隐藏提示
                setTimeout(() => {
                    hint.style.display = 'none';
                }, 2000);
            }
            
            // 更新预览并自动应用设置
            function updatePreviewAndApply() {
                updatePreview();
                // 添加一个小的延迟，确保UI已经更新
                setTimeout(applyBlurSettings, 100);
            }
            
            // 更新预览
            function updatePreview() {
                const blurX = parseInt(document.getElementById('blurX').value);
                const blurY = parseInt(document.getElementById('blurY').value);
                const blurWidth = parseInt(document.getElementById('blurWidth').value);
                const blurHeight = parseInt(document.getElementById('blurHeight').value);
                const blurRadius = parseInt(document.getElementById('blurRadius').value);
                const blurEnabled = document.getElementById('blurEnabled').checked;
                const shape = currentShape;
                
                const previewArea = document.querySelector('.preview-area');
                const previewBlur = document.getElementById('previewBlur');
                
                // 计算预览尺寸比例
                const containerWidth = previewArea.clientWidth;
                const containerHeight = previewArea.clientHeight;
                
                // 800x600 是实际视频尺寸，预览区域是等比例缩小
                const scaleX = containerWidth / 800;
                const scaleY = containerHeight / 600;
                
                // 更新模糊区域预览
                if (blurEnabled) {
                    const previewBlurX = blurX * scaleX;
                    const previewBlurY = blurY * scaleY;
                    const previewBlurWidth = blurWidth * scaleX;
                    const previewBlurHeight = blurHeight * scaleY;
                    
                    previewBlur.style.left = previewBlurX + 'px';
                    previewBlur.style.top = previewBlurY + 'px';
                    
                    if (shape === 'circle') {
                        // 圆形：使用宽度和高度中较小的作为直径
                        const diameter = Math.min(previewBlurWidth, previewBlurHeight);
                        previewBlur.style.width = diameter + 'px';
                        previewBlur.style.height = diameter + 'px';
                        previewBlur.style.borderRadius = '50%';
                        previewBlur.classList.add('circular');
                    } else {
                        // 矩形
                        previewBlur.style.width = previewBlurWidth + 'px';
                        previewBlur.style.height = previewBlurHeight + 'px';
                        previewBlur.style.borderRadius = '0';
                        previewBlur.classList.remove('circular');
                    }
                    
                    previewBlur.style.display = 'block';
                    
                    // 调整模糊效果预览
                    const blurAmount = blurRadius / 10;
                    previewBlur.style.filter = `blur(${blurAmount * 3}px)`;
                    
                    // 更新模糊区域标签
                    let blurLabel = document.getElementById('blurPreviewLabel');
                    if (!blurLabel) {
                        blurLabel = document.createElement('div');
                        blurLabel.id = 'blurPreviewLabel';
                        blurLabel.className = 'preview-label';
                        previewArea.appendChild(blurLabel);
                    }
                    blurLabel.textContent = `${shape === 'circle' ? '圆形' : '矩形'} ${blurWidth}×${blurHeight}`;
                    blurLabel.style.left = (previewBlurX + 5) + 'px';
                    blurLabel.style.top = (previewBlurY + 5) + 'px';
                    blurLabel.style.display = 'block';
                } else {
                    previewBlur.style.display = 'none';
                    const blurLabel = document.getElementById('blurPreviewLabel');
                    if (blurLabel) blurLabel.style.display = 'none';
                }
            }
            
            function updateStatus(status, isActive = false) {
                const statusText = document.getElementById('statusText');
                const statusIndicator = document.getElementById('statusIndicator');
                const streamUrl = document.getElementById('streamUrl');
                
                statusText.textContent = status;
                statusIndicator.className = 'status-indicator' + (isActive ? ' active' : '');
                
                if (currentStreamUrl) {
                    streamUrl.textContent = currentStreamUrl;
                } else {
                    streamUrl.textContent = '未连接';
                }
            }
            
            function updatePerformanceInfo() {
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
            
            function startStream() {
                const rtspUrl = document.getElementById('rtspUrl').value.trim();
                
                if (!rtspUrl) {
                    alert('请输入RTSP地址');
                    return;
                }
                
                if (isStreaming) {
                    alert('已经在播放中，请先停止当前播放');
                    return;
                }
                
                showLoading(true);
                updateStatus('正在连接视频流...', true);
                
                fetch('/start_stream', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: 'rtsp_url=' + encodeURIComponent(rtspUrl)
                })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        currentStreamUrl = rtspUrl;
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
                        
                        // 显示点击提示
                        const hint = document.getElementById('clickHint');
                        hint.style.display = 'block';
                        
                        // 页面加载后第一次启动流时，自动应用设置
                        if (pageJustLoaded) {
                            pageJustLoaded = false;
                            setTimeout(() => {
                                applyBlurSettings();
                            }, 500);
                        }
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
                        updatePerformanceInfo();
                    }
                }, 100);
                
                // 每500ms更新视频显示尺寸
                setInterval(() => {
                    if (isStreaming) {
                        updateVideoDisplaySize();
                    }
                }, 500);
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
                    
                    // 更新视频显示尺寸
                    updateVideoDisplaySize();
                };
            }
            
            function stopStream() {
                if (!isStreaming) {
                    alert('当前没有正在播放的视频流');
                    return;
                }
                
                if (confirm('确定要停止播放吗？')) {
                    fetch('/stop_stream', {
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
                currentStreamUrl = '';
                retryCount = 0;
                fps = 0;
                
                const video = document.getElementById('video');
                const videoPlaceholder = document.getElementById('videoPlaceholder');
                const hint = document.getElementById('clickHint');
                
                video.style.display = 'none';
                video.src = '';
                videoPlaceholder.style.display = 'flex';
                hint.style.display = 'none';
                
                updateStatus('已停止播放', false);
                updatePerformanceInfo(0, 0);
            }
            
            // 应用模糊设置 - 修改为显示状态提示
            function applyBlurSettings() {
                const blurX = parseInt(document.getElementById('blurX').value);
                const blurY = parseInt(document.getElementById('blurY').value);
                const blurWidth = parseInt(document.getElementById('blurWidth').value);
                const blurHeight = parseInt(document.getElementById('blurHeight').value);
                const blurRadius = parseInt(document.getElementById('blurRadius').value);
                const blurEnabled = document.getElementById('blurEnabled').checked;
                const borderSize = 2; // 固定边框大小
                const shape = currentShape;
                
                // 显示应用中的状态
                const autoApplyNote = document.querySelector('.auto-apply-note');
                const originalText = autoApplyNote.innerHTML;
                autoApplyNote.innerHTML = '<span class="material-icons-outlined" style="font-size: 14px;">sync</span> <span>正在应用设置...</span>';
                autoApplyNote.style.color = '#ff9800';
                
                fetch('/update_blur_settings', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: `x=${blurX}&y=${blurY}&width=${blurWidth}&height=${blurHeight}&blur_radius=${blurRadius}&border_size=${borderSize}&enabled=${blurEnabled}&shape=${shape}`
                })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        // 显示成功提示
                        autoApplyNote.innerHTML = '<span class="material-icons-outlined" style="font-size: 14px;">check_circle</span> <span>设置已自动应用</span>';
                        autoApplyNote.style.color = '#28a745';
                        
                        setTimeout(() => {
                            autoApplyNote.innerHTML = '<span class="material-icons-outlined" style="font-size: 14px;">autorenew</span> <span>所有设置已启用自动应用</span>';
                            autoApplyNote.style.color = '#28a745';
                        }, 2000);
                    } else {
                        // 显示错误提示
                        autoApplyNote.innerHTML = '<span class="material-icons-outlined" style="font-size: 14px;">error</span> <span>应用失败: ' + data.error + '</span>';
                        autoApplyNote.style.color = '#f44336';
                        
                        setTimeout(() => {
                            autoApplyNote.innerHTML = originalText;
                            autoApplyNote.style.color = '#28a745';
                        }, 3000);
                    }
                })
                .catch(error => {
                    // 显示网络错误提示
                    autoApplyNote.innerHTML = '<span class="material-icons-outlined" style="font-size: 14px;">wifi_off</span> <span>网络错误，请重试</span>';
                    autoApplyNote.style.color = '#f44336';
                    
                    setTimeout(() => {
                        autoApplyNote.innerHTML = originalText;
                        autoApplyNote.style.color = '#28a745';
                    }, 3000);
                    
                    console.error('Error:', error);
                });
            }
            
            // 窗口大小变化时更新预览和视频尺寸
            window.addEventListener('resize', function() {
                updatePreview();
                updateVideoDisplaySize();
            });
            
            // 输入框回车事件
            document.getElementById('rtspUrl').addEventListener('keypress', function(e) {
                if (e.key === 'Enter') {
                    startStream();
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
            
            // 视频点击事件
            document.getElementById('videoContainer').addEventListener('click', handleVideoClick);
            
            // 页面加载时检查当前状态
            window.onload = function() {
                fetch('/stream_status')
                    .then(response => response.json())
                    .then(data => {
                        if (data.is_streaming && data.current_url) {
                            document.getElementById('rtspUrl').value = data.current_url;
                            currentStreamUrl = data.current_url;
                            isStreaming = true;
                            
                            const video = document.getElementById('video');
                            const videoPlaceholder = document.getElementById('videoPlaceholder');
                            const hint = document.getElementById('clickHint');
                            
                            video.style.display = 'block';
                            videoPlaceholder.style.display = 'none';
                            hint.style.display = 'block';
                            
                            startVideoStream();
                            updateStatus('正在播放', true);
                        }
                        
                        // 获取模糊设置
                        fetch('/get_blur_settings')
                            .then(response => response.json())
                            .then(settings => {
                                if (settings) {
                                    document.getElementById('blurX').value = settings.x;
                                    document.getElementById('blurY').value = settings.y;
                                    document.getElementById('blurWidth').value = settings.width;
                                    document.getElementById('blurHeight').value = settings.height;
                                    document.getElementById('blurRadius').value = settings.blur_radius;
                                    document.getElementById('blurEnabled').checked = settings.enabled;
                                    
                                    document.getElementById('blurXValue').textContent = settings.x;
                                    document.getElementById('blurYValue').textContent = settings.y;
                                    document.getElementById('blurWidthValue').textContent = settings.width;
                                    document.getElementById('blurHeightValue').textContent = settings.height;
                                    document.getElementById('blurRadiusValue').textContent = settings.blur_radius;
                                    
                                    // 设置形状
                                    selectShape(settings.shape);
                                    
                                    updatePreview();
                                    
                                    // 页面加载后，如果有流在播放，自动应用设置
                                    if (isStreaming) {
                                        setTimeout(() => {
                                            applyBlurSettings();
                                            pageJustLoaded = false;
                                        }, 1000);
                                    }
                                }
                            });
                    })
                    .catch(error => {
                        console.error('检查状态时出错:', error);
                    });
                
                // 初始化预览
                updatePreview();
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
    
    std::cout << "RTSP视频流模糊处理器服务器" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "服务器启动在端口 " << port << std::endl;
    std::cout << "在浏览器中打开 http://localhost:" << port << std::endl;
    std::cout << std::endl;
    std::cout << "可用端点:" << std::endl;
    std::cout << "  GET  /                      - 网页界面" << std::endl;
    std::cout << "  GET  /video_frame           - 获取处理后的视频帧" << std::endl;
    std::cout << "  POST /start_stream          - 启动RTSP流" << std::endl;
    std::cout << "  POST /stop_stream           - 停止流" << std::endl;
    std::cout << "  GET  /stream_status         - 获取流状态" << std::endl;
    std::cout << "  POST /update_blur_settings  - 更新模糊设置" << std::endl;
    std::cout << "  GET  /get_blur_settings     - 获取当前模糊设置" << std::endl;
    std::cout << std::endl;
    std::cout << "功能说明:" << std::endl;
    std::cout << "  1. 只使用一路RTSP流" << std::endl;
    std::cout << "  2. 支持圆形和矩形两种模糊区域" << std::endl;
    std::cout << "  3. 支持鼠标点击视频画面定位模糊区域" << std::endl;
    std::cout << "  4. 页面刷新时会自动应用当前设置" << std::endl;
    std::cout << "  5. 可实时调整模糊区域的位置、大小和模糊半径" << std::endl;
    std::cout << "  6. 可启用或禁用模糊效果" << std::endl;
    std::cout << "  7. 使用高斯模糊算法实现平滑的模糊效果" << std::endl;
    std::cout << std::endl;
    
    // 创建HTTP服务器
    httplib::Server server;
    
    // 主页
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(HTML_PAGE, "text/html");
    });
    
    // 获取处理后的视频帧
    server.Get("/video_frame", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        std::vector<uint8_t> frame;
        if (decoder_manager.get_processed_frame(frame)) {
            res.set_content(reinterpret_cast<char*>(frame.data()), frame.size(), "image/jpeg");
        } else {
            // 返回空白图像
            res.set_content(reinterpret_cast<const char*>(BLANK_JPEG), sizeof(BLANK_JPEG), "image/jpeg");
        }
    });
    
    // 启动流
    server.Post("/start_stream", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        std::string rtsp_url = req.get_param_value("rtsp_url");
        
        if (rtsp_url.empty()) {
            res.set_content(create_json_response(false, "RTSP URL is required"), "application/json");
            return;
        }
        
        if (decoder_manager.start_stream(rtsp_url)) {
            res.set_content(create_json_response(true), "application/json");
        } else {
            res.set_content(create_json_response(false, "Failed to connect to RTSP stream"), "application/json");
        }
    });
    
    // 停止流
    server.Post("/stop_stream", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        decoder_manager.stop_all();
        res.set_content(create_json_response(true), "application/json");
    });
    
    // 获取流状态
    server.Get("/stream_status", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        res.set_content(create_status_json(
            decoder_manager.is_streaming(),
            decoder_manager.get_current_url()
        ), "application/json");
    });
    
    // 更新模糊设置
    server.Post("/update_blur_settings", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        std::string x_str = req.get_param_value("x");
        std::string y_str = req.get_param_value("y");
        std::string width_str = req.get_param_value("width");
        std::string height_str = req.get_param_value("height");
        std::string blur_radius_str = req.get_param_value("blur_radius");
        std::string border_size_str = req.get_param_value("border_size");
        std::string enabled_str = req.get_param_value("enabled");
        std::string shape_str = req.get_param_value("shape");
        
        if (x_str.empty() || y_str.empty() || width_str.empty() || height_str.empty()) {
            res.set_content(create_json_response(false, "Position and size parameters are required"), "application/json");
            return;
        }
        
        try {
            int x = std::stoi(x_str);
            int y = std::stoi(y_str);
            int width = std::stoi(width_str);
            int height = std::stoi(height_str);
            
            int blur_radius = blur_radius_str.empty() ? 5 : std::stoi(blur_radius_str);
            int border_size = border_size_str.empty() ? 2 : std::stoi(border_size_str);
            bool enabled = enabled_str.empty() ? true : (enabled_str == "true");
            std::string shape = shape_str.empty() ? "circle" : shape_str;
            
            if (decoder_manager.update_blur_settings(x, y, width, height,
                                                   blur_radius, border_size, enabled, shape)) {
                res.set_content(create_json_response(true), "application/json");
            } else {
                res.set_content(create_json_response(false, "Invalid blur settings"), "application/json");
            }
        } catch (const std::exception& e) {
            res.set_content(create_json_response(false, "Invalid parameter format"), "application/json");
        }
    });
    
    // 获取当前模糊设置
    server.Get("/get_blur_settings", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        res.set_content(decoder_manager.get_blur_settings_json(), "application/json");
    });
    
    // 设置HTTP服务器选项
    server.set_keep_alive_max_count(100);
    server.set_read_timeout(10, 0); // 10秒读超时
    server.set_write_timeout(10, 0); // 10秒写超时
    
    // 启动服务器
    std::cout << "按 Ctrl+C 停止服务器" << std::endl;
    
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "无法在端口 " << port << " 启动服务器" << std::endl;
        return 1;
    }
    
    return 0;
}