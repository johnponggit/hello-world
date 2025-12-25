#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <vector>
#include <memory>
#include <sstream>

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

// JPEG编码器类
class JPEGEncoder {
private:
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    
public:
    JPEGEncoder() {
        // 初始化JPEG编码器
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) {
            throw std::runtime_error("JPEG codec not found");
        }
        
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            throw std::runtime_error("Could not allocate codec context");
        }
        
        codec_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        codec_ctx->width = 640;
        codec_ctx->height = 480;
        codec_ctx->time_base = {1, 25};
        codec_ctx->framerate = {25, 1};
        
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
    
    std::vector<uint8_t> encode(AVFrame* input_frame) {
        std::vector<uint8_t> jpeg_data;
        
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
        av_frame_get_buffer(frame, 0);
        
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

// 视频帧缓冲区
class FrameBuffer {
private:
    std::queue<std::vector<uint8_t>> frames;
    std::mutex mtx;
    size_t max_size = 10; // 最大缓存帧数
    
public:
    void push(const std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock(mtx);
        if (frames.size() >= max_size) {
            frames.pop();
        }
        frames.push(frame);
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
    
    FrameBuffer& frame_buffer;
    JPEGEncoder jpeg_encoder;
    std::mutex decoder_mutex;
    
public:
    RTSPDecoder(const std::string& url, FrameBuffer& buffer)
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
    
    void update_url(const std::string& new_url) {
        if (new_url != rtsp_url) {
            stop();
            rtsp_url = new_url;
        }
    }
    
private:
    void decode_loop() {
        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();
        unsigned long frame_count = 0;

        while (running) {
            std::lock_guard<std::mutex> lock(decoder_mutex);
            
            if (!fmt_ctx) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            int ret = av_read_frame(fmt_ctx, pkt);
            if (ret < 0) {
                // 重新连接或退出
                if (ret == AVERROR_EOF) {
                    std::cout << "End of stream reached, reconnecting..." << std::endl;
                    av_seek_frame(fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
                    av_packet_unref(pkt);
                    continue;
                }
                
                // 网络错误，尝试重新连接
                std::cerr << "Error reading frame: " << ret << ", retrying..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
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
                    
                    // 编码为JPEG并存入缓冲区
                    if (++frame_count % 5 != 0) {
                        // 降低帧率
                        av_frame_unref(frame);
                        break;
                    }

                    auto jpeg_data = jpeg_encoder.encode(frame);
                    if (!jpeg_data.empty()) {
                        frame_buffer.push(jpeg_data);
                    }
                    
                    av_frame_unref(frame);
                }
            }
            
            av_packet_unref(pkt);
        }
        
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }
};

// 全局解码器管理器
class DecoderManager {
private:
    std::unique_ptr<RTSPDecoder> decoder;
    std::unique_ptr<FrameBuffer> frame_buffer;
    std::mutex manager_mutex;
    std::string current_url;
    
public:
    DecoderManager() 
        : frame_buffer(std::make_unique<FrameBuffer>()) {}
    
    bool start_stream(const std::string& rtsp_url) {
        std::lock_guard<std::mutex> lock(manager_mutex);
        
        if (decoder && decoder->is_running()) {
            if (rtsp_url == current_url) {
                return true; // 已经在播放相同的流
            }
            decoder->stop();
        }
        
        current_url = rtsp_url;
        decoder = std::make_unique<RTSPDecoder>(rtsp_url, *frame_buffer);
        
        if (decoder->start()) {
            std::cout << "Started new stream: " << rtsp_url << std::endl;
            return true;
        } else {
            std::cerr << "Failed to start stream: " << rtsp_url << std::endl;
            decoder.reset();
            current_url.clear();
            return false;
        }
    }
    
    void stop_stream() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        if (decoder) {
            decoder->stop();
            decoder.reset();
            frame_buffer->clear();
            current_url.clear();
            std::cout << "Stream stopped" << std::endl;
        }
    }
    
    bool get_latest_frame(std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock(manager_mutex);
        if (frame_buffer && !frame_buffer->empty()) {
            return frame_buffer->get_latest(frame);
        }
        return false;
    }
    
    std::string get_current_url() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return current_url;
    }
    
    bool is_streaming() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return decoder && decoder->is_running();
    }
};


