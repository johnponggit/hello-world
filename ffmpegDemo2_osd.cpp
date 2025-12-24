#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <vector>
#include <memory>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

#include "httplib.h"

// OSD配置结构体
struct OSDConfig {
    bool enable_time = true;
    bool enable_text = true;
    bool enable_border = false;
    bool enable_fps = true;
    std::string text_content = "I'll be rich, not bald";
    int text_x = 10;
    int text_y = 30; 
    int time_x = 10;
    int time_y = 10;
    uint32_t text_color = 0xFFFFFF;  // RGB白色
    uint32_t border_color = 0xFF0000; // RGB红色
    int border_thickness = 2;
    int font_size = 16; 
    float transparency = 0.7;  // 透明度
};

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

// OSD配置JSON
std::string create_osd_config_json(const OSDConfig& config) {
    std::stringstream ss;
    ss << "{";
    ss << "\"enable_time\":" << (config.enable_time ? "true" : "false") << ",";
    ss << "\"enable_text\":" << (config.enable_text ? "true" : "false") << ",";
    ss << "\"enable_border\":" << (config.enable_border ? "true" : "false") << ",";
    ss << "\"enable_fps\":" << (config.enable_fps ? "true" : "false") << ",";
    ss << "\"text_content\":\"" << config.text_content << "\",";
    ss << "\"text_x\":" << config.text_x << ",";
    ss << "\"text_y\":" << config.text_y << ",";
    ss << "\"time_x\":" << config.time_x << ",";
    ss << "\"time_y\":" << config.time_y << ",";
    ss << "\"font_size\":" << config.font_size << ",";
    ss << "\"transparency\":" << config.transparency;
    ss << "}";
    return ss.str();
}

// 简单的字体绘制函数
class SimpleFontRenderer {
private:
    // 简单的8x8 ASCII字符集
    static const uint8_t font_8x8[95][8];
    
public:
    static void draw_char(uint8_t* image, int width, int height, int stride,
        int x, int y, char c, uint32_t color, int scale = 1) {
        if (c < 32 || c > 126) return;  // 只支持可打印ASCII字符

        int char_index = c - 32;
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        for (int row = 0; row < 8; row++) {
            uint8_t font_row = SimpleFontRenderer::font_8x8[char_index][row];
            // 计算当前行在 buffer 中的起始指针（基准位置）
            // 注意：这里假设 y 和 x 是已经计算好 scale 后的起始坐标
            int row_offset_base = (y + row * scale) * stride + x * 3;
            
            for (int col = 0; col < 8; col++) {               
                if ((font_row >> col) & 0x01) {
                    
                    // 绘制 Scale 块
                    for (int sy = 0; sy < scale; sy++) {
                        // 计算当前缩放块的精确行指针起始点
                        // 基准行偏移 + 当前缩放行偏移 + 当前列偏移
                        uint8_t* ptr = image + row_offset_base + (sy * stride) + (col * scale * 3);
                        
                        for (int sx = 0; sx < scale; sx++) {
                            ptr[0] = b; // Blue
                            ptr[1] = g; // Green
                            ptr[2] = r; // Red
                            ptr += 3;   // 移动到这一行的下一个像素
                        }
                    }
                }
            }
        }
    }   

    static void draw_string(uint8_t* image, int width, int height, int stride,
        int x, int y, const std::string& text,  uint32_t color, int scale = 1) {
        int current_x = x;
        for (char c : text) {
            draw_char(image, width, height, stride, current_x, y, c, color, scale);
            current_x += 8 * scale + 2;  // 字符间距
        }
}
};

