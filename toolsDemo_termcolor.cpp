#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <iomanip>
#include <cmath>
#include <ctime>
#include <string>
#include <sstream>
#include <algorithm>
#include <random>
#include "termcolor.hpp" // 确保该文件在同一目录下

using namespace std;
using namespace std::this_thread;
using namespace std::chrono;

// --- 类型定义：修复三元运算符重载歧义 ---
typedef std::ostream& (*ColorFunc)(std::ostream&);

// ==========================================
// 辅助函数和常量定义
// ==========================================
const vector<string> LOG_MESSAGES = {
    "NPU引擎初始化完成",
    "ISP流水线已就绪",
    "检测到人脸目标",
    "目标跟踪稳定",
    "温度传感器正常",
    "网络连接稳定",
    "数据流传输正常",
    "AI推理中...",
    "模型加载完成",
    "边缘计算节点同步",
    "加密通道建立",
    "系统自检通过"
};

const vector<string> ALERT_MESSAGES = {
    "温度过高警告",
    "内存使用率超过阈值",
    "网络延迟增加",
    "NPU负载过重",
    "磁盘空间不足",
    "检测到异常数据包",
    "系统响应延迟"
};

// 随机数生成器
mt19937 rng(time(0));

// 生成随机整数
int random_int(int min, int max) {
    uniform_int_distribution<int> dist(min, max);
    return dist(rng);
}

// 生成随机浮点数
float random_float(float min, float max) {
    uniform_real_distribution<float> dist(min, max);
    return dist(rng);
}

// ==========================================
// 1. 增强的基础组件模块
// ==========================================

// 彩虹色加载条
void print_rainbow_loading(string task, int steps) {
    cout << termcolor::white << "  " << task << " [";
    const vector<ColorFunc> rainbow = {
        (ColorFunc)termcolor::red,
        (ColorFunc)termcolor::yellow,
        (ColorFunc)termcolor::green,
        (ColorFunc)termcolor::cyan,
        (ColorFunc)termcolor::blue,
        (ColorFunc)termcolor::magenta
    };
    
    for (int i = 0; i < steps; ++i) {
        ColorFunc color = rainbow[i % rainbow.size()];
        cout << color << "█";
        cout.flush();
        sleep_for(milliseconds(30 + random_int(-10, 10)));
    }
    cout << termcolor::white << "] " << termcolor::bold << termcolor::cyan << "✓ DONE" << termcolor::reset << endl;
}

// 脉冲式加载动画
void print_pulse_loading(string task, int duration_ms) {
    cout << termcolor::white << "  " << task << " ";
    auto start = high_resolution_clock::now();
    
    while (duration_cast<milliseconds>(high_resolution_clock::now() - start).count() < duration_ms) {
        for (int i = 0; i < 10; i++) {
            string pulse = string(i, '▓') + string(10-i, '░');
            cout << "\r" << termcolor::white << "  " << task << " [" 
                 << termcolor::magenta << pulse << termcolor::white << "]";
            cout.flush();
            sleep_for(milliseconds(50));
        }
    }
    cout << "\r" << termcolor::white << "  " << task << " ["
         << termcolor::green << "██████████" << termcolor::white << "] "
         << termcolor::bold << termcolor::green << "✓ COMPLETE" << termcolor::reset << endl;
}

// ==========================================
// 2. 增强的动态 UI 组件
// ==========================================

// 3D立体波形效果
void print_3d_wave(int frame) {
    const vector<string> blocks = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    const int layers = 3;
    
    for (int layer = layers-1; layer >= 0; layer--) {
        if (layer == 2) cout << termcolor::blue << " [3D_WAVE] " << termcolor::reset;
        else cout << "           ";
        
        for (int i = 0; i < 20; ++i) {
            // 多层波形，每层相位不同
            double phase = frame * 0.3 + i * 0.4 - layer * 0.5;
            int height = static_cast<int>(4.0 + 3.0 * sin(phase) + layer);
            
            // 每层使用不同颜色
            if (layer == 0) cout << termcolor::cyan;
            else if (layer == 1) cout << termcolor::blue;
            else cout << termcolor::bright_blue;
            
            if (height >= 0 && height < static_cast<int>(blocks.size())) {
                cout << blocks[height];
            } else {
                cout << " ";
            }
        }
        cout << termcolor::reset << endl;
    }
}