// HTML页面内容
const std::string HTML_PAGE = R"====(
    <!DOCTYPE html>
    <html lang="zh-CN">
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>RTSP视频流播放器</title>
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
                display: flex;
                justify-content: center;
                align-items: center;
                padding: 20px;
            }
            
            .container {
                background: white;
                border-radius: 20px;
                box-shadow: 0 20px 40px rgba(0, 0, 0, 0.1);
                overflow: hidden;
                width: 100%;
                max-width: 1000px;
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
                position: relative;
                overflow: hidden;
            }
            
            .header::before {
                content: "";
                position: absolute;
                top: -50%;
                left: -50%;
                width: 200%;
                height: 200%;
                background: radial-gradient(circle, rgba(255,255,255,0.1) 1px, transparent 1px);
                background-size: 20px 20px;
                animation: float 20s linear infinite;
            }
            
            @keyframes float {
                0% { transform: translate(0, 0) rotate(0deg); }
                100% { transform: translate(20px, 20px) rotate(360deg); }
            }
            
            .header h1 {
                font-size: 32px;
                margin-bottom: 10px;
                font-weight: 600;
                position: relative;
                z-index: 1;
            }
            
            .header p {
                opacity: 0.9;
                font-size: 16px;
                position: relative;
                z-index: 1;
            }
            
            .content {
                padding: 30px;
            }
            
            .control-panel {
                background: #f8f9fa;
                padding: 25px;
                border-radius: 15px;
                margin-bottom: 30px;
                border: 1px solid #e9ecef;
                box-shadow: 0 5px 15px rgba(0, 0, 0, 0.05);
            }
            
            .input-group {
                margin-bottom: 20px;
            }
            
            .input-group label {
                display: block;
                margin-bottom: 10px;
                color: #495057;
                font-weight: 500;
                font-size: 16px;
                display: flex;
                align-items: center;
                gap: 8px;
            }
            
            .rtsp-input {
                width: 100%;
                padding: 15px 20px;
                border: 2px solid #e9ecef;
                border-radius: 10px;
                font-size: 16px;
                transition: all 0.3s ease;
                background: white;
                box-shadow: inset 0 2px 4px rgba(0,0,0,0.05);
            }
            
            .rtsp-input:focus {
                outline: none;
                border-color: #4b6cb7;
                box-shadow: 0 0 0 3px rgba(75, 108, 183, 0.1), inset 0 2px 4px rgba(0,0,0,0.05);
            }
            
            .rtsp-input::placeholder {
                color: #adb5bd;
            }
            
            .button-group {
                display: flex;
                gap: 15px;
                flex-wrap: wrap;
                margin-top: 20px;
            }
            
            .btn {
                padding: 15px 30px;
                border: none;
                border-radius: 10px;
                font-size: 16px;
                font-weight: 500;
                cursor: pointer;
                transition: all 0.3s ease;
                display: flex;
                align-items: center;
                justify-content: center;
                gap: 10px;
                min-width: 160px;
                box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
            }
            
            .btn:hover {
                transform: translateY(-2px);
                box-shadow: 0 6px 12px rgba(0, 0, 0, 0.15);
            }
            
            .btn:active {
                transform: translateY(0);
            }
            
            .btn-start {
                background: linear-gradient(135deg, #28a745 0%, #20c997 100%);
                color: white;
            }
            
            .btn-start:hover {
                background: linear-gradient(135deg, #218838 0%, #1aa179 100%);
            }
            
            .btn-stop {
                background: linear-gradient(135deg, #dc3545 0%, #e83e8c 100%);
                color: white;
            }
            
            .btn-stop:hover {
                background: linear-gradient(135deg, #c82333 0%, #d63384 100%);
            }
            
            .btn-capture {
                background: linear-gradient(135deg, #007bff 0%, #6610f2 100%);
                color: white;
            }
            
            .btn-capture:hover {
                background: linear-gradient(135deg, #0056b3 0%, #560bd0 100%);
            }
            
            .video-section {
                margin-top: 20px;
            }
            
            .video-container {
                width: 100%;
                max-width: 800px;
                margin: 0 auto;
                background: #000;
                border-radius: 12px;
                overflow: hidden;
                box-shadow: 0 10px 30px rgba(0, 0, 0, 0.2);
                position: relative;
            }
            
            #video {
                width: 100%;
                display: block;
                transition: opacity 0.3s ease;
            }
            
            .video-placeholder {
                width: 100%;
                height: 450px;
                display: flex;
                flex-direction: column;
                align-items: center;
                justify-content: center;
                color: white;
                font-size: 18px;
                background: linear-gradient(135deg, #2c3e50 0%, #4ca1af 100%);
                text-align: center;
            }
            
            .video-placeholder i {
                font-size: 64px;
                margin-bottom: 20px;
                opacity: 0.8;
            }
            
            .status-container {
                margin-top: 20px;
                text-align: center;
                padding: 20px;
                border-radius: 12px;
                background: #e7f5ff;
                border: 1px solid #d0ebff;
                box-shadow: 0 5px 15px rgba(0, 0, 0, 0.05);
            }
            
            .status {
                font-size: 16px;
                color: #495057;
                display: flex;
                align-items: center;
                justify-content: center;
                gap: 12px;
                margin-bottom: 10px;
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
            
            .current-stream {
                margin-top: 15px;
                padding: 12px 20px;
                background: white;
                border-radius: 8px;
                font-size: 14px;
                color: #1864ab;
                border: 1px solid #d0ebff;
                word-break: break-all;
                box-shadow: inset 0 2px 4px rgba(0,0,0,0.05);
            }
            
            .info-panel {
                margin-top: 30px;
                padding: 25px;
                background: #f8f9fa;
                border-radius: 15px;
                border-left: 5px solid #4b6cb7;
                box-shadow: 0 5px 15px rgba(0, 0, 0, 0.05);
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
                font-size: 15px;
                color: #6c757d;
                border: 1px solid #dee2e6;
                transition: all 0.2s ease;
                display: flex;
                align-items: center;
                gap: 10px;
            }
            
            .info-list li:hover {
                background: #e7f5ff;
                border-color: #4b6cb7;
                color: #4b6cb7;
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
                
                .button-group {
                    flex-direction: column;
                }
                
                .btn {
                    width: 100%;
                }
                
                .video-container {
                    max-width: 100%;
                }
                
                .video-placeholder {
                    height: 300px;
                }
            }
            
            /* 自定义滚动条 */
            ::-webkit-scrollbar {
                width: 8px;
            }
            
            ::-webkit-scrollbar-track {
                background: #f1f1f1;
                border-radius: 4px;
            }
            
            ::-webkit-scrollbar-thumb {
                background: linear-gradient(135deg, #4b6cb7 0%, #182848 100%);
                border-radius: 4px;
            }
            
            ::-webkit-scrollbar-thumb:hover {
                background: linear-gradient(135deg, #3a5795 0%, #121f38 100%);
            }
        </style>
        <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css">
    </head>
    <body>
        <div class="container">
            <div class="header">
                <h1><i class="fas fa-video"></i> RTSP视频流播放器</h1>
                <p>实时视频流播放 - 支持动态切换视频源</p>
            </div>
            
            <div class="content">
                <div class="control-panel">
                    <div class="input-group">
                        <label for="rtspUrl"><i class="fas fa-link"></i> RTSP 地址</label>
                        <input type="text" 
                               id="rtspUrl" 
                               class="rtsp-input" 
                               placeholder="请输入RTSP流地址，例如：rtsp://example.com/stream"
                               value="">
                    </div>
                    
                    <div class="button-group">
                        <button id="startBtn" class="btn btn-start" onclick="startStream()">
                            <i class="fas fa-play"></i> 开始播放
                        </button>
                        <button id="stopBtn" class="btn btn-stop" onclick="stopStream()">
                            <i class="fas fa-stop"></i> 停止播放
                        </button>
                        <button id="captureBtn" class="btn btn-capture" onclick="captureFrame()">
                            <i class="fas fa-camera"></i> 截图保存
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
                            <i class="fas fa-film"></i>
                            <div>
                                <h3>等待视频流</h3>
                                <p>请输入RTSP地址并点击开始播放</p>
                            </div>
                        </div>
                        <img id="video" src="" style="display: none;">
                    </div>
                    
                    <div class="status-container">
                        <div class="status">
                            <div id="statusIndicator" class="status-indicator"></div>
                            <span id="statusText">准备就绪，请输入RTSP地址</span>
                        </div>
                        <div id="currentStream" class="current-stream" style="display: none;">
                            <i class="fas fa-satellite-dish"></i> 当前流地址: <span id="streamUrl"></span>
                        </div>
                    </div>
                </div>
                
                <div class="info-panel">
                    <h3><i class="fas fa-info-circle"></i> 使用说明</h3>
                    <ul class="info-list">
                        <li><i class="fas fa-keyboard"></i> 在输入框中输入有效的RTSP流地址</li>
                        <li><i class="fas fa-play-circle"></i> 点击"开始播放"按钮启动视频流</li>
                        <li><i class="fas fa-stop-circle"></i> 使用"停止播放"按钮可以停止当前流</li>
                        <li><i class="fas fa-exchange-alt"></i> 支持随时切换不同的RTSP流地址</li>
                        <li><i class="fas fa-camera-retro"></i> 点击"截图保存"可以将当前帧保存为图片</li>
                    </ul>
                </div>
            </div>
        </div>
        
        <canvas id="canvas" style="display:none;"></canvas>
        <a id="download" style="display:none;"></a>
        
        <script>
            let streamInterval = null;
            let isStreaming = false;
            let currentStreamUrl = '';
            
            function updateStatus(status, isActive = false) {
                const statusText = document.getElementById('statusText');
                const statusIndicator = document.getElementById('statusIndicator');
                const currentStream = document.getElementById('currentStream');
                const streamUrl = document.getElementById('streamUrl');
                
                statusText.textContent = status;
                statusIndicator.className = 'status-indicator' + (isActive ? ' active' : '');
                
                if (currentStreamUrl) {
                    currentStream.style.display = 'block';
                    streamUrl.textContent = currentStreamUrl;
                } else {
                    currentStream.style.display = 'none';
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
            
            function startStream() {
                const rtspUrl = document.getElementById('rtspUrl').value.trim();
                
                if (!rtspUrl) {
                    alert('请输入RTSP地址');
                    return;
                }
                
                if (isStreaming && rtspUrl === currentStreamUrl) {
                    alert('当前已在播放此视频流');
                    return;
                }
                
                // 停止之前的流
                if (isStreaming) {
                    stopStreamInternal();
                }
                
                showLoading(true);
                updateStatus('正在连接视频流...', true);
                
                // 发送请求到服务器开始新的流
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
                        
                        updateStatus('正在播放: ' + rtspUrl, true);
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
                
                // 每100ms更新一次帧
                streamInterval = setInterval(updateVideoFrame, 100);
            }
            
            function updateVideoFrame() {
                if (!isStreaming) return;
                
                const video = document.getElementById('video');
                const timestamp = new Date().getTime();
                video.src = '/video_frame?t=' + timestamp;
            }
            
            function stopStream() {
                if (!isStreaming) {
                    alert('当前没有正在播放的视频流');
                    return;
                }
                
                if (confirm('确定要停止当前视频流吗？')) {
                    stopStreamInternal();
                }
            }
            
            function stopStreamInternal() {
                if (streamInterval) {
                    clearInterval(streamInterval);
                    streamInterval = null;
                }
                
                // 发送停止请求到服务器
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
            
            function resetVideoDisplay() {
                isStreaming = false;
                currentStreamUrl = '';
                
                const video = document.getElementById('video');
                const videoPlaceholder = document.getElementById('videoPlaceholder');
                
                video.style.display = 'none';
                video.src = '';
                videoPlaceholder.style.display = 'flex';
                
                updateStatus('已停止播放', false);
            }
            
            function captureFrame() {
                if (!isStreaming) {
                    alert('请先开始播放视频流');
                    return;
                }
                
                const video = document.getElementById('video');
                const canvas = document.getElementById('canvas');
                const downloadLink = document.getElementById('download');
                
                canvas.width = video.naturalWidth || 640;
                canvas.height = video.naturalHeight || 480;
                
                const ctx = canvas.getContext('2d');
                ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
                
                const dataURL = canvas.toDataURL('image/jpeg', 0.9);
                const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
                
                downloadLink.href = dataURL;
                downloadLink.download = 'rtsp截图_' + timestamp + '.jpg';
                downloadLink.textContent = '下载图片';
                downloadLink.click();
                
                alert('截图已保存！');
            }
            
            // 输入框回车事件
            document.getElementById('rtspUrl').addEventListener('keypress', function(e) {
                if (e.key === 'Enter') {
                    startStream();
                }
            });
            
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
                        if (data.is_streaming && data.current_url) {
                            document.getElementById('rtspUrl').value = data.current_url;
                            startStream();
                        }
                    })
                    .catch(error => {
                        console.error('检查流状态时出错:', error);
                    });
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
    
    std::cout << "RTSP Stream Viewer Server" << std::endl;
    std::cout << "==========================" << std::endl;
    std::cout << "Server starting on port " << port << std::endl;
    std::cout << "Open http://localhost:" << port << " in your browser" << std::endl;
    std::cout << std::endl;
    std::cout << "Available endpoints:" << std::endl;
    std::cout << "  GET  /              - Web interface" << std::endl;
    std::cout << "  GET  /video_frame   - Get video frame (MJPEG stream)" << std::endl;
    std::cout << "  POST /start_stream  - Start RTSP stream" << std::endl;
    std::cout << "  POST /stop_stream   - Stop current stream" << std::endl;
    std::cout << "  GET  /stream_status - Get current stream status" << std::endl;
    std::cout << std::endl;
    
    // 创建HTTP服务器
    httplib::Server server;
    
    // 主页
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(HTML_PAGE, "text/html");
    });
    
    // 获取视频帧
    server.Get("/video_frame", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        std::vector<uint8_t> frame;
        if (decoder_manager.get_latest_frame(frame)) {
            res.set_content(reinterpret_cast<char*>(frame.data()), frame.size(), "image/jpeg");
        } else {
            // 返回空白图像
            res.set_content(reinterpret_cast<const char*>(BLANK_JPEG), sizeof(BLANK_JPEG), "image/jpeg");
        }
    });
    
    // 开始流
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
        decoder_manager.stop_stream();
        res.set_content(create_json_response(true), "application/json");
    });
    
    // 获取流状态
    server.Get("/stream_status", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        res.set_content(create_status_json(decoder_manager.is_streaming(), decoder_manager.get_current_url()), "application/json");
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