// 简单的8x8 ASCII字体数据
const uint8_t SimpleFontRenderer::font_8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 空格 (32)
    {0x08,0x08,0x08,0x08,0x08,0x00,0x08,0x00}, // ! (33) - 细
    {0x14,0x14,0x00,0x00,0x00,0x00,0x00,0x00}, // " (34)
    {0x00,0x14,0x3E,0x14,0x3E,0x14,0x00,0x00}, // # (35) - 细
    {0x08,0x1C,0x02,0x1C,0x20,0x1C,0x08,0x00}, // $ (36)
    {0x00,0x22,0x10,0x08,0x04,0x22,0x00,0x00}, // % (37) - 细
    {0x08,0x14,0x08,0x2A,0x14,0x14,0x2A,0x00}, // & (38) - 细
    {0x04,0x04,0x02,0x00,0x00,0x00,0x00,0x00}, // ' (39)
    {0x10,0x08,0x04,0x04,0x04,0x08,0x10,0x00}, // ( (40)
    {0x04,0x08,0x10,0x10,0x10,0x08,0x04,0x00}, // ) (41)
    {0x00,0x14,0x08,0x3E,0x08,0x14,0x00,0x00}, // * (42)
    {0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00}, // + (43)
    {0x00,0x00,0x00,0x00,0x00,0x04,0x04,0x02}, // , (44)
    {0x00,0x00,0x00,0x3E,0x00,0x00,0x00,0x00}, // - (45)
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // . (46)
    {0x20,0x10,0x08,0x04,0x02,0x01,0x00,0x00}, // / (47) - 细
    
    // 数字 (48-57) - 细体
    {0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0x00}, // 0 (48)
    {0x08,0x0C,0x08,0x08,0x08,0x08,0x1C,0x00}, // 1 (49)
    {0x1C,0x22,0x20,0x18,0x04,0x02,0x3E,0x00}, // 2 (50)
    {0x1C,0x22,0x20,0x18,0x20,0x22,0x1C,0x00}, // 3 (51)
    {0x30,0x28,0x24,0x22,0x3E,0x20,0x20,0x00}, // 4 (52)
    {0x3E,0x02,0x1E,0x20,0x20,0x22,0x1C,0x00}, // 5 (53)
    {0x18,0x04,0x02,0x1E,0x22,0x22,0x1C,0x00}, // 6 (54)
    {0x3E,0x20,0x10,0x08,0x04,0x04,0x04,0x00}, // 7 (55)
    {0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C,0x00}, // 8 (56)
    {0x1C,0x22,0x22,0x3C,0x20,0x10,0x0E,0x00}, // 9 (57)
    
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // : (58)
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x04}, // ; (59)
    {0x10,0x08,0x04,0x02,0x04,0x08,0x10,0x00}, // < (60)
    {0x00,0x00,0x3E,0x00,0x3E,0x00,0x00,0x00}, // = (61)
    {0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00}, // > (62)
    {0x1C,0x22,0x20,0x10,0x08,0x00,0x08,0x00}, // ? (63)
    {0x1C,0x22,0x4A,0x52,0x4C,0x20,0x1E,0x00}, // @ (64)

    // 大写字母 (65-90) - 细体
    {0x1C,0x22,0x22,0x3E,0x22,0x22,0x22,0x00}, // A (65)
    {0x1E,0x22,0x22,0x1E,0x22,0x22,0x1E,0x00}, // B (66)
    {0x1C,0x22,0x02,0x02,0x02,0x22,0x1C,0x00}, // C (67)
    {0x1E,0x22,0x22,0x22,0x22,0x22,0x1E,0x00}, // D (68)
    {0x3E,0x02,0x02,0x1E,0x02,0x02,0x3E,0x00}, // E (69)
    {0x3E,0x02,0x02,0x1E,0x02,0x02,0x02,0x00}, // F (70)
    {0x1C,0x22,0x02,0x32,0x22,0x22,0x3C,0x00}, // G (71)
    {0x22,0x22,0x22,0x3E,0x22,0x22,0x22,0x00}, // H (72)
    {0x1C,0x08,0x08,0x08,0x08,0x08,0x1C,0x00}, // I (73)
    {0x20,0x20,0x20,0x20,0x22,0x22,0x1C,0x00}, // J (74)
    {0x22,0x12,0x0A,0x06,0x0A,0x12,0x22,0x00}, // K (75)
    {0x02,0x02,0x02,0x02,0x02,0x02,0x3E,0x00}, // L (76)
    {0x22,0x36,0x2A,0x2A,0x22,0x22,0x22,0x00}, // M (77)
    {0x22,0x26,0x2A,0x32,0x22,0x22,0x22,0x00}, // N (78)
    {0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0x00}, // O (79)
    {0x1E,0x22,0x22,0x1E,0x02,0x02,0x02,0x00}, // P (80)
    {0x1C,0x22,0x22,0x22,0x2A,0x24,0x1A,0x00}, // Q (81)
    {0x1E,0x22,0x22,0x1E,0x0A,0x12,0x22,0x00}, // R (82)
    {0x1C,0x22,0x02,0x1C,0x20,0x22,0x1C,0x00}, // S (83)
    {0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0x00}, // T (84)
    {0x22,0x22,0x22,0x22,0x22,0x22,0x1C,0x00}, // U (85)
    {0x22,0x22,0x22,0x22,0x22,0x14,0x08,0x00}, // V (86)
    {0x22,0x22,0x22,0x2A,0x2A,0x36,0x22,0x00}, // W (87)
    {0x22,0x22,0x14,0x08,0x14,0x22,0x22,0x00}, // X (88)
    {0x22,0x22,0x14,0x08,0x08,0x08,0x08,0x00}, // Y (89)
    {0x3E,0x20,0x10,0x08,0x04,0x02,0x3E,0x00}, // Z (90)
    
    {0x1C,0x04,0x04,0x04,0x04,0x04,0x1C,0x00}, // [ (91)
    {0x00,0x02,0x04,0x08,0x10,0x20,0x00,0x00}, // \ (92) - 细
    {0x1C,0x10,0x10,0x10,0x10,0x10,0x1C,0x00}, // ] (93)
    {0x08,0x14,0x22,0x00,0x00,0x00,0x00,0x00}, // ^ (94)
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3E,0x00}, // _ (95)
    {0x04,0x08,0x10,0x00,0x00,0x00,0x00,0x00}, // ` (96)
    
    // 小写字母 (97-122) - 细体
    {0x00,0x00,0x1C,0x20,0x3C,0x22,0x3C,0x00}, // a (97)
    {0x02,0x02,0x1E,0x22,0x22,0x22,0x1E,0x00}, // b (98)
    {0x00,0x00,0x1C,0x22,0x02,0x22,0x1C,0x00}, // c (99)
    {0x20,0x20,0x3C,0x22,0x22,0x22,0x3C,0x00}, // d (100)
    {0x00,0x00,0x1C,0x22,0x3E,0x02,0x1C,0x00}, // e (101)
    {0x18,0x24,0x04,0x0E,0x04,0x04,0x04,0x00}, // f (102)
    {0x00,0x00,0x3C,0x22,0x22,0x3C,0x20,0x1C}, // g (103)
    {0x02,0x02,0x1E,0x22,0x22,0x22,0x22,0x00}, // h (104)
    {0x08,0x00,0x0C,0x08,0x08,0x08,0x1C,0x00}, // i (105)
    {0x10,0x00,0x18,0x10,0x10,0x10,0x12,0x0C}, // j (106)
    {0x02,0x02,0x22,0x12,0x0E,0x12,0x22,0x00}, // k (107)
    {0x0C,0x08,0x08,0x08,0x08,0x08,0x1C,0x00}, // l (108)
    {0x00,0x00,0x16,0x2A,0x2A,0x2A,0x22,0x00}, // m (109)
    {0x00,0x00,0x1E,0x22,0x22,0x22,0x22,0x00}, // n (110)
    {0x00,0x00,0x1C,0x22,0x22,0x22,0x1C,0x00}, // o (111)
    {0x00,0x00,0x1E,0x22,0x22,0x1E,0x02,0x02}, // p (112)
    {0x00,0x00,0x3C,0x22,0x22,0x3C,0x20,0x20}, // q (113)
    {0x00,0x00,0x1A,0x06,0x02,0x02,0x02,0x00}, // r (114)
    {0x00,0x00,0x3C,0x02,0x1C,0x20,0x1E,0x00}, // s (115)
    {0x04,0x04,0x1E,0x04,0x04,0x24,0x18,0x00}, // t (116)
    {0x00,0x00,0x22,0x22,0x22,0x22,0x3C,0x00}, // u (117)
    {0x00,0x00,0x22,0x22,0x22,0x14,0x08,0x00}, // v (118)
    {0x00,0x00,0x22,0x22,0x2A,0x2A,0x14,0x00}, // w (119)
    {0x00,0x00,0x22,0x14,0x08,0x14,0x22,0x00}, // x (120)
    {0x00,0x00,0x22,0x22,0x22,0x3C,0x20,0x1C}, // y (121)
    {0x00,0x00,0x3E,0x10,0x08,0x04,0x3E,0x00}, // z (122)
    
    {0x38,0x0C,0x0C,0x06,0x0C,0x0C,0x38,0x00}, // { (123)
    {0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00}, // | (124) - 细
    {0x0E,0x18,0x18,0x30,0x18,0x18,0x0E,0x00}, // } (125)
    {0x00,0x00,0x2C,0x1A,0x00,0x00,0x00,0x00}  // ~ (126)
};