// 火焰效果热力图
void print_flame_cores(int frame) {
    cout << " | " << termcolor::bright_red << "FLAME_CORES:" << termcolor::reset;
    
    const vector<pair<ColorFunc, string>> flame_colors = {
        {(ColorFunc)termcolor::on_red, "🔥"},
        {(ColorFunc)termcolor::on_yellow, "🔥"},
        {(ColorFunc)termcolor::on_white, "⚪"},
        {(ColorFunc)termcolor::on_grey, "⬤"}
    };
    
    for (int i = 0; i < 8; ++i) {
        int state = (frame * 2 + i * 3) % 20;
        if (state < 3) {
            cout << termcolor::on_red << termcolor::yellow << "🔥" << termcolor::reset;
        } else if (state < 8) {
            cout << termcolor::on_yellow << termcolor::red << "🔥" << termcolor::reset;
        } else if (state < 15) {
            cout << termcolor::on_white << termcolor::grey << "⚪" << termcolor::reset;
        } else {
            cout << termcolor::on_grey << "  " << termcolor::reset;
        }
        cout << " ";
    }
}

// 全息风格进度条
void hologram_bar(string label, int percent, ColorFunc color, bool show_sparkle = false) {
    int width = 25;
    int filled = (percent * width) / 100;
    
    // 标签带发光效果
    cout << termcolor::bold << termcolor::bright_white << "⟢ " 
         << termcolor::reset << left << setw(18) << label << " ";
    
    // 进度条开始符号
    cout << termcolor::bright_cyan << "⟦" << termcolor::reset;
    
    // 进度条主体
    for (int i = 0; i < width; ++i) {
        if (i < filled) {
            cout << color << "▉";
            // 闪烁效果
            if (show_sparkle && random_int(0, 100) < 5) {
                cout << termcolor::bright_white << "✦" << termcolor::reset;
                i++; // 跳过一格
            }
        } else {
            // 未填充部分用渐变
            int shade = 240 - (i * 10 / width) * 30;
            cout << termcolor::grey << "░";
        }
    }
    
    // 进度条结束符号和百分比
    cout << termcolor::bright_cyan << "⟧" << termcolor::reset
         << " " << termcolor::bold;
    
    // 根据百分比使用不同颜色
    if (percent < 30) cout << termcolor::green;
    else if (percent < 70) cout << termcolor::yellow;
    else cout << termcolor::red;
    
    cout << setw(3) << percent << "%" << termcolor::reset << endl;
}

// 数字仪表盘
void digital_gauge(string label, float value, float min, float max, string unit) {
    float percentage = ((value - min) / (max - min)) * 100;
    int width = 20;
    int pos = (percentage * width) / 100;
    
    cout << termcolor::bold << termcolor::bright_white << "⟣ " 
         << termcolor::reset << left << setw(15) << label << " [";
    
    for (int i = 0; i < width; ++i) {
        if (i < pos) {
            // 根据位置使用渐变色
            if (i < width/3) cout << termcolor::green;
            else if (i < 2*width/3) cout << termcolor::yellow;
            else cout << termcolor::red;
            cout << "█";
        } else {
            cout << termcolor::grey << "░";
        }
    }
    
    cout << termcolor::reset << "] " << termcolor::bold;
    
    // 数值颜色
    if (value < (min + max) / 3) cout << termcolor::green;
    else if (value < 2 * (min + max) / 3) cout << termcolor::yellow;
    else cout << termcolor::red;
    
    cout << fixed << setprecision(1) << value << unit << termcolor::reset << endl;
}

// 旋转图标动画
void print_spinning_icon(int frame, string message) {
    const vector<string> spin_chars = {"◐", "◓", "◑", "◒"};
    string spin_char = spin_chars[frame % spin_chars.size()];
    
    ColorFunc spin_color;
    switch (frame % 4) {
        case 0: spin_color = (ColorFunc)termcolor::cyan; break;
        case 1: spin_color = (ColorFunc)termcolor::magenta; break;
        case 2: spin_color = (ColorFunc)termcolor::yellow; break;
        case 3: spin_color = (ColorFunc)termcolor::green; break;
    }
    
    cout << " " << spin_color << spin_char << termcolor::reset 
         << " " << message << endl;
}

// 径向菜单
void print_radial_menu(int selection) {
    const vector<string> menu_items = {"DIAGNOSTIC", "CONTROL", "MONITOR", "CONFIG", "LOG"};
    const vector<ColorFunc> menu_colors = {
        (ColorFunc)termcolor::red,
        (ColorFunc)termcolor::yellow,
        (ColorFunc)termcolor::green,
        (ColorFunc)termcolor::blue,
        (ColorFunc)termcolor::magenta
    };
    
    cout << termcolor::bright_white << " ⚙ RADIAL MENU:" << termcolor::reset << endl;
    
    for (int i = 0; i < menu_items.size(); ++i) {
        cout << "   ";
        if (i == selection) {
            cout << termcolor::blink << termcolor::on_white << termcolor::grey;
        } else {
            cout << menu_colors[i];
        }
        cout << " [" << menu_items[i] << "] " << termcolor::reset;
    }
    cout << endl << endl;
}

// 数据流瀑布图
void print_data_waterfall(int frame, int lines = 5) {
    cout << termcolor::cyan << " [DATA_STREAM]" << termcolor::reset << endl;
    
    for (int line = 0; line < lines; ++line) {
        cout << "  ";
        for (int i = 0; i < 40; ++i) {
            int value = (frame + line * 7 + i * 3) % 256;
            char display_char;
            
            if (value < 64) display_char = ' ';
            else if (value < 128) display_char = '.';
            else if (value < 192) display_char = ':';
            else display_char = '#';
            
            // 根据值使用不同颜色
            if (value < 85) cout << termcolor::blue;
            else if (value < 170) cout << termcolor::cyan;
            else cout << termcolor::bright_cyan;
            
            cout << display_char;
        }
        cout << termcolor::reset << endl;
    }
}

// ==========================================
// 3. 系统状态监控模块
// ==========================================

// 网络连接状态图
void print_network_map(int frame) {
    const vector<string> nodes = {"EDGE", "CLOUD", "NPU", "ISP", "AI", "DATA"};
    
    cout << termcolor::bright_white << " NETWORK TOPOLOGY:" << termcolor::reset << endl;
    cout << "  ";
    
    for (size_t i = 0; i < nodes.size(); ++i) {
        // 随机连接状态
        bool connected = ((frame + i) % 10) > 2;
        
        if (connected) {
            cout << termcolor::green << "●" << termcolor::bright_green << nodes[i];
            
            // 显示连接线
            if (i < nodes.size() - 1) {
                if (((frame + i) % 20) > 10) {
                    cout << termcolor::bright_green << "═══";
                } else {
                    cout << termcolor::green << "───";
                }
            }
        } else {
            cout << termcolor::red << "○" << termcolor::bright_red << nodes[i];
            if (i < nodes.size() - 1) cout << termcolor::dark << "···";
        }
        cout << termcolor::reset << " ";
    }
    cout << endl;
}

// 温度计式温度显示
void print_thermometer(float temp) {
    int height = 8;
    int temp_level = static_cast<int>((temp - 20) / 10 * height);
    
    cout << termcolor::bright_white << " TEMPERATURE:" << termcolor::reset << endl;
    cout << "  ┌─┐" << endl;
    
    for (int i = height; i >= 0; --i) {
        cout << "  │";
        
        if (i <= temp_level) {
            if (temp < 40) cout << termcolor::green;
            else if (temp < 60) cout << termcolor::yellow;
            else cout << termcolor::red;
            cout << "██";
        } else {
            cout << termcolor::grey << "░░";
        }
        
        cout << termcolor::reset << "│";
        
        if (i == height) cout << " 100°C";
        else if (i == height/2) cout << "  50°C";
        else if (i == 0) cout << "  20°C";
        
        cout << endl;
    }
    
    cout << "  └─┘  Current: " << termcolor::bold;
    if (temp < 40) cout << termcolor::green;
    else if (temp < 60) cout << termcolor::yellow;
    else cout << termcolor::red;
    cout << temp << "°C" << termcolor::reset << endl;
}