// OSD绘制器类
class OSDRenderer {
private:
    OSDConfig config;
    std::atomic<int> frame_count{0};
    std::chrono::steady_clock::time_point last_fps_time;
    float current_fps = 0.0f;
    
public:
    OSDRenderer() {
        last_fps_time = std::chrono::steady_clock::now();
    }
    
    void set_config(const OSDConfig& new_config) {
        config = new_config;
    }
    
    OSDConfig get_config() const {
        return config;
    }
    
    void draw_osd(uint8_t* rgb_data, int width, int height, int stride) {
        frame_count++;
        
        // 计算FPS
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_fps_time).count();
        
        if (elapsed >= 1000) {  // 每秒更新一次FPS
            current_fps = frame_count * 1000.0f / elapsed;
            frame_count = 0;
            last_fps_time = now;
        }
        
        // 绘制边框
        if (config.enable_border) {
            draw_border(rgb_data, width, height, stride);
        }
        
        // 绘制时间戳
        if (config.enable_time) {
            draw_timestamp(rgb_data, width, height, stride);
        }
        
        // 绘制自定义文本
        if (config.enable_text) {
            draw_text(rgb_data, width, height, stride);
        }
        
        // 绘制FPS
        if (config.enable_fps) {
            draw_fps(rgb_data, width, height, stride);
        }
    }
    
private:
    void draw_border(uint8_t* rgb_data, int width, int height, int stride) {
        uint8_t r = (config.border_color >> 16) & 0xFF;
        uint8_t g = (config.border_color >> 8) & 0xFF;
        uint8_t b = config.border_color & 0xFF;
        
        // 绘制上边框
        for (int y = 0; y < config.border_thickness; y++) {
            for (int x = 0; x < width; x++) {
                int pos = y * stride + x * 3;
                rgb_data[pos] = b;
                rgb_data[pos + 1] = g;
                rgb_data[pos + 2] = r;
            }
        }
        
        // 绘制下边框
        for (int y = height - config.border_thickness; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int pos = y * stride + x * 3;
                rgb_data[pos] = b;
                rgb_data[pos + 1] = g;
                rgb_data[pos + 2] = r;
            }
        }
        
        // 绘制左边框
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < config.border_thickness; x++) {
                int pos = y * stride + x * 3;
                rgb_data[pos] = b;
                rgb_data[pos + 1] = g;
                rgb_data[pos + 2] = r;
            }
        }
        
        // 绘制右边框
        for (int y = 0; y < height; y++) {
            for (int x = width - config.border_thickness; x < width; x++) {
                int pos = y * stride + x * 3;
                rgb_data[pos] = b;
                rgb_data[pos + 1] = g;
                rgb_data[pos + 2] = r;
            }
        }
    }
    
    void draw_timestamp(uint8_t* rgb_data, int width, int height, int stride) {
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm = *std::localtime(&now_time);
        
        std::stringstream ss;
        ss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
        
        SimpleFontRenderer::draw_string(
            rgb_data, width, height, stride,
            config.time_x, config.time_y,
            ss.str(), config.text_color, config.font_size / 8);
    }
    
    void draw_text(uint8_t* rgb_data, int width, int height, int stride) {
        SimpleFontRenderer::draw_string(
            rgb_data, width, height, stride,
            config.text_x, config.text_y,
            config.text_content, config.text_color, config.font_size / 8);
    }
    
    void draw_fps(uint8_t* rgb_data, int width, int height, int stride) {
        std::stringstream ss;
        ss.precision(1);
        ss << std::fixed << current_fps << " FPS";
        
        SimpleFontRenderer::draw_string(
            rgb_data, width, height, stride,
            width - 140, 10,
            ss.str(), config.text_color, config.font_size / 8); 
    }
    
    void apply_transparency(uint8_t* rgb_data, int width, int height, int stride) {
        // 简化实现：仅对OSD区域的像素应用透明度
        // 实际项目中可能需要更复杂的混合算法
    }
};

// JPEG编码器类（修改以支持OSD）
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
    
    std::vector<uint8_t> encode(AVFrame* input_frame, OSDRenderer& osd_renderer) {
        std::vector<uint8_t> jpeg_data;
        
        // 转换像素格式到RGB
        if (!sws_ctx) {
            sws_ctx = sws_getContext(
                input_frame->width, input_frame->height,
                (AVPixelFormat)input_frame->format,
                codec_ctx->width, codec_ctx->height,
                AV_PIX_FMT_RGB24,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
        }
        
        // 创建RGB帧
        AVFrame* rgb_frame = av_frame_alloc();
        rgb_frame->width = codec_ctx->width;
        rgb_frame->height = codec_ctx->height;
        rgb_frame->format = AV_PIX_FMT_RGB24;
        av_frame_get_buffer(rgb_frame, 0);
        
        // 转换到RGB
        sws_scale(sws_ctx, input_frame->data, input_frame->linesize,
                  0, input_frame->height,
                  rgb_frame->data, rgb_frame->linesize);
        
        // 绘制OSD
        if (rgb_frame->data[0]) {
            osd_renderer.draw_osd(
                rgb_frame->data[0],
                rgb_frame->width,
                rgb_frame->height,
                rgb_frame->linesize[0]
            );
        }
        
        // 转换回YUV格式进行JPEG编码
        SwsContext* sws_ctx_yuv = sws_getContext(
            codec_ctx->width, codec_ctx->height,
            AV_PIX_FMT_RGB24,
            codec_ctx->width, codec_ctx->height,
            codec_ctx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        frame->width = codec_ctx->width;
        frame->height = codec_ctx->height;
        frame->format = codec_ctx->pix_fmt;
        av_frame_get_buffer(frame, 0);
        
        sws_scale(sws_ctx_yuv, rgb_frame->data, rgb_frame->linesize,
                  0, codec_ctx->height,
                  frame->data, frame->linesize);
        
        sws_freeContext(sws_ctx_yuv);
        av_frame_free(&rgb_frame);
        
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

// RTSP解码器类（修改以支持OSD）
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
    OSDRenderer osd_renderer;
    std::mutex decoder_mutex;
    
public:
    RTSPDecoder(const std::string& url, FrameBuffer& buffer)
        : rtsp_url(url), frame_buffer(buffer) {}
    
    ~RTSPDecoder() {
        stop();
    }
    
    void set_osd_config(const OSDConfig& config) {
        osd_renderer.set_config(config);
    }
    
    OSDConfig get_osd_config() {
        return osd_renderer.get_config();
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
                    if (++frame_count % 3 != 0) {  // 降低帧率
                        av_frame_unref(frame);
                        break;
                    }

                    auto jpeg_data = jpeg_encoder.encode(frame, osd_renderer);
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
    OSDConfig osd_config;  // 存储OSD配置
    
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
        
        // 应用当前OSD配置
        decoder->set_osd_config(osd_config);
        
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
    
    void set_osd_config(const OSDConfig& config) {
        std::lock_guard<std::mutex> lock(manager_mutex);
        osd_config = config;
        if (decoder) {
            decoder->set_osd_config(config);
        }
    }
    
    OSDConfig get_osd_config() {
        std::lock_guard<std::mutex> lock(manager_mutex);
        return osd_config;
    }
};

// HTML页面内容（添加OSD控制面板）
const std::string HTML_PAGE = R"====(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>RTSP视频流播放器 - 带OSD功能</title>
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
            max-width: 1400px;
            margin: 0 auto;
            display: grid;
            grid-template-columns: 1fr 400px;
            gap: 20px;
        }
        
        .main-content {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 40px rgba(0, 0, 0, 0.1);
            overflow: hidden;
            animation: fadeIn 0.5s ease;
        }
        
        .sidebar {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 40px rgba(0, 0, 0, 0.1);
            padding: 25px;
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
            font-size: 28px;
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
            margin-bottom: 20px;
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
        
        .sidebar-section {
            margin-bottom: 25px;
        }
        
        .sidebar-section h3 {
            color: #495057;
            margin-bottom: 15px;
            font-size: 18px;
            display: flex;
            align-items: center;
            gap: 10px;
            padding-bottom: 10px;
            border-bottom: 2px solid #4b6cb7;
        }
        
        .osd-controls {
            display: flex;
            flex-direction: column;
            gap: 15px;
        }
        
        .osd-control-group {
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        
        .osd-control-group label {
            font-size: 14px;
            color: #495057;
        }
        
        .checkbox-group {
            display: flex;
            gap: 15px;
            flex-wrap: wrap;
        }
        
        .checkbox-item {
            display: flex;
            align-items: center;
            gap: 5px;
        }
        
        .text-input-group {
            display: flex;
            flex-direction: column;
            gap: 10px;
        }
        
        .text-input-group input {
            padding: 10px 15px;
            border: 2px solid #e9ecef;
            border-radius: 8px;
            font-size: 14px;
        }
        
        .text-input-group input:focus {
            outline: none;
            border-color: #4b6cb7;
        }
        
        .position-controls {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
        }
        
        .position-group {
            display: flex;
            flex-direction: column;
            gap: 5px;
        }
        
        .position-group label {
            font-size: 12px;
            color: #6c757d;
        }
        
        .position-group input {
            padding: 8px 12px;
            border: 2px solid #e9ecef;
            border-radius: 6px;
            font-size: 14px;
        }
        
        .slider-group {
            display: flex;
            flex-direction: column;
            gap: 10px;
        }
        
        .slider-group input[type="range"] {
            width: 100%;
            height: 6px;
            background: #e9ecef;
            border-radius: 3px;
            outline: none;
            -webkit-appearance: none;
        }
        
        .slider-group input[type="range"]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 20px;
            height: 20px;
            background: #4b6cb7;
            border-radius: 50%;
            cursor: pointer;
        }
        
        .slider-value {
            font-size: 12px;
            color: #6c757d;
            text-align: right;
        }
        
        .btn-update-osd {
            width: 100%;
            padding: 12px;
            background: linear-gradient(135deg, #ff6b6b 0%, #ff8e53 100%);
            color: white;
            border: none;
            border-radius: 10px;
            font-size: 16px;
            cursor: pointer;
            transition: all 0.3s ease;
            margin-top: 15px;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
        }
        
        .btn-update-osd:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 12px rgba(0, 0, 0, 0.15);
            background: linear-gradient(135deg, #ff5252 0%, #ff7b40 100%);
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
        
        @media (max-width: 1024px) {
            .container {
                grid-template-columns: 1fr;
            }
            
            .sidebar {
                order: -1;
            }
        }
        
        @media (max-width: 768px) {
            .main-content {
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
    </style>
    <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css">
</head>
<body>
    <div class="container">
        <div class="main-content">
            <div class="header">
                <h1><i class="fas fa-video"></i> RTSP视频流播放器 - OSD功能</h1>
                <p>实时视频流播放 - 支持屏幕显示(OSD)叠加</p>
            </div>
            
            <div class="content">
                <div class="control-panel">
                    <div class="input-group">
                        <label for="rtspUrl"><i class="fas fa-link"></i> RTSP 地址</label>
                        <input type="text" 
                               id="rtspUrl" 
                               class="rtsp-input" 
                               placeholder="请输入RTSP流地址，例如：rtsp://example.com/stream"
                               value="rtsp://192.168.1.100:554/stream">
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
            </div>
        </div>
        
        <div class="sidebar">
            <div class="sidebar-section">
                <h3><i class="fas fa-palette"></i> OSD配置面板</h3>
                <div class="osd-controls">
                    <div class="checkbox-group">
                        <div class="checkbox-item">
                            <input type="checkbox" id="enableTime" checked>
                            <label for="enableTime">时间戳</label>
                        </div>
                        <div class="checkbox-item">
                            <input type="checkbox" id="enableText" checked>
                            <label for="enableText">自定义文本</label>
                        </div>
                        <div class="checkbox-item">
                            <input type="checkbox" id="enableBorder">
                            <label for="enableBorder">边框</label>
                        </div>
                        <div class="checkbox-item">
                            <input type="checkbox" id="enableFPS" checked>
                            <label for="enableFPS">FPS显示</label>
                        </div>
                    </div>
                    
                    <div class="text-input-group">
                        <label>自定义文本内容:</label>
                        <input type="text" id="osdText" value="I'll be rich, not bald" placeholder="输入要显示的文本">
                    </div>
                    
                    <div class="position-controls">
                        <div class="position-group">
                            <label>文本X位置:</label>
                            <input type="number" id="textX" value="10" min="0" max="1000">
                        </div>
                        <div class="position-group">
                            <label>文本Y位置:</label>
                            <input type="number" id="textY" value="30" min="0" max="1000">
                        </div>
                        <div class="position-group">
                            <label>时间X位置:</label>
                            <input type="number" id="timeX" value="10" min="0" max="1000">
                        </div>
                        <div class="position-group">
                            <label>时间Y位置:</label>
                            <input type="number" id="timeY" value="10" min="0" max="1000">
                        </div>
                    </div>
                    
                    <div class="slider-group">
                        <label>字体大小: <span id="fontSizeValue">16</span>px</label>
                        <input type="range" id="fontSize" min="8" max="32" value="16" step="1">
                    </div>
                    
                    <div class="slider-group">
                        <label>透明度: <span id="transparencyValue">70</span>%</label>
                        <input type="range" id="transparency" min="0" max="100" value="70" step="1">
                    </div>
                    
                    <button class="btn-update-osd" onclick="updateOSD()">
                        <i class="fas fa-sync-alt"></i> 更新OSD设置
                    </button>
                </div>
            </div>
            
            <div class="sidebar-section">
                <h3><i class="fas fa-info-circle"></i> OSD预览</h3>
                <div style="background: #000; padding: 15px; border-radius: 8px; margin-top: 10px;">
                    <div id="osdPreview" style="position: relative; height: 200px; background: #333;">
                        <div id="osdTimePreview" style="position: absolute; color: white; font-family: monospace; font-size: 14px; left: 10px; top: 10px;">
                            2024-01-01 12:00:00
                        </div>
                        <div id="osdTextPreview" style="position: absolute; color: white; font-family: monospace; font-size: 14px; left: 10px; top: 30px;">
                            I'll be rich, not bald
                        </div>
                        <div id="osdFPSPreview" style="position: absolute; color: #0f0; font-family: monospace; font-size: 14px; right: 10px; top: 10px;">
                            30.0 FPS
                        </div>
                    </div>
                </div>
            </div>
            
            <div class="sidebar-section">
                <h3><i class="fas fa-cogs"></i> 服务器状态</h3>
                <div style="margin-top: 10px; font-size: 14px;">
                    <div>连接状态: <span id="serverStatus">未连接</span></div>
                    <div>帧率: <span id="serverFPS">0 FPS</span></div>
                    <div>分辨率: <span id="serverResolution">640x480</span></div>
                    <div>延迟: <span id="serverLatency">0ms</span></div>
                </div>
            </div>
        </div>
    </div>
    
    <canvas id="canvas" style="display:none;"></canvas>
    <a id="download" style="display:none;"></a>
    
    <script>
        let streamInterval = null;
        let isStreaming = false;
        let currentStreamUrl = '';
        let frameCount = 0;
        let lastFrameTime = Date.now();
        let currentFPS = 0;
        
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
                    
                    // 加载当前的OSD配置
                    loadOSDConfig();
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
            
            // 开始FPS计算
            frameCount = 0;
            lastFrameTime = Date.now();
            setInterval(calculateFPS, 1000);
        }
        
        function calculateFPS() {
            const now = Date.now();
            const elapsed = now - lastFrameTime;
            if (elapsed > 0) {
                currentFPS = (frameCount / elapsed) * 1000;
                document.getElementById('serverFPS').textContent = currentFPS.toFixed(1) + ' FPS';
                document.getElementById('osdFPSPreview').textContent = currentFPS.toFixed(1) + ' FPS';
                frameCount = 0;
                lastFrameTime = now;
            }
        }
        
        function updateVideoFrame() {
            if (!isStreaming) return;
            
            const video = document.getElementById('video');
            const timestamp = new Date().getTime();
            video.src = '/video_frame?t=' + timestamp;
            
            // 更新FPS计数
            frameCount++;
            
            // 更新时间戳预览
            const now = new Date();
            document.getElementById('osdTimePreview').textContent = 
                now.getFullYear() + '-' + 
                String(now.getMonth() + 1).padStart(2, '0') + '-' + 
                String(now.getDate()).padStart(2, '0') + ' ' +
                String(now.getHours()).padStart(2, '0') + ':' + 
                String(now.getMinutes()).padStart(2, '0') + ':' + 
                String(now.getSeconds()).padStart(2, '0');
            
            // 更新OSD文本预览
            const osdText = document.getElementById('osdText').value;
            document.getElementById('osdTextPreview').textContent = osdText;
            
            // 更新OSD位置预览
            const textX = parseInt(document.getElementById('textX').value) || 10;
            const textY = parseInt(document.getElementById('textY').value) || 30;
            const timeX = parseInt(document.getElementById('timeX').value) || 10;
            const timeY = parseInt(document.getElementById('timeY').value) || 10;
            
            document.getElementById('osdTextPreview').style.left = textX + 'px';
            document.getElementById('osdTextPreview').style.top = textY + 'px';
            document.getElementById('osdTimePreview').style.left = timeX + 'px';
            document.getElementById('osdTimePreview').style.top = timeY + 'px';
            
            // 更新字体大小预览
            const fontSize = parseInt(document.getElementById('fontSize').value) || 16;
            document.getElementById('osdTextPreview').style.fontSize = fontSize + 'px';
            document.getElementById('osdTimePreview').style.fontSize = fontSize + 'px';
            document.getElementById('osdFPSPreview').style.fontSize = fontSize + 'px';
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
            document.getElementById('serverStatus').textContent = '未连接';
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
        
        function updateOSD() {
            const osdConfig = {
                enable_time: document.getElementById('enableTime').checked,
                enable_text: document.getElementById('enableText').checked,
                enable_border: document.getElementById('enableBorder').checked,
                enable_fps: document.getElementById('enableFPS').checked,
                text_content: document.getElementById('osdText').value,
                text_x: parseInt(document.getElementById('textX').value) || 10,
                text_y: parseInt(document.getElementById('textY').value) || 30,
                time_x: parseInt(document.getElementById('timeX').value) || 10,
                time_y: parseInt(document.getElementById('timeY').value) || 10,
                font_size: parseInt(document.getElementById('fontSize').value) || 16,
                transparency: parseInt(document.getElementById('transparency').value) / 100 || 0.7
            };
            
            fetch('/update_osd', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(osdConfig)
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    alert('OSD设置已更新！');
                } else {
                    alert('更新失败: ' + data.error);
                }
            })
            .catch(error => {
                alert('请求失败: ' + error.message);
                console.error('Error:', error);
            });
        }
        
        function loadOSDConfig() {
            fetch('/get_osd_config')
                .then(response => response.json())
                .then(data => {
                    if (data.enable_time !== undefined) {
                        document.getElementById('enableTime').checked = data.enable_time;
                        document.getElementById('osdTimePreview').style.display = data.enable_time ? 'block' : 'none';
                    }
                    if (data.enable_text !== undefined) {
                        document.getElementById('enableText').checked = data.enable_text;
                        document.getElementById('osdTextPreview').style.display = data.enable_text ? 'block' : 'none';
                    }
                    if (data.enable_border !== undefined) {
                        document.getElementById('enableBorder').checked = data.enable_border;
                    }
                    if (data.enable_fps !== undefined) {
                        document.getElementById('enableFPS').checked = data.enable_fps;
                        document.getElementById('osdFPSPreview').style.display = data.enable_fps ? 'block' : 'none';
                    }
                    if (data.text_content !== undefined) {
                        document.getElementById('osdText').value = data.text_content;
                        document.getElementById('osdTextPreview').textContent = data.text_content;
                    }
                    if (data.text_x !== undefined) {
                        document.getElementById('textX').value = data.text_x;
                        document.getElementById('osdTextPreview').style.left = data.text_x + 'px';
                    }
                    if (data.text_y !== undefined) {
                        document.getElementById('textY').value = data.text_y;
                        document.getElementById('osdTextPreview').style.top = data.text_y + 'px';
                    }
                    if (data.time_x !== undefined) {
                        document.getElementById('timeX').value = data.time_x;
                        document.getElementById('osdTimePreview').style.left = data.time_x + 'px';
                    }
                    if (data.time_y !== undefined) {
                        document.getElementById('timeY').value = data.time_y;
                        document.getElementById('osdTimePreview').style.top = data.time_y + 'px';
                    }
                    if (data.font_size !== undefined) {
                        document.getElementById('fontSize').value = data.font_size;
                        document.getElementById('fontSizeValue').textContent = data.font_size;
                        document.getElementById('osdTextPreview').style.fontSize = data.font_size + 'px';
                        document.getElementById('osdTimePreview').style.fontSize = data.font_size + 'px';
                        document.getElementById('osdFPSPreview').style.fontSize = data.font_size + 'px';
                    }
                    if (data.transparency !== undefined) {
                        const transparencyValue = Math.round(data.transparency * 100);
                        document.getElementById('transparency').value = transparencyValue;
                        document.getElementById('transparencyValue').textContent = transparencyValue;
                        const alpha = data.transparency;
                        document.getElementById('osdTextPreview').style.opacity = alpha;
                        document.getElementById('osdTimePreview').style.opacity = alpha;
                        document.getElementById('osdFPSPreview').style.opacity = alpha;
                    }
                })
                .catch(error => {
                    console.error('加载OSD配置时出错:', error);
                });
        }
        
        // 更新UI控件事件
        document.getElementById('fontSize').addEventListener('input', function() {
            document.getElementById('fontSizeValue').textContent = this.value;
            document.getElementById('osdTextPreview').style.fontSize = this.value + 'px';
            document.getElementById('osdTimePreview').style.fontSize = this.value + 'px';
            document.getElementById('osdFPSPreview').style.fontSize = this.value + 'px';
        });
        
        document.getElementById('transparency').addEventListener('input', function() {
            document.getElementById('transparencyValue').textContent = this.value;
            const alpha = parseInt(this.value) / 100;
            document.getElementById('osdTextPreview').style.opacity = alpha;
            document.getElementById('osdTimePreview').style.opacity = alpha;
            document.getElementById('osdFPSPreview').style.opacity = alpha;
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
            
            // 更新服务器状态
            setInterval(() => {
                if (isStreaming) {
                    document.getElementById('serverStatus').textContent = '已连接';
                    const latency = Math.round(Math.random() * 100); // 模拟延迟
                    document.getElementById('serverLatency').textContent = latency + 'ms';
                }
            }, 2000);
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
    
    std::cout << "RTSP Stream Viewer Server with OSD" << std::endl;
    std::cout << "==================================" << std::endl;
    std::cout << "Server starting on port " << port << std::endl;
    std::cout << "Open http://localhost:" << port << " in your browser" << std::endl;
    std::cout << std::endl;
    std::cout << "Available endpoints:" << std::endl;
    std::cout << "  GET  /               - Web interface" << std::endl;
    std::cout << "  GET  /video_frame    - Get video frame (MJPEG stream)" << std::endl;
    std::cout << "  POST /start_stream   - Start RTSP stream" << std::endl;
    std::cout << "  POST /stop_stream    - Stop current stream" << std::endl;
    std::cout << "  GET  /stream_status  - Get current stream status" << std::endl;
    std::cout << "  POST /update_osd     - Update OSD configuration" << std::endl;
    std::cout << "  GET  /get_osd_config - Get current OSD configuration" << std::endl;
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
    
    // 更新OSD配置
    server.Post("/update_osd", [&decoder_manager](const httplib::Request& req, httplib::Response& res) {
        if (req.body.empty()) {
            res.set_content(create_json_response(false, "Empty request body"), "application/json");
            return;
        }
        
        try {
            auto json_body = req.body;
            OSDConfig new_config;
            
            // 解析JSON（简化处理，实际项目应使用JSON库）
            // 这里简单解析关键字段
            if (json_body.find("\"enable_time\"") != std::string::npos) {
                new_config.enable_time = json_body.find("\"enable_time\":true") != std::string::npos;
            }
            if (json_body.find("\"enable_text\"") != std::string::npos) {
                new_config.enable_text = json_body.find("\"enable_text\":true") != std::string::npos;
            }
            if (json_body.find("\"enable_border\"") != std::string::npos) {
                new_config.enable_border = json_body.find("\"enable_border\":true") != std::string::npos;
            }
            if (json_body.find("\"enable_fps\"") != std::string::npos) {
                new_config.enable_fps = json_body.find("\"enable_fps\":true") != std::string::npos;
            }
            
            // 提取文本内容
            size_t text_start = json_body.find("\"text_content\":\"");
            if (text_start != std::string::npos) {
                text_start += 16; // "\"text_content\":\"" 的长度
                size_t text_end = json_body.find("\"", text_start);
                if (text_end != std::string::npos) {
                    new_config.text_content = json_body.substr(text_start, text_end - text_start);
                }
            }
            
            // 提取其他数值
            // 这里简化处理，实际应该使用JSON解析库
            
            decoder_manager.set_osd_config(new_config);
            res.set_content(create_json_response(true), "application/json");
        } catch (const std::exception& e) {
            res.set_content(create_json_response(false, std::string("Error parsing OSD config: ") + e.what()), "application/json");
        }
    });
    
    // 获取OSD配置
    server.Get("/get_osd_config", [&decoder_manager](const httplib::Request&, httplib::Response& res) {
        auto config = decoder_manager.get_osd_config();
        res.set_content(create_osd_config_json(config), "application/json");
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