// ==========================================
// 主演示控制中心
// ==========================================
int main() {
    srand(static_cast<unsigned int>(time(NULL)));
    rng.seed(time(0));
    
    // 清屏并显示启动画面
    cout << "\033[2J\033[1;1H"; // 清屏并移动光标到左上角
    
    // --- 启动动画 ---
    cout << termcolor::bold << termcolor::cyan << "\n╔══════════════════════════════════════════════════════╗" << endl;
    cout << "║      CYBER MONITOR CONTROL CENTER v6.0      ║" << endl;
    cout << "║     Advanced Holographic Interface          ║" << endl;
    cout << "╚══════════════════════════════════════════════════════╝" << termcolor::reset << endl;
    
    sleep_for(milliseconds(500));
    
    // --- 第一阶段：增强的系统启动 ---
    cout << termcolor::bold << termcolor::bright_cyan << "\n>>> STAGE 1: ENHANCED KERNEL BOOT SEQUENCE" << termcolor::reset << endl;
    cout << termcolor::bright_white << string(60, '─') << termcolor::reset << endl;
    
    print_rainbow_loading("Loading Quantum_Kernel     ", 15);
    print_pulse_loading("Initializing Neural Core   ", 1200);
    print_rainbow_loading("Calibrating Sensors       ", 12);
    print_pulse_loading("Establishing Secure Link   ", 800);
    print_rainbow_loading("Syncing Edge Nodes        ", 18);
    
    sleep_for(milliseconds(800));
    
    // --- 第二阶段：增强的系统快照 ---
    cout << termcolor::bold << termcolor::bright_cyan << "\n>>> STAGE 2: COMPREHENSIVE SYSTEM SNAPSHOT" << termcolor::reset << endl;
    cout << termcolor::on_bright_white << termcolor::grey << termcolor::bold
         << left << setw(20) << " SERVICE" 
         << left << setw(12) << " PORT" 
         << left << setw(15) << " STATUS" 
         << left << setw(15) << " LOAD" << termcolor::reset << endl;
    
    vector<tuple<string, string, string, int>> services = {
        {"sshd_rv1126", "22", "ONLINE", 15},
        {"frpc_tunnel", "6022", "SECURE", 40},
        {"rk_npu_srv", "--", "ACTIVE", 85},
        {"ai_inference", "8080", "RUNNING", 92},
        {"data_stream", "9000", "STREAMING", 68},
        {"monitor_daemon", "--", "WATCHING", 12}
    };
    
    for (const auto& [name, port, status, load] : services) {
        cout << left << setw(20) << name 
             << left << setw(12) << port;
             
        if (status == "ONLINE" || status == "SECURE") {
            cout << termcolor::green << termcolor::bold;
        } else if (status == "ACTIVE" || status == "RUNNING") {
            cout << termcolor::yellow << termcolor::bold;
        } else {
            cout << termcolor::cyan;
        }
        
        cout << left << setw(15) << status << termcolor::reset;
        
        // 负载指示器
        cout << "[";
        for (int i = 0; i < 10; i++) {
            if (i < load / 10) {
                if (load < 50) cout << termcolor::green;
                else if (load < 80) cout << termcolor::yellow;
                else cout << termcolor::red;
                cout << "█";
            } else {
                cout << termcolor::grey << "░";
            }
        }
        cout << termcolor::reset << "]" << endl;
    }
    
    // 显示网络拓扑
    cout << endl;
    print_network_map(0);
    
    sleep_for(seconds(1));
    
    // --- 第三阶段：终极赛博监控面板 ---
    cout << termcolor::bold << termcolor::bright_cyan << "\n>>> STAGE 3: HOLOGRAPHIC MONITORING PANEL" << termcolor::reset << endl;
    cout << termcolor::on_bright_blue << termcolor::bright_white << termcolor::bold
         << "  QUANTUM MONITOR v6.0 - REAL-TIME CYBER DASHBOARD  " << termcolor::reset << endl;
    
    int selection = 0;
    int alert_counter = 0;
    
    for (int frame = 0; frame < 300; ++frame) {
        // 清屏并重新绘制整个界面
        if (frame > 0) {
            cout << "\033[2J\033[1;1H"; // 清屏
            cout << termcolor::bold << termcolor::cyan << "╔══════════════════════════════════════════════════════╗" << endl;
            cout << "║      LIVE MONITORING - FRAME " << setw(4) << frame << "          ║" << endl;
            cout << "╚══════════════════════════════════════════════════════╝" << termcolor::reset << endl;
        }
        
        // 1. 进度条区域
        cout << endl;
        hologram_bar("NPU_ENGINE", 60 + (rand() % 35), (ColorFunc)termcolor::magenta, true);
        
        int temp = 45 + (frame % 40);
        ColorFunc tCol = (temp > 75) ? (ColorFunc)termcolor::red : 
                        (temp > 60) ? (ColorFunc)termcolor::yellow : 
                        (ColorFunc)termcolor::green;
        hologram_bar("SOC_THERMAL", temp, tCol);
        
        hologram_bar("MEMORY", 30 + (rand() % 60), (ColorFunc)termcolor::cyan);
        hologram_bar("NETWORK", 40 + (rand() % 50), (ColorFunc)termcolor::blue);
        hologram_bar("STORAGE", 20 + (rand() % 70), (ColorFunc)termcolor::yellow);
        
        // 数字仪表盘
        cout << endl;
        digital_gauge("CPU_FREQ", 1.8 + sin(frame * 0.1) * 0.5, 1.0, 3.0, "GHz");
        digital_gauge("POWER", 12.5 + sin(frame * 0.2) * 2.0, 10.0, 15.0, "W");
        digital_gauge("BANDWIDTH", 45.0 + sin(frame * 0.15) * 15.0, 10.0, 100.0, "MB/s");
        
        cout << endl << termcolor::grey << string(70, '─') << termcolor::reset << endl;
        
        // 2. 动态图形区域
        print_3d_wave(frame);
        cout << endl;
        print_flame_cores(frame);
        cout << endl << endl;
        
        // 3. 温度计显示
        if (frame % 50 < 25) {
            print_thermometer(temp);
        } else {
            print_data_waterfall(frame, 4);
        }
        
        cout << endl << termcolor::grey << string(70, '─') << termcolor::reset << endl;
        
        // 4. 实时日志和状态区域
        cout << termcolor::bright_white << " SYSTEM STATUS:" << termcolor::reset << endl;
        
        // 旋转图标动画
        print_spinning_icon(frame, LOG_MESSAGES[frame % LOG_MESSAGES.size()]);
        
        // 警告消息（随机出现）
        if (alert_counter <= 0 && random_int(0, 100) < 10) {
            cout << termcolor::blink << termcolor::on_red << termcolor::bright_white 
                 << " ⚠ ALERT: " << ALERT_MESSAGES[random_int(0, ALERT_MESSAGES.size()-1)] 
                 << " ⚠ " << termcolor::reset << endl;
            alert_counter = 15; // 显示15帧
        } else if (alert_counter > 0) {
            cout << termcolor::blink << termcolor::on_red << termcolor::bright_white 
                 << " ⚠ ALERT: " << ALERT_MESSAGES[(frame/2) % ALERT_MESSAGES.size()] 
                 << " ⚠ " << termcolor::reset << endl;
            alert_counter--;
        }
        
        // 5. 径向菜单（随时间变化选择）
        if (frame % 100 < 25) {
            cout << endl;
            selection = (frame / 20) % 5;
            print_radial_menu(selection);
        }
        
        // 6. 系统页脚
        cout << termcolor::grey << "┌────────────────────────────────────────────────────┐" << endl;
        cout << "│ Uptime: " << setw(6) << fixed << setprecision(1) << (frame * 0.12) << "s"
             << " | Hair: " << (frame % 100 < 80 ? "Stable" : "Fluctuating")
             << " | Threats: " << (frame % 200 < 50 ? "None" : "Low") << "   │" << endl;
        cout << "│ Ports: 22/6022/8080 | Nodes: 8 | Frame: " << setw(4) << frame 
             << " | FPS: " << setw(3) << (80 + frame % 20) << "  │" << endl;
        cout << "└────────────────────────────────────────────────────┘" << termcolor::reset;
        
        cout.flush();
        sleep_for(milliseconds(120));
        
        // 每30帧显示一个提示
        if (frame % 30 == 29) {
            cout << termcolor::bright_yellow << "\n💡 Tip: Press Ctrl+C to exit the monitoring panel" 
                 << termcolor::reset << endl;
            sleep_for(milliseconds(500));
        }
    }
    
    // --- 结束动画 ---
    cout << "\n\n" << termcolor::bold << termcolor::bright_cyan;
    for (int i = 0; i < 5; i++) {
        cout << "█▓▒░ SHUTTING DOWN MONITORING SYSTEM ░▒▓█" << endl;
        cout << "\033[1A"; // 上移一行
        sleep_for(milliseconds(200));
    }
    
    cout << termcolor::green << "\n✓ Monitoring session completed successfully!" << termcolor::reset << endl;
    cout << termcolor::bright_white << "⏱  Duration: 36.0s | Frames: 300 | Alerts: 3" << termcolor::reset << endl;
    cout << termcolor::grey << "System returning to standby mode..." << termcolor::reset << endl;
    
    return 0;
}