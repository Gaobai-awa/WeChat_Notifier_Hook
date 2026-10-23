// WeChat Notifier - Hook 版
// ============================================================
// 消息来源：WeChat-Hook (version.dll) 的 HTTP 回调推送
// 不再依赖：CipherTalk API / session_id / 数据库 key / 内存偏移
//
// 架构变化：
//   旧版 = 主动轮询 CipherTalk:5031 /v1/messages，靠 createTime 去重
//   新版 = 内置 HTTP 服务器接收 hook 的 POST 推送，靠 msgid 去重
//
// 数据流：
//   Weixin.exe (version.dll hook)
//        └─ POST http://127.0.0.1:<listen_port>/recv
//              └─ 本程序解析 → 过滤 → 匹配 → 系统通知
// ============================================================

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <string>
#include <vector>
#include <deque>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <set>
#include <filesystem>

#include "json.hpp"
#include "httplib.h"

namespace fs = std::filesystem;

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "runtimeobject.lib")

using json = nlohmann::json;
using namespace Microsoft::WRL;
using namespace std::chrono_literals;

// ==================== i18n ====================
std::string g_lang = "zh_cn";
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> D = {
    {"zh_cn", {
        {"banner","WeChat Notifier (Hook)"},{"target","监听"},{"listen","监听端口"},
        {"hook_api","Hook 地址"},{"autostart","开机自启动"},
        {"silent_mode","静默运行"},{"language","语言"},
        {"started","已启动"},{"ready","Hook 已连接"},{"disconnected","Hook 未连接"},
        {"waiting","等待消息中..."},{"exit","退出"},
        {"created","config.json 已创建，请编辑后重启。"},{"no_users","未配置监听对象！"},
        {"tray_tip","WeChat Notifier - 右键菜单"},{"cfg_saved","配置已保存"},
        {"status","状态"},{"retry","重连中..."},
        {"watch_scope","监听范围"},
        {"scope_all","所有人"},{"scope_private","仅私聊"},{"scope_config","Config 内用户"},
        {"image_preview","图片通知"},{"img_on","开"},{"img_off","关"},{"wx_root","微信数据"},
    }},
    {"zh_tw", {
        {"banner","WeChat Notifier (Hook)"},{"target","監聽"},{"listen","監聽埠"},
        {"hook_api","Hook 位址"},{"autostart","開機自啟動"},
        {"silent_mode","靜默執行"},{"language","語言"},
        {"started","已啟動"},{"ready","Hook 已連線"},{"disconnected","Hook 未連線"},
        {"waiting","等待訊息中..."},{"exit","退出"},
        {"created","config.json 已建立，請編輯後重啟。"},{"no_users","未設定監聽對象！"},
        {"tray_tip","WeChat Notifier - 右鍵選單"},{"cfg_saved","設定已儲存"},
        {"status","狀態"},{"retry","重新連線中..."},
        {"watch_scope","監聽範圍"},
        {"scope_all","所有人"},{"scope_private","僅私聊"},{"scope_config","Config 內用戶"},
        {"image_preview","圖片通知"},{"img_on","開"},{"img_off","關"},{"wx_root","微信資料"},
    }},
    {"en_us", {
        {"banner","WeChat Notifier (Hook)"},{"target","target"},{"listen","listen port"},
        {"hook_api","hook url"},{"autostart","autostart"},
        {"silent_mode","silent mode"},{"language","language"},
        {"started","started"},{"ready","Hook connected"},{"disconnected","Hook disconnected"},
        {"waiting","waiting for messages..."},{"exit","Exit"},
        {"created","config.json created. Edit and restart."},{"no_users","No target configured!"},
        {"tray_tip","WeChat Notifier - right-click menu"},{"cfg_saved","Config saved"},
        {"status","status"},{"retry","reconnecting..."},
        {"watch_scope","watch scope"},
        {"scope_all","everyone"},{"scope_private","private only"},{"scope_config","config users"},
        {"image_preview","image preview"},{"img_on","on"},{"img_off","off"},{"wx_root","wechat data"},
    }},
};
std::string _(const std::string& k) { auto& d = D.count(g_lang) ? D[g_lang] : D["en_us"]; return d.count(k) ? d[k] : k; }

// ==================== Globals ====================
std::string g_exeDir;
json g_cfg;
const wchar_t* AUMID = L"WeChat";
std::atomic<bool> g_running(true), g_silent(false);
bool g_quietStart = false;
std::atomic<bool> g_autostart{false};
std::atomic<bool> g_hookOnline{false};
HWND g_trayHwnd = nullptr;
NOTIFYICONDATAW g_nid = {};

// hook 配置
std::string g_hookHost = "127.0.0.1";
int         g_hookPort = 30001;
int         g_listenPort = 8899;
std::wstring g_callbackUrl;
// 默认全部类型都通知：文本 1、图片 3、语音 34、好友申请 37、名片 42、
// 视频 43、表情 47、位置 48、卡片 49、通话 50、视频(另一路) 62、
// 系统消息 10000/10002。51 是同步信令，在 handle_push 里单独丢弃。
std::unordered_set<int> g_notifyTypes = {
    1, 3, 34, 37, 42, 43, 47, 48, 49, 50, 62, 10000, 10002
};
std::unordered_map<std::string, std::string> g_memberNames;  // wxid -> 昵称（群成员用）

// ==================== 图片预览 ====================
// 微信 4.x 的图片本体在 msg\attach\<md5(会话)>\<yyyy-MM>\Img\*.dat，是加密的，
// 而且每台机器、每个账号的密钥都不一样，所以程序根本不碰密钥，只读微信自己解好的：
//   1. cache\<月>\Message\<md5>\Thumb\*_thumb.jpg —— 明文缩略图，
//      但只有微信在界面里渲染过这张图才会生成，不能指望
//   2. cache\<月>\Message\<md5>\ImageTemp\<序号>_<Unix秒>_mid_temp_convert ——
//      微信解好的 wxgf(HEVC) 预览。文件名里的秒 == 消息 timestamp，
//      文件大小 == 推送正文里的 hevc_mid_size，两个特征就能唯一定位到这条消息自己的图。
//      这是主力路径：剥掉 wxgf 头，交给 ffmpeg 取一帧存成 JPEG
//   3. hook 的 /Decode_Pic（不同 hook 版本行为不同，兜底）
std::atomic<bool> g_imgPreview{true};
std::atomic<bool> g_dbgPayload{false};    // 把原始推送写进 payload.log，排查用
std::wstring g_imgDir;                    // 自己管的图片缓存（Toast 引用期间必须存在）
std::vector<std::wstring> g_wxRoots;      // ...\xwechat_files\<wxid>
std::string  g_imgAesKey;                 // 图片 dat 的 AES-128 key（16 字符），来自 config
std::mutex g_imgMtx;

// 已经附给某条通知的预览文件。微信的 ImageTemp 里一份文件对应一张图，
// 用过的不能再给下一条消息用 —— 否则重试/并发时会拿上一条的图顶上。
std::mutex g_usedMtx;
std::set<std::wstring> g_usedConv;

static bool conv_used(const std::wstring& p) {
    std::lock_guard<std::mutex> lk(g_usedMtx);
    return g_usedConv.count(p) > 0;
}
static void conv_mark_used(const std::wstring& p) {
    std::lock_guard<std::mutex> lk(g_usedMtx);
    if (g_usedConv.size() > 512) g_usedConv.clear();
    g_usedConv.insert(p);
}

// ==================== 监听范围 ====================
// ALL      = 所有人（群聊 + 私聊 + 公众号全收）
// PRIVATE  = 仅私聊（roomid 为空，且排除公众号/服务号）
// CONFIG   = 只收 config.json 里 users 列出的对象（原有行为）
enum class WatchMode { ALL = 0, PRIVATE = 1, CONFIG = 2 };
std::atomic<int> g_watchMode{(int)WatchMode::CONFIG};

// 判定是不是公众号/服务号（无法读联系人库，只能按 wxid 前缀猜）
// gh_       公众号
// wxid_ 后接特殊前缀的官方号也一并排除，避免刷屏
bool is_mass_account(const std::string& wxid) {
    if (wxid.rfind("gh_", 0) == 0) return true;
    if (wxid.rfind("mphelper", 0) == 0) return true;   // 公众平台助手
    return false;
}

struct WatchTarget {
    std::string id;        // wxid 或 xxxxx@chatroom
    std::string name;      // 显示名
    bool        isGroup = false;
};
std::unordered_map<std::string, WatchTarget> g_watch;

// 去重
std::mutex g_seenMtx;
std::deque<uint64_t> g_seenOrder;
std::unordered_set<uint64_t> g_seenSet;

httplib::Server* g_svr = nullptr;

// ==================== UTF-8 ====================
std::wstring u8tow(const std::string& s) {
    if (s.empty()) return L"";
    int l = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(l, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], l);
    w.resize(l - 1);
    return w;
}
std::string wtou8(const std::wstring& w) {
    if (w.empty()) return "";
    int l = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(l, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], l, nullptr, nullptr);
    s.resize(l - 1);
    return s;
}

// 按字符安全截断（不切坏 UTF-8）
std::string clamp_utf8(const std::string& s, size_t maxChars) {
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < maxChars) {
        unsigned char c = (unsigned char)s[i];
        size_t step = 1;
        if ((c & 0x80) == 0x00) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (i + step > s.size()) break;
        i += step;
        chars++;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "...";
}

// ==================== Config ====================
std::string gen_config_template() {
    return std::string("{\n") +
        "  // WeChat-Hook 的 HTTP 服务地址（version.dll 注入后监听）\n"
        "  \"hook_api\": \"http://127.0.0.1:30001\",\n\n"
        "  // 本程序接收推送的端口，必须与 hook 回调地址一致\n"
        "  \"listen_port\": 8899,\n\n"
        "  // 监听范围：0=所有人  1=仅私聊  2=Config 内用户\n"
        "  // 托盘右键菜单里也能切换\n"
        "  \"watch_mode\": 2,\n\n"
        "  // 要监听的对象：wxid（私聊）或 xxxxx@chatroom（群聊）\n"
        "  // 只在 watch_mode=2 时生效\n"
        "  \"users\": [\n"
        "    { \"wxid\": \"wxid_xxxxxxxxxxxx\", \"display_name\": \"好友昵称\", \"is_group\": false }\n"
        "  ],\n\n"
        "  // 要通知的消息类型：\n"
        "  //   1=文本 3=图片 34=语音 37=好友申请 42=名片 43=视频\n"
        "  //   47=表情 48=位置 49=卡片 50=通话 62=视频(另一路) 10000/10002=系统\n"
        "  // 留着全部即可；想只收文本就改成 [1]\n"
        "  \"notify_types\": [1, 3, 34, 37, 42, 43, 47, 48, 49, 50, 62, 10000, 10002],\n\n"
        "  // 图片消息：在通知里直接显示图片\n"
        "  \"image_preview\": true,\n\n"
        "  // 微信的图片预览是 wxgf(HEVC) 格式，需要 ffmpeg 取一帧转成 JPEG。\n"
        "  // 留空则依次找：程序目录\\ffmpeg.exe、程序目录\\bin\\ffmpeg.exe、PATH\n"
        "  \"ffmpeg_path\": \"\",\n\n"
        "  // 排查用：把收到的原始推送写进程序目录的 payload.log\n"
        "  \"debug_payload\": false,\n\n"
        "  // 微信数据目录，留空自动探测（一般不用填）\n"
        "  \"wx_data_root\": \"\",\n\n"
        "  // 开机自动启动\n"
        "  \"autostart\": false,\n\n"
        "  // 静默运行（隐藏控制台）\n"
        "  \"silent_mode\": false,\n\n"
        "  // 语言：zh_cn / zh_tw / en_us\n"
        "  \"language\": \"zh_cn\",\n\n"
        "  // 群成员 wxid -> 昵称（可选，用于群消息显示发送者名字）\n"
        "  \"members\": {}\n"
        "}\n";
}

std::string strip_comments(const std::string& r) {
    std::string o;
    std::istringstream s(r);
    std::string l;
    while (std::getline(s, l)) {
        auto p = l.find_first_not_of(" \t\r");
        if (p != std::string::npos && l.substr(p, 2) == "//") continue;
        o += l + "\n";
    }
    return o;
}

// 配置文件唯一权威位置 = exe 同目录。
// 开发时源码目录下有一份 Hook\config.json，首次运行会复制过去，
// 之后只认 exe 同目录的那份，避免"改了源码目录的配置却不生效"。
std::string g_cfgPath;

std::string find_config() {
    return g_exeDir + "config.json";
}

json load_config() {
    g_cfgPath = find_config();

    if (GetFileAttributesA(g_cfgPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 先尝试从源码目录（上三级）复制一份，方便开发调试
        std::string src = g_exeDir + "..\\..\\..\\config.json";
        if (GetFileAttributesA(src.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::ifstream in(src, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            std::ofstream out(g_cfgPath, std::ios::binary);
            out << content;
            printf("[CFG] copied from repo: %s -> %s\n", src.c_str(), g_cfgPath.c_str());
        } else {
            std::ofstream o(g_cfgPath, std::ios::binary);
            o << gen_config_template();
            o.close();
            printf("[CFG] %s\n%s\n", _("created").c_str(), g_cfgPath.c_str());
            MessageBoxW(NULL, u8tow(_("created")).c_str(), L"WeChat Notifier", MB_OK);
            exit(1);
        }
    }

    std::ifstream f(g_cfgPath);
    if (!f.is_open()) {
        printf("[E] cannot open config: %s\n", g_cfgPath.c_str());
        MessageBoxW(NULL, u8tow("无法打开配置文件:\n" + g_cfgPath).c_str(), L"WeChat Notifier", MB_ICONERROR);
        exit(1);
    }
    std::string r((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    printf("[CFG] loaded: %s\n", g_cfgPath.c_str());
    return json::parse(strip_comments(r));
}

void save_config() {
    if (g_cfgPath.empty()) g_cfgPath = g_exeDir + "config.json";
    std::ofstream f(g_cfgPath);
    f << g_cfg.dump(4) << "\n";
}

// hook_api 字符串 → host / port
bool parse_url(const std::string& url, std::string& host, int& port) {
    size_t s = url.find("://");
    size_t st = (s != std::string::npos) ? s + 3 : 0;
    size_t slash = url.find('/', st);
    std::string hp = (slash == std::string::npos) ? url.substr(st) : url.substr(st, slash - st);
    size_t colon = hp.find(':');
    if (colon == std::string::npos) { host = hp; port = 80; }
    else {
        host = hp.substr(0, colon);
        try { port = std::stoi(hp.substr(colon + 1)); } catch (...) { return false; }
    }
    return !host.empty() && port > 0 && port <= 65535;
}

// ==================== Forward declarations ====================
void setup_autostart(bool en);

// ==================== Tray Icon ====================
#define WM_TRAY (WM_USER + 1)
#define ID_EXIT      1001
#define ID_AUTO      1003
#define ID_SILENT    1004
#define ID_STATUS    1005
// 监听范围二级菜单
#define ID_SCOPE_ALL     1101
#define ID_SCOPE_PRIVATE 1102
#define ID_SCOPE_CONFIG  1103

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_TRAY && LOWORD(lp) == WM_RBUTTONUP) {
        POINT pt; GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();

        // 状态行（灰显不可点）
        std::string status = g_hookOnline ? (_("status") + ": " + _("ready"))
                                          : (_("status") + ": " + _("disconnected"));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_STATUS, u8tow(status).c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        // --- 监听范围子菜单 ---
        HMENU scope = CreatePopupMenu();
        int cur = g_watchMode.load();
        AppendMenuW(scope, MF_STRING | (cur == (int)WatchMode::ALL     ? MF_CHECKED : MF_UNCHECKED),
                    ID_SCOPE_ALL,     u8tow(_("scope_all")).c_str());
        AppendMenuW(scope, MF_STRING | (cur == (int)WatchMode::PRIVATE ? MF_CHECKED : MF_UNCHECKED),
                    ID_SCOPE_PRIVATE, u8tow(_("scope_private")).c_str());
        AppendMenuW(scope, MF_STRING | (cur == (int)WatchMode::CONFIG  ? MF_CHECKED : MF_UNCHECKED),
                    ID_SCOPE_CONFIG,  u8tow(_("scope_config")).c_str());
        AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)scope, u8tow(_("watch_scope")).c_str());

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (g_autostart ? MF_CHECKED : MF_UNCHECKED), ID_AUTO,   u8tow(_("autostart")).c_str());
        AppendMenuW(menu, MF_STRING | (g_silent    ? MF_CHECKED : MF_UNCHECKED), ID_SILENT, u8tow(_("silent_mode")).c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_EXIT, u8tow(_("exit")).c_str());

        SetForegroundWindow(h);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
        DestroyMenu(menu);

        if (cmd == ID_EXIT) { g_running = false; PostQuitMessage(0); }
        else if (cmd == ID_SCOPE_ALL || cmd == ID_SCOPE_PRIVATE || cmd == ID_SCOPE_CONFIG) {
            WatchMode nm = (cmd == ID_SCOPE_ALL)     ? WatchMode::ALL
                         : (cmd == ID_SCOPE_PRIVATE) ? WatchMode::PRIVATE
                                                     : WatchMode::CONFIG;
            g_watchMode = (int)nm;
            g_cfg["watch_mode"] = (int)nm;
            save_config();

            std::string label = (nm == WatchMode::ALL)     ? _("scope_all")
                              : (nm == WatchMode::PRIVATE) ? _("scope_private")
                                                           : _("scope_config");
            std::string msg = _("cfg_saved") + " (" + _("watch_scope") + ": " + label + ")";
            g_nid.szInfo[0] = 0;
            wcsncpy_s(g_nid.szInfo, u8tow(msg).c_str(), 255);
            g_nid.uFlags |= NIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            printf("[*] %s -> %s\n", _("watch_scope").c_str(), label.c_str());
            fflush(stdout);
        }
        else if (cmd == ID_AUTO || cmd == ID_SILENT) {
            std::string key;
            if (cmd == ID_AUTO) {
                bool v = !g_autostart; g_autostart = v; g_cfg["autostart"] = v; key = "autostart";
                setup_autostart(v);
            } else {
                bool v = !g_silent.load(); g_cfg["silent_mode"] = v; key = "silent_mode";
                if (!v) {
                    // 关闭静默 → 重新以可见窗口启动自己
                    wchar_t exe[MAX_PATH], dir[MAX_PATH];
                    GetModuleFileNameW(NULL, exe, MAX_PATH);
                    GetCurrentDirectoryW(MAX_PATH, dir);
                    save_config();
                    ShellExecuteW(NULL, L"open", exe, nullptr, dir, SW_SHOW);
                    g_running = false;
                    PostQuitMessage(0);
                    return DefWindowProc(h, m, wp, lp);
                }
                g_silent = true;
                HWND cw = GetConsoleWindow();
                if (cw) { ShowWindow(cw, SW_HIDE); FreeConsole(); }
            }
            save_config();
            std::string msg = _("cfg_saved") + " (" + _(key) + ": " + (g_cfg[key] ? "ON" : "OFF") + ")";
            g_nid.szInfo[0] = 0;
            wcsncpy_s(g_nid.szInfo, u8tow(msg).c_str(), 255);
            g_nid.uFlags |= NIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        }
    } else if (m == WM_COMMAND) {
        if (LOWORD(wp) == ID_EXIT) { g_running = false; PostQuitMessage(0); }
    }
    return DefWindowProc(h, m, wp, lp);
}

void create_tray() {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"WxNTC";
    RegisterClassW(&wc);
    g_trayHwnd = CreateWindowW(L"WxNTC", L"", 0, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);

    HICON hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    if (!hIcon) hIcon = (HICON)LoadImageW(hi, L"weixin.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    if (!hIcon) hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_trayHwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = hIcon;
    wcscpy_s(g_nid.szTip, u8tow(_("tray_tip")).c_str());
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void run_tray_loop() {
    create_tray();
    MSG m;
    while (g_running && GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayHwnd) DestroyWindow(g_trayHwnd);
}

// ==================== 图片：定位微信的明文缩略图 ====================
// 前向声明：图片兜底要调 hook，实现在后面
std::string http_post_json(const std::string& host, int port, const std::string& path,
                           const std::string& body, int timeoutSec);

std::string md5_hex(const std::string& in) {
    std::string out;
    HCRYPTPROV prov = 0;
    HCRYPTHASH h = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return out;
    if (CryptCreateHash(prov, CALG_MD5, 0, 0, &h)) {
        if (CryptHashData(h, (const BYTE*)in.data(), (DWORD)in.size(), 0)) {
            BYTE dig[16] = {};
            DWORD len = sizeof(dig);
            if (CryptGetHashParam(h, HP_HASHVAL, dig, &len, 0)) {
                char buf[33] = {};
                for (int i = 0; i < 16; i++) sprintf_s(buf + i * 2, 3, "%02x", dig[i]);
                out.assign(buf, 32);
            }
        }
        CryptDestroyHash(h);
    }
    CryptReleaseContext(prov, 0);
    return out;
}

// 微信把「文件保存位置」的一级目录写在 %APPDATA%\Tencent\xwechat\config\<hash>.ini 的第一行
static void add_roots_from(const std::wstring& base) {
    std::error_code ec;
    fs::path p(base);
    if (!fs::is_directory(p, ec)) return;
    fs::path cand = (p.filename() == L"xwechat_files") ? p : (p / L"xwechat_files");
    if (!fs::is_directory(cand, ec)) return;
    for (auto& e : fs::directory_iterator(cand, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        auto name = e.path().filename().wstring();
        if (name.rfind(L"wxid_", 0) != 0) continue;
        std::wstring full = e.path().wstring();
        if (std::find(g_wxRoots.begin(), g_wxRoots.end(), full) == g_wxRoots.end())
            g_wxRoots.push_back(full);
    }
}

void detect_wx_roots() {
    // 1. config.json 里手填的（优先级最高，填哪个用哪个）
    std::string cfgRoot = g_cfg.value("wx_data_root", std::string());
    if (!cfgRoot.empty()) add_roots_from(u8tow(cfgRoot));

    // 2. 微信自己的配置目录
    wchar_t ad[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, ad))) {
        std::error_code ec;
        fs::path cfgDir = fs::path(ad) / L"Tencent" / L"xwechat" / L"config";
        if (fs::is_directory(cfgDir, ec)) {
            for (auto& e : fs::directory_iterator(cfgDir, ec)) {
                if (ec) break;
                if (!e.is_regular_file(ec) || e.path().extension() != L".ini") continue;
                std::ifstream in(e.path());
                std::string line;
                if (!std::getline(in, line)) continue;
                int wl = MultiByteToWideChar(CP_ACP, 0, line.c_str(), -1, nullptr, 0);
                if (wl <= 1) continue;
                std::wstring wline(wl, L'\0');
                MultiByteToWideChar(CP_ACP, 0, line.c_str(), -1, &wline[0], wl);
                wline.resize(wl - 1);
                size_t b = wline.find_first_not_of(L" \t\r\n");
                if (b == std::wstring::npos) continue;
                wline = wline.substr(b);
                while (!wline.empty() && (wline.back() == L' ' || wline.back() == L'\r' || wline.back() == L'\t'))
                    wline.pop_back();
                add_roots_from(wline);
            }
        }
    }

    // 3. 文档目录（4.x 默认位置）
    wchar_t doc[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, doc))) {
        add_roots_from(doc);
        add_roots_from(std::wstring(doc) + L"\\xwechat_files");
    }
}

// 消息时间戳 → "yyyy-MM"（缓存目录按本地月份分）
static std::wstring month_of(long long tsSec, int addMonths = 0) {
    time_t t = (time_t)tsSec;
    struct tm tmv = {};
    localtime_s(&tmv, &t);
    int y = tmv.tm_year + 1900, m = tmv.tm_mon + 1 + addMonths;
    while (m <= 0) { m += 12; y--; }
    while (m > 12) { m -= 12; y++; }
    wchar_t buf[16];
    swprintf_s(buf, L"%04d-%02d", y, m);
    return buf;
}

// 目录里找 <序号>_<Unix秒>_thumb.jpg，取时间戳最接近的
static std::wstring find_thumb_in(const std::wstring& dir, long long ts, long long tolSec) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return L"";
    std::wstring best;
    long long bestDiff = LLONG_MAX;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        auto name = e.path().filename().wstring();
        auto p = name.rfind(L"_thumb");
        if (p == std::wstring::npos) continue;
        auto u = name.rfind(L'_', p - 1);
        if (u == std::wstring::npos) continue;
        long long fts = 0;
        try { fts = std::stoll(name.substr(u + 1, p - u - 1)); } catch (...) { continue; }
        // 微信是先收到消息、过一会儿才落盘缩略图。
        // 比消息时刻还早 3 秒以上的，只能是上一条消息留下的，直接排除。
        if (fts + 3 < ts) continue;
        long long d = (fts > ts) ? (fts - ts) : (ts - fts);
        if (d <= tolSec && d < bestDiff) { bestDiff = d; best = e.path().wstring(); }
    }
    return best;
}

// 全部都猜不到会话目录时，扫整月所有会话（几十个目录，可接受）
static std::wstring scan_message_dir(const std::wstring& monthMsgDir, long long ts, long long tolSec) {
    std::error_code ec;
    if (!fs::is_directory(monthMsgDir, ec)) return L"";
    for (auto& e : fs::directory_iterator(monthMsgDir, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        auto f = find_thumb_in((e.path() / L"Thumb").wstring(), ts, tolSec);
        if (!f.empty()) return f;
    }
    return L"";
}

// 推送里可能的会话标识：群用 roomid，私聊用 sender；
// 不同 hook 版本的 wxid 字段含义不一致，config 里的对象也一并当候选。
static std::vector<std::string> peer_candidates(const json& d) {
    std::vector<std::string> v;
    auto add = [&](const std::string& s) {
        if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    };
    add(d.value("roomid", std::string()));
    add(d.value("sender", std::string()));
    add(d.value("wxid", std::string()));
    for (auto& [id, w] : g_watch) add(id);
    return v;
}

// 在 cache\<月>\Message\<md5>\Thumb 里找文图；找不到返回空
static std::wstring find_plain_thumb(const json& d, long long ts, long long tolSec) {
    auto peers = peer_candidates(d);
    std::wstring months[3] = { month_of(ts), month_of(ts, -1), month_of(ts, 1) };

    for (auto& root : g_wxRoots) {
        for (auto& m : months) {
            std::wstring msgDir = root + L"\\cache\\" + m + L"\\Message";
            for (auto& p : peers) {
                std::string md5 = md5_hex(p);
                if (md5.empty()) continue;
                auto f = find_thumb_in(msgDir + L"\\" + u8tow(md5) + L"\\Thumb", ts, tolSec);
                if (!f.empty()) return f;
            }
        }
    }
    // 候选都没命中 → 整月兜底
    for (auto& root : g_wxRoots) {
        for (auto& m : months) {
            auto f = scan_message_dir(root + L"\\cache\\" + m + L"\\Message", ts, tolSec);
            if (!f.empty()) return f;
        }
    }
    return L"";
}

// 兜底：从 attach 目录找缩略图 dat，让 hook 在微信进程内解一次
static std::wstring find_dat_near(const std::wstring& dir, long long ts, long long tolSec) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return L"";
    std::wstring best;
    long long bestDiff = LLONG_MAX;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        auto name = e.path().filename().wstring();
        if (name.size() < 4 || name.substr(name.size() - 4) != L".dat") continue;
        auto ft = fs::last_write_time(e.path(), ec);
        if (ec) continue;
        // 用文件修改时间与消息时间对齐；转换到 time_t 后按秒比较
        auto sys = std::chrono::system_clock::now() + (ft - fs::file_time_type::clock::now());
        long long fts = (long long)std::chrono::system_clock::to_time_t(sys);
        long long dd = (fts > ts) ? (fts - ts) : (ts - fts);
        if (dd <= tolSec && dd < bestDiff) { bestDiff = dd; best = e.path().wstring(); }
    }
    return best;
}

// ---------- wxgf / 裸 HEVC → JPEG ----------
// 微信 4.x 收到图片后，除了加密的 .dat，还会在
//   cache\<月>\Message\<md5>\ImageTemp\<序号>_<Unix秒>_mid_temp_convert
// 放一份它自己解好的预览：wxgf 头 + 裸 HEVC 码流（Main Still Picture）。
// 这是微信现成的解密产物，不需要任何密钥，剥掉头交给 ffmpeg 取第一帧即可。
static std::wstring g_ffmpegPath;

static bool file_exists_w(const std::wstring& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// 找不到就返回空，调用方自然退到文字通知
static std::wstring find_ffmpeg() {
    if (!g_ffmpegPath.empty()) return g_ffmpegPath;

    auto take = [](const std::wstring& p) -> bool {
        if (!file_exists_w(p)) return false;
        g_ffmpegPath = p;
        return true;
    };

    std::string cfg = g_cfg.value("ffmpeg_path", std::string());
    if (!cfg.empty() && take(u8tow(cfg))) return g_ffmpegPath;

    if (take(u8tow(g_exeDir) + L"ffmpeg.exe"))      return g_ffmpegPath;
    if (take(u8tow(g_exeDir) + L"bin\\ffmpeg.exe")) return g_ffmpegPath;

    wchar_t buf[MAX_PATH * 2];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH * 2, buf, nullptr) > 0)
        if (take(buf)) return g_ffmpegPath;

    return L"";
}

// 用 CreateProcess 跑 ffmpeg，不弹黑窗，超时就放弃（不能卡住通知线程）
static bool run_hidden(const std::wstring& cmdline, DWORD timeoutMs) {
    std::wstring cmd = cmdline;              // CreateProcessW 要求可写缓冲
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    DWORD r = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (r != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r == WAIT_OBJECT_0;
}

// wxgf（或任何以 Annex-B 起始码开头的 HEVC 缓冲）→ 一张 JPEG
static std::wstring hevc_blob_to_jpg(const std::string& blob, uint64_t msgid) {
    std::wstring ff = find_ffmpeg();
    if (ff.empty() || blob.size() < 256) return L"";

    static const char SC[4] = { 0, 0, 0, 1 };
    size_t pos = blob.find(std::string(SC, 4), 0);
    if (pos == std::string::npos || pos > 512 || blob.size() - pos < 128) return L"";

    std::error_code ec;
    fs::create_directories(g_imgDir, ec);
    wchar_t base[80];
    swprintf_s(base, L"img_%llu", (unsigned long long)(msgid ? msgid : (uint64_t)time(nullptr)));
    std::wstring tmp = g_imgDir + L"\\" + base + L".hevc";
    std::wstring dst = g_imgDir + L"\\" + base + L".jpg";

    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return L"";
        o.write(blob.data() + pos, (std::streamsize)(blob.size() - pos));
        if (!o) { }          // 写完再检查一次
    }
    DeleteFileW(dst.c_str());

    std::wstring cmd = L"\"" + ff + L"\" -hide_banner -loglevel error -y -f hevc -i \"" +
                       tmp + L"\" -frames:v 1 -q:v 3 \"" + dst + L"\"";
    bool ran = run_hidden(cmd, 8000);

    std::error_code sec;
    auto sz = fs::file_size(dst, sec);
    fs::remove(tmp, sec);                       // 中间文件立刻清掉
    if (!ran || sec || sz < 512) {
        DeleteFileW(dst.c_str());
        return L"";
    }
    return dst;
}

// 推送正文里的 <img hevc_mid_size="N">：微信为这条消息生成的那份预览的确切字节数。
// 实测 N 与 ImageTemp 下 _mid_temp_convert 的文件大小完全相同，文件名里的 Unix 秒
// 又正好等于消息 timestamp —— 两个特征叠起来就能唯一定位到这条消息自己的图。
static long long img_hevc_mid_size(const json& d) {
    auto c = d.value("content", std::string());
    auto p = c.find("hevc_mid_size=\"");
    if (p == std::string::npos) return 0;
    p += sizeof("hevc_mid_size=\"") - 1;
    auto e = c.find('"', p);
    if (e == std::string::npos) return 0;
    try { return std::stoll(c.substr(p, e - p)); } catch (...) { return 0; }
}

// 在 ImageTemp 里定位这条消息自己那份预览。三级判定，越往下越宽松，
// 但任何一级都不接受"比消息更早生成"的文件 —— 那只能是上一条消息的图：
//   ① 文件名秒 ≈ 消息秒 且 大小 == hevc_mid_size   → 独一份，最可信
//   ② 文件名秒 ≈ 消息秒                            → 同一秒只有一条图片消息时成立
//   ③ 大小 == hevc_mid_size 且 相差 120 秒内        → 时间戳不准时靠大小认
static std::wstring find_mid_convert(const json& d, long long ts, long long wantSize) {
    auto peers = peer_candidates(d);
    std::wstring months[3] = { month_of(ts), month_of(ts, -1), month_of(ts, 1) };

    std::wstring bestA, bestB, bestC;
    long long diffA = LLONG_MAX, diffB = LLONG_MAX, diffC = LLONG_MAX;

    for (auto& root : g_wxRoots) {
        for (auto& m : months) {
            for (auto& p : peers) {
                std::string md5 = md5_hex(p);
                if (md5.empty()) continue;
                std::wstring dir = root + L"\\cache\\" + m + L"\\Message\\" +
                                   u8tow(md5) + L"\\ImageTemp";
                std::error_code ec;
                if (!fs::is_directory(dir, ec)) continue;

                for (auto& e : fs::directory_iterator(dir, ec)) {
                    if (ec) break;
                    if (!e.is_regular_file(ec)) continue;
                    auto name = e.path().filename().wstring();
                    auto up = name.rfind(L"_mid_temp_convert");
                    if (up == std::wstring::npos) continue;
                    auto us = name.rfind(L'_', up - 1);
                    if (us == std::wstring::npos) continue;
                    long long fts = 0;
                    try { fts = std::stoll(name.substr(us + 1, up - us - 1)); } catch (...) { continue; }

                    if (fts + 3 < ts) continue;            // 早于消息 = 上一条的图
                    long long dd = fts - ts;
                    if (dd < 0) dd = -dd;
                    if (dd > 120) continue;

                    std::error_code sec;
                    auto sz = (long long)fs::file_size(e.path(), sec);
                    if (sec) continue;

                    std::wstring path = e.path().wstring();
                    if (conv_used(path)) continue;          // 已经附给别的消息了

                    bool sizeOk = (wantSize > 0 && sz == wantSize);
                    if (sizeOk && dd <= 2 && dd < diffA) { diffA = dd; bestA = path; }
                    if (dd <= 2 && dd < diffB)           { diffB = dd; bestB = path; }
                    if (sizeOk && dd < diffC)            { diffC = dd; bestC = path; }
                }
            }
        }
    }
    if (!bestA.empty()) return bestA;
    if (!bestB.empty()) return bestB;
    return bestC;
}

// 最后兜底：推送里没有 hevc_mid_size、文件名秒数也和 timestamp 对不上时，
// 只认"这条消息之后 30 秒内新冒出来、且没被别的消息用过"的那一份。
// 绝不再用整个会话最近的一份凑数 —— 那正是偶尔套上一条图的根源。
static std::wstring find_mid_convert_recent(const json& d, long long sinceSec) {
    auto peers = peer_candidates(d);
    long long now = (long long)time(nullptr);
    std::wstring best;
    long long bestT = 0;

    for (auto& root : g_wxRoots) {
        for (auto& p : peers) {
            std::string md5 = md5_hex(p);
            if (md5.empty()) continue;
            for (int off : { 0, -1, 1 }) {
                std::wstring dir = root + L"\\cache\\" + month_of(now, off) +
                                   L"\\Message\\" + u8tow(md5) + L"\\ImageTemp";
                std::error_code ec;
                if (!fs::is_directory(dir, ec)) continue;
                for (auto& e : fs::directory_iterator(dir, ec)) {
                    if (ec) break;
                    if (!e.is_regular_file(ec)) continue;
                    auto name = e.path().filename().wstring();
                    if (name.find(L"_mid_temp_convert") == std::wstring::npos) continue;
                    auto ft = fs::last_write_time(e.path(), ec);
                    if (ec) continue;
                    auto sys = std::chrono::system_clock::now() + (ft - fs::file_time_type::clock::now());
                    long long fts = (long long)std::chrono::system_clock::to_time_t(sys);
                    long long age = now - fts;
                    if (age < 0) age = 0;
                    if (age > 30) continue;              // 只认刚生成的
                    if (fts + 3 < sinceSec) continue;    // 不能早于这条消息
                    if (conv_used(e.path().wstring())) continue;
                    if (fts > bestT) { bestT = fts; best = e.path().wstring(); }
                }
            }
        }
    }
    return best;
}

static std::string read_file_bin(const std::wstring& p) {    std::ifstream f(p, std::ios::binary);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::wstring decode_dat_via_hook(const json& d, long long ts, uint64_t msgid) {
    auto peers = peer_candidates(d);
    std::wstring src;
    for (auto& root : g_wxRoots) {
        for (auto& p : peers) {
            std::string md5 = md5_hex(p);
            if (md5.empty()) continue;
            std::wstring dir = root + L"\\msg\\attach\\" + u8tow(md5) + L"\\" + month_of(ts) + L"\\Img";
            src = find_dat_near(dir, ts, 90);
            if (!src.empty()) break;
        }
        if (!src.empty()) break;
    }
    if (src.empty()) return L"";

    std::error_code ec;
    fs::create_directories(g_imgDir, ec);
    wchar_t name[64];
    swprintf_s(name, L"dec_%llu.jpg", (unsigned long long)msgid);
    std::wstring dst = g_imgDir + L"\\" + name;
    DeleteFileW(dst.c_str());

    json body = { {"src_path", wtou8(src)}, {"dst_path", wtou8(dst)} };
    http_post_json(g_hookHost, g_hookPort, "/Decode_Pic", body.dump(), 2);

    // 这个接口不管成败都回 success，只能看文件到底出来了没有
    std::error_code szec;
    auto sz = fs::file_size(dst, szec);
    if (szec || sz == 0) return L"";
    return dst;
}

static std::atomic<long long> g_lastImgClean{0};

// 拷进自己管的目录：Toast 是异步取图的，微信缓存随时可能被清
static std::wstring store_for_toast(const std::wstring& src, uint64_t msgid) {
    if (src.empty()) return L"";
    std::error_code ec;
    fs::create_directories(g_imgDir, ec);

    long long now = (long long)time(nullptr);
    long long last = g_lastImgClean.load();
    if (now - last > 300 && g_lastImgClean.compare_exchange_strong(last, now)) {
        for (auto& e : fs::directory_iterator(g_imgDir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            auto ft = fs::last_write_time(e.path(), ec);
            if (ec) continue;
            auto sys = std::chrono::system_clock::now() + (ft - fs::file_time_type::clock::now());
            long long age = now - (long long)std::chrono::system_clock::to_time_t(sys);
            if (age > 7200) fs::remove(e.path(), ec);
        }
    }

    wchar_t name[64];
    swprintf_s(name, L"%llu.jpg", (unsigned long long)(msgid ? msgid : (uint64_t)now));
    std::wstring dst = g_imgDir + L"\\" + name;
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return L"";

    // CopyFile 会把源文件的修改时间一起搬过来。清理按修改时间算年龄，
    // 不重置的话刚拷进来的图会立刻被当成两天前的旧文件删掉。
    HANDLE h = CreateFileW(dst.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        SetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
    }
    return dst;
}

// 主入口：拿到一条图片消息，尽力找一张可用的明文图；找不到返回空（照常发纯文字通知）
std::wstring resolve_image(const json& d, uint64_t msgid) {
    long long ts = (long long)d.value("timestamp", 0LL);
    if (ts <= 0) ts = (long long)time(nullptr);
    if (ts > 100000000000LL) ts /= 1000;   // 防御：万一给了毫秒

    // 明文缩略图（微信在 UI 里渲染过才有）
    std::wstring f = find_plain_thumb(d, ts, 3);
    if (f.empty()) f = find_plain_thumb(d, ts, 12);
    if (!f.empty()) return store_for_toast(f, msgid);

    // 微信自己解好的预览（wxgf/HEVC）。用 hevc_mid_size + 文件名秒双重定位，
    // 两个特征都取自这条消息本身，不再靠"谁和时间戳更接近"去猜。
    long long wantSize = img_hevc_mid_size(d);
    std::wstring conv = find_mid_convert(d, ts, wantSize);
    if (!conv.empty()) {
        std::wstring jpg = hevc_blob_to_jpg(read_file_bin(conv), msgid);
        if (!jpg.empty()) { conv_mark_used(conv); return jpg; }
    }
    // 时间戳和大小都对不上时，只接受"这条消息之后新生成的那份"
    conv = find_mid_convert_recent(d, ts);
    if (!conv.empty()) {
        std::wstring jpg = hevc_blob_to_jpg(read_file_bin(conv), msgid);
        if (!jpg.empty()) { conv_mark_used(conv); return jpg; }
    }

    return decode_dat_via_hook(d, ts, msgid);
}

// ==================== Toast ====================
// 异步化：hook 的 HTTP 推送在 httplib 工作线程里处理，
// 如果在这里同步等 PowerShell（最多 6 秒），会把工作线程占死，
// 后续推送全部排队。改成入队 + 专门的工作线程消费。
struct ToastItem {
    std::string title, body;
    std::wstring image;
    json     data;               // 图片消息：把原始推送留下来，到工作线程再解析
    uint64_t msgid = 0;
    bool     wantImage = false;
};
std::mutex              g_toastMtx;
std::condition_variable g_toastCv;
std::deque<ToastItem>   g_toastQueue;

static void toast_worker_loop();

void enqueue_toast(const std::string& t, const std::string& b, const std::wstring& img = L"") {
    {
        std::lock_guard<std::mutex> lk(g_toastMtx);
        // 积压上限：真刷屏时丢掉最老的，避免无限增长
        if (g_toastQueue.size() >= 32) g_toastQueue.pop_front();
        g_toastQueue.push_back({t, b, img});
    }
    g_toastCv.notify_one();
}

// 图片消息走这个：解析（可能要等微信写文件、要跑 ffmpeg）全放到工作线程
void enqueue_toast_image(const std::string& t, const std::string& b, const json& d, uint64_t msgid) {
    {
        std::lock_guard<std::mutex> lk(g_toastMtx);
        if (g_toastQueue.size() >= 32) g_toastQueue.pop_front();
        ToastItem it;
        it.title = t;
        it.body  = b;
        it.data  = d;
        it.msgid = msgid;
        it.wantImage = true;
        g_toastQueue.push_back(std::move(it));
    }
    g_toastCv.notify_one();
}

static std::wstring xml_escape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'&':  o += L"&amp;";  break;
            case L'<':  o += L"&lt;";   break;
            case L'>':  o += L"&gt;";   break;
            case L'"':  o += L"&quot;"; break;
            case L'\'': o += L"&apos;"; break;
            default:    o += c;
        }
    }
    return o;
}

// 真正拉起 PowerShell 的那一步（只在 toast 工作线程里跑）
static void send_toast_now(const std::string& t, const std::string& b, const std::wstring& image) {
    std::wstring wt = u8tow(t), wb = u8tow(b);

    // 有图就走 ToastGeneric（能放图）；没图沿用原来的 ToastText02
    if (image.empty()) {
        for (auto& c : wt) if (c == L'\'') c = L'\u2019';
        for (auto& c : wb) if (c == L'\'') c = L'\u2019';

        std::wstring ps =
            L"[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime]|Out-Null;"
            L"$x=[Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02);"
            L"$n=$x.GetElementsByTagName('text');"
            L"$n.Item(0).AppendChild($x.CreateTextNode('" + wt + L"'))|Out-Null;"
            L"$n.Item(1).AppendChild($x.CreateTextNode('" + wb + L"'))|Out-Null;"
            L"[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('" + std::wstring(AUMID) +
            L"').Show([Windows.UI.Notifications.ToastNotification]::new($x))";

        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"powershell -NoP -Command \"" + ps + L"\"";
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 6000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        return;
    }

    // ---- 带图 ----
    std::wstring uri = L"file:///" + image;
    for (auto& c : uri) if (c == L'\\') c = L'/';

    std::wstring xml =
        L"<toast><visual><binding template=\"ToastGeneric\">"
        L"<text>" + xml_escape(wt) + L"</text>"
        L"<text>" + xml_escape(wb) + L"</text>"
        L"<image src=\"" + xml_escape(uri) + L"\" placement=\"hero\" hint-crop=\"none\" alt=\"图片\"/>"
        L"</binding></visual></toast>";

    // XML 走临时文件：正文里的引号、换行、特殊字符都不用再操心转义层级
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    wchar_t xmlFile[MAX_PATH] = {};
    swprintf_s(xmlFile, L"%swxn_%lu_%llu.xml", tmpDir, GetCurrentProcessId(),
               (unsigned long long)GetTickCount64());

    {
        std::string u8 = wtou8(xml);
        std::ofstream f(fs::path(xmlFile), std::ios::binary);
        if (f.is_open()) { f.write(u8.data(), (std::streamsize)u8.size()); f.close(); }
        else return;   // 连临时文件都写不出来，就别折腾了
    }

    std::wstring pathArg(xmlFile);
    for (size_t i = pathArg.find(L'\''); i != std::wstring::npos; i = pathArg.find(L'\'', i + 1))
        pathArg.insert(i, L"'");   // PowerShell 单引号里自身要写两遍

    std::wstring ps =
        L"[Windows.Data.Xml.Dom.XmlDocument,Windows.Data.Xml.Dom,ContentType=WindowsRuntime]|Out-Null;"
        L"[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime]|Out-Null;"
        L"[Windows.UI.Notifications.ToastNotification,Windows.UI.Notifications,ContentType=WindowsRuntime]|Out-Null;"
        L"$x=New-Object Windows.Data.Xml.Dom.XmlDocument;"
        L"$x.LoadXml([IO.File]::ReadAllText('" + pathArg + L"',[Text.Encoding]::UTF8));"
        L"[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('" + std::wstring(AUMID) +
        L"').Show([Windows.UI.Notifications.ToastNotification]::new($x))";

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"powershell -NoP -Command \"" + ps + L"\"";
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 6000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    DeleteFileW(xmlFile);
}

static void toast_worker_loop() {
    for (;;) {
        ToastItem item;
        {
            std::unique_lock<std::mutex> lk(g_toastMtx);
            g_toastCv.wait(lk, [] { return !g_toastQueue.empty() || !g_running; });
            if (!g_running && g_toastQueue.empty()) return;
            item = std::move(g_toastQueue.front());
            g_toastQueue.pop_front();
        }
        if (item.wantImage) {
            // 微信是在收到消息前后才把预览写盘的，稍等一下再找；
            // 这段（含 ffmpeg）放在工作线程，HTTP 推送回调不会被拖住
            for (int attempt = 0; attempt < 3 && item.image.empty(); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(attempt ? 1200 : 500));
                item.image = resolve_image(item.data, item.msgid);
            }
            item.data = json();
            if (!item.image.empty()) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                printf("[%02d:%02d:%02d] [%s] %s  [图片已附]\n", st.wHour, st.wMinute, st.wSecond,
                       item.title.c_str(), item.body.c_str());
                fflush(stdout);
            }
        }
        send_toast_now(item.title, item.body, item.image);
    }
}

// ==================== WinHTTP POST ====================
std::string http_post_json(const std::string& host, int port, const std::string& path, const std::string& body, int timeoutSec = 3) {
    std::string result;
    HINTERNET hS = WinHttpOpen(L"WxNotifier/2.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return result;

    WinHttpSetTimeouts(hS, timeoutSec * 1000, timeoutSec * 1000, timeoutSec * 1000, timeoutSec * 1000);

    HINTERNET hC = WinHttpConnect(hS, u8tow(host).c_str(), (INTERNET_PORT)port, 0);
    if (!hC) { WinHttpCloseHandle(hS); return result; }

    HINTERNET hR = WinHttpOpenRequest(hC, L"POST", u8tow(path).c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (hR) {
        const wchar_t* hdrs = L"Content-Type: application/json\r\n";
        BOOL ok = WinHttpSendRequest(hR, hdrs, (DWORD)-1L,
                                     (LPVOID)body.data(), (DWORD)body.size(),
                                     (DWORD)body.size(), 0);
        if (ok && WinHttpReceiveResponse(hR, nullptr)) {
            DWORD avail = 0;
            do {
                if (!WinHttpQueryDataAvailable(hR, &avail) || !avail) break;
                std::vector<char> buf(avail + 1, 0);
                DWORD rd = 0;
                if (!WinHttpReadData(hR, buf.data(), avail, &rd) || !rd) break;
                result.append(buf.data(), rd);
            } while (avail > 0);
        }
        WinHttpCloseHandle(hR);
    }
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return result;
}

// ==================== Hook 交互 ====================
bool hook_ping() {
    std::string r = http_post_json(g_hookHost, g_hookPort, "/GetSelfProfile", "{}", 2);
    return !r.empty() && r.find('{') != std::string::npos;
}

bool hook_set_callback() {
    json body = { {"url", wtou8(g_callbackUrl)} };
    std::string r = http_post_json(g_hookHost, g_hookPort, "/set_callback", body.dump(), 3);
    return r.find("\"ret\":0") != std::string::npos;
}

// ==================== 去重 ====================
bool mark_seen(uint64_t id) {
    if (id == 0) return true;   // 没有 msgid 就不去重
    std::lock_guard<std::mutex> lk(g_seenMtx);
    if (g_seenSet.count(id)) return false;
    g_seenSet.insert(id);
    g_seenOrder.push_back(id);
    while (g_seenOrder.size() > 2000) {
        g_seenSet.erase(g_seenOrder.front());
        g_seenOrder.pop_front();
    }
    return true;
}

// ==================== 消息格式化 ====================
// 从 XML 里抠一个标签内容
std::string xml_tag(const std::string& xml, const std::string& tag) {
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    size_t a = xml.find(open);
    if (a == std::string::npos) return "";
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::string::npos) return "";
    std::string v = xml.substr(a, b - a);
    // 去 CDATA
    size_t cs = v.find("<![CDATA[");
    if (cs != std::string::npos) {
        size_t ce = v.find("]]>", cs);
        if (ce != std::string::npos) v = v.substr(cs + 9, ce - cs - 9);
    }
    // 去首尾空白
    size_t f = v.find_first_not_of(" \t\r\n");
    if (f == std::string::npos) return "";
    size_t e = v.find_last_not_of(" \t\r\n");
    return v.substr(f, e - f + 1);
}

std::string format_content(int type, const std::string& raw) {
    // 类型 1 = 纯文本：不加前缀，原样输出（压缩连续空行）
    if (type == 1) {
        std::string s = raw;
        for (auto& c : s) if (c == '\r') c = ' ';
        std::string out;
        bool lastNl = false;
        for (char c : s) {
            if (c == '\n') { if (!lastNl) out += '\n'; lastNl = true; }
            else { out += c; lastNl = false; }
        }
        return out;
    }

    // 类型 49 = XML 卡片，按 appmsg/type 细分
    if (type == 49) {
        std::string appname = xml_tag(raw, "appname");
        std::string title   = xml_tag(raw, "title");
        std::string des     = xml_tag(raw, "des");
        std::string subType = xml_tag(raw, "type");

        // appmsg 的 type 编号（微信内部定义）
        //   5=链接  6=文件  3=音乐  4=视频  8=表情  17=位置共享
        //   19=聊天记录  33/36=小程序  57=引用回复  51/52/48=视频号
        std::string tag = "卡片";
        if      (subType == "5")  tag = "link链接";
        else if (subType == "6")  tag = "file文件";
        else if (subType == "3")  tag = "music音乐";
        else if (subType == "4")  tag = "video视频";
        else if (subType == "8")  tag = "emoji表情";
        else if (subType == "17") tag = "location位置";
        else if (subType == "19") tag = "chatrecord聊天记录";
        else if (subType == "33" || subType == "36") tag = "miniapp小程序";
        else if (subType == "57") tag = "quote引用回复";
        else if (subType == "51" || subType == "52" || subType == "48") tag = "channels视频号";
        else if (!appname.empty()) tag = appname;

        // 正文：标题 + 描述
        std::string body;
        if (!title.empty()) {
            body = title;
            if (!des.empty() && des != title) body += "\n" + des;
        } else if (subType == "57") {
            // 引用回复：正文在 content 里
            body = xml_tag(raw, "content");
        }

        if (body.empty()) return "[" + tag + "]";
        return "[" + tag + "] " + body;
    }

    // 其他类型：统一 [tag中文] 前缀
    switch (type) {
        case 3:     return "[image图片]";
        case 34:    return "[audio语音]";
        case 37:    return "[friend申请加好友]";
        case 42:    return "[contact名片]";
        case 43:    return "[video视频]";
        case 47:    return "[emoji表情]";
        case 48:    return "[location位置]";
        case 49:    return "[卡片]";              // 已在上方处理，兜底
        case 50:    return "[call通话]";
        case 62:    return "[video视频]";
        case 10000: return "[system系统消息]";
        case 10002: return "[system系统通知]";
        default:    return "[type=" + std::to_string(type) + "]";
    }
}

// ==================== 推送处理 ====================
void handle_push(const json& d) {
    int type = d.value("type", 0);

    // 51 = 同步信令，不是真消息，直接丢
    if (type == 51) return;
    if (!g_notifyTypes.count(type)) return;

    std::string selfwx   = d.value("wxid", std::string());      // 本机登录账号
    std::string sender   = d.value("sender", std::string());    // 实际发送者
    std::string roomid   = d.value("roomid", std::string());
    // 私聊的「对方」是 sender；wxid 是登录账号自己，拿它去匹配永远对不上。
    // 少数 hook 版本不带 sender，这时才退回 wxid。
    std::string peer     = (!sender.empty() && sender != selfwx) ? sender : selfwx;
    std::string content  = d.value("content", std::string());
    uint64_t    msgid    = d.value("msgid", 0ULL);

    if (content.empty() && type == 1) return;

    // 调试：把原始推送原样记一份（排查图片路径/字段问题时用，默认关闭）
    if (g_dbgPayload.load()) {
        static std::mutex dbgMtx;
        std::lock_guard<std::mutex> lk(dbgMtx);
        std::string line = d.dump();
        for (auto& c : line) if (c == '\n' || c == '\r') c = ' ';
        std::ofstream f(fs::path(u8tow(g_exeDir) + L"payload.log"), std::ios::binary | std::ios::app);
        if (f) f << line << "\n";
    }

    // ===== 按监听范围决定这条消息要不要通知 =====
    WatchMode mode = (WatchMode)g_watchMode.load();

    // 公众号/服务号一律不通知（无法读联系人库，只能按 wxid 前缀排）
    // 只在 ALL 模式下也排掉，否则一天几百条推送会刷屏
    const std::string& who_id = roomid.empty() ? peer : sender;
    if (is_mass_account(who_id)) return;

    std::string title;
    WatchMode effective = mode;

    if (mode == WatchMode::CONFIG) {
        // 只收 config.json 里 users 列出的对象
        std::string key = roomid.empty() ? peer : roomid;
        auto it = g_watch.find(key);
        if (it == g_watch.end()) return;

        const WatchTarget& w = it->second;
        title = w.name;
        if (w.isGroup && !roomid.empty()) {
            auto m = g_memberNames.find(sender);
            std::string who = (m != g_memberNames.end()) ? m->second : sender;
            title = w.name + " · " + who;
        }
    }
    else if (mode == WatchMode::PRIVATE) {
        // 仅私聊：群消息直接丢
        if (!roomid.empty()) return;
        // 标题用 wxid（拿不到昵称）
        title = peer;
        auto it = g_watch.find(peer);
        if (it != g_watch.end()) title = it->second.name;   // config 里有就用配置的名字
    }
    else {
        // 所有人：群聊 + 私聊都收
        if (roomid.empty()) {
            title = peer;
            auto it = g_watch.find(peer);
            if (it != g_watch.end()) title = it->second.name;
        } else {
            title = roomid;
            auto it = g_watch.find(roomid);
            if (it != g_watch.end()) title = it->second.name;
            // 群聊显示发言人
            auto m = g_memberNames.find(sender);
            std::string who = (m != g_memberNames.end()) ? m->second : sender;
            title += " · " + who;
        }
    }

    if (!mark_seen(msgid)) return;

    // 正文
    std::string body = clamp_utf8(format_content(type, content), 300);

    if (body.empty()) return;

    // 图片消息：找图这一步（可能等微信落盘、可能跑 ffmpeg）交给 toast 工作线程，
    // 这里只把原始推送转过去，立刻返回。
    // 全程只读微信的缓存文件 + 调 hook 的 HTTP 接口，不注入、不读内存、不重启微信。
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("[%02d:%02d:%02d] [%s] %s\n", st.wHour, st.wMinute, st.wSecond,
           title.c_str(), body.c_str());
    fflush(stdout);

    if (type == 3 && g_imgPreview.load()) {
        enqueue_toast_image(title, body, d, msgid);
    } else {
        enqueue_toast(title, body);
    }
}

// ==================== 启动推送服务 ====================
void start_push_server() {
    g_svr = new httplib::Server();

    g_svr->Post("/recv", [](const httplib::Request& req, httplib::Response& res) {
        json d;
        try {
            d = json::parse(req.body);
        } catch (...) {
            res.set_content("{\"code\":0,\"msg\":\"bad json\"}", "application/json");
            return;
        }
        // 单条推送：只要求有 type，不强制 content
        // （图片/语音类推送可能不带 content 字段，强制要求会整条丢失）
        if (d.contains("type")) {
            handle_push(d);
        }
        // 兼容旧版 msglist 包裹格式
        else if (d.contains("msglist") && d["msglist"].is_array()) {
            for (auto& m : d["msglist"]) {
                json one = {
                    {"type",     m.value("msgtype", 0)},
                    {"content",  m.value("msg", std::string())},
                    {"sender",   m.value("fromid", std::string())},
                    {"roomid",   m.value("toid", std::string())},
                    {"msgid",    m.value("msgsvrid", 0ULL)},
                    {"wxid",     d.value("selfwxid", std::string())},
                    {"event_type", 1001},
                };
                handle_push(one);
            }
        }
        res.set_content("{\"code\":1,\"msg\":\"success\"}", "application/json");
    });

    // 健康检查
    g_svr->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json j = {
            {"ok", true},
            {"hook_online", g_hookOnline.load()},
            {"listen_port", g_listenPort},
            {"watchers", g_watch.size()},
        };
        res.set_content(j.dump(), "application/json");
    });

    g_svr->set_keep_alive_max_count(32);
    g_svr->set_read_timeout(5, 0);

    // 端口占用检测：bind_to_port 明确返回成败，listen 则是静默阻塞
    if (!g_svr->bind_to_port("127.0.0.1", g_listenPort)) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "端口 %d 已被占用，无法接收 hook 推送。\n"
                 "请关闭占用该端口的程序，或修改 config.json 的 listen_port。",
                 g_listenPort);
        printf("[E] %s\n", buf);
        fflush(stdout);
        MessageBoxW(NULL, u8tow(buf).c_str(), L"WeChat Notifier", MB_ICONERROR);
        g_running = false;
        return;
    }

    OutputDebugStringA("[PUSH] server starting\n");
    g_svr->listen_after_bind();
    OutputDebugStringA("[PUSH] server stopped\n");
}

// ==================== AUMID / Autostart ====================
void ensure_shortcut() {
    wchar_t ad[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, ad))) return;
    std::wstring l = std::wstring(ad) + L"\\Microsoft\\Windows\\Start Menu\\Programs\\WeChat-Notifier.lnk";
    if (GetFileAttributesW(l.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    wchar_t ex[MAX_PATH];
    GetModuleFileNameW(NULL, ex, MAX_PATH);
    CoInitialize(NULL);
    ComPtr<IShellLinkW> sl;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, (void**)&sl))) {
        sl->SetPath(ex);
        sl->SetIconLocation(ex, 0);
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(sl.As(&ps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_LPWSTR;
            pv.pwszVal = (LPWSTR)AUMID;
            PROPERTYKEY k;
            CLSIDFromString(L"{9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}", &k.fmtid);
            k.pid = 5;
            ps->SetValue(k, pv);
            ps->Commit();
            PropVariantClear(&pv);
        }
        ComPtr<IPersistFile> pf;
        if (SUCCEEDED(sl.As(&pf))) pf->Save(l.c_str(), TRUE);
    }
    CoUninitialize();
}

void setup_autostart(bool en) {
    wchar_t ad[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, ad))) return;
    std::wstring l = std::wstring(ad) + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\WeChat-Notifier.lnk";

    if (en) {
        if (GetFileAttributesW(l.c_str()) != INVALID_FILE_ATTRIBUTES) return;
        wchar_t ex[MAX_PATH], d[MAX_PATH];
        GetModuleFileNameW(NULL, ex, MAX_PATH);
        GetCurrentDirectoryW(MAX_PATH, d);
        std::wstring ps = L"$w=New-Object -ComObject WScript.Shell;$s=$w.CreateShortcut('" + l +
                          L"');$s.TargetPath='" + std::wstring(ex) + L"';$s.WorkingDirectory='" +
                          std::wstring(d) + L"';$s.WindowStyle=7;$s.Save()";
        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::wstring c = L"powershell -NoP -Command \"" + ps + L"\"";
        if (CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    } else {
        if (GetFileAttributesW(l.c_str()) != INVALID_FILE_ATTRIBUTES) DeleteFileW(l.c_str());
    }
}

// ==================== Hook 守护线程 ====================
void hook_watchdog() {
    while (g_running) {
        bool online = hook_ping();

        if (online) {
            // 每次 ping 成功都重申 callback。
            // 理由：/set_callback 的状态只存在 hook 的内存里，WeChat 重启、
            // 别的工具调用了 set_callback、hook 内部状态被清，都会导致回调丢失。
            // 每 15 秒重注册一次的成本可忽略，但能免疫所有这类丢失。
            bool ok = hook_set_callback();

            if (ok) {
                if (!g_hookOnline) {
                    printf("[+] %s -> %s\n", _("ready").c_str(), wtou8(g_callbackUrl).c_str());
                    fflush(stdout);
                }
                g_hookOnline = true;
            } else {
                if (g_hookOnline) {
                    printf("[!] set_callback failed, retrying\n");
                    fflush(stdout);
                }
                g_hookOnline = false;
            }
        } else {
            if (g_hookOnline) {
                printf("[!] %s\n", _("disconnected").c_str());
                fflush(stdout);
            }
            g_hookOnline = false;
        }

        // 在线：15 秒一轮（含重注册）；离线：5 秒一轮，快速恢复
        int wait = g_hookOnline ? 15 : 5;
        for (int i = 0; i < wait && g_running; i++)
            std::this_thread::sleep_for(1s);
    }
}

// ==================== Entry ====================
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) g_quietStart = true;
    }

    wchar_t b[MAX_PATH];
    GetModuleFileNameW(NULL, b, MAX_PATH);
    g_exeDir = wtou8(std::wstring(b).substr(0, std::wstring(b).find_last_of(L"\\/") + 1));

    // 通知里的图片必须活到 Toast 真正读它为止，统一放临时目录 %temp%\WxNotifier
    // （随系统临时目录走，用户能自己清；程序也会删掉 2 小时前的旧图）
    {
        wchar_t lt[MAX_PATH] = {};
        DWORD n = GetTempPathW(MAX_PATH, lt);
        if (n > 0 && n < MAX_PATH)
            g_imgDir = std::wstring(lt) + L"WxNotifier";   // GetTempPathW 自带尾部反斜杠
        else
            g_imgDir = u8tow(g_exeDir) + L"WxNotifier";
    }

    try {
        g_cfg = load_config();
    } catch (const std::exception& e) {
        printf("[E] config: %s\n", e.what());
        Sleep(10000);
        return 1;
    }

    g_lang = g_cfg.value("language", "zh_cn");
    if (!D.count(g_lang)) g_lang = "en_us";

    // --- 读配置 ---
    std::string hookApi = g_cfg.value("hook_api", "http://127.0.0.1:30001");
    if (!parse_url(hookApi, g_hookHost, g_hookPort)) {
        printf("[E] invalid hook_api: %s\n", hookApi.c_str());
        Sleep(10000);
        return 1;
    }

    g_listenPort = g_cfg.value("listen_port", 8899);
    if (g_listenPort <= 0 || g_listenPort > 65535) g_listenPort = 8899;
    g_callbackUrl = u8tow("http://127.0.0.1:" + std::to_string(g_listenPort) + "/recv");

    g_autostart = g_cfg.value("autostart", false);
    g_silent    = g_cfg.value("silent_mode", false);

    // 监听范围：0=所有人 1=仅私聊 2=Config内用户
    {
        int wm = g_cfg.value("watch_mode", (int)WatchMode::CONFIG);
        if (wm < 0 || wm > 2) wm = (int)WatchMode::CONFIG;
        g_watchMode = wm;
    }

    g_imgPreview = g_cfg.value("image_preview", true);
    g_dbgPayload = g_cfg.value("debug_payload", false);

    // 通知类型
    if (g_cfg.contains("notify_types") && g_cfg["notify_types"].is_array()) {
        g_notifyTypes.clear();
        for (auto& t : g_cfg["notify_types"]) {
            if (t.is_number_integer()) g_notifyTypes.insert(t.get<int>());
        }
        if (g_notifyTypes.empty()) g_notifyTypes = {1, 3, 34, 37, 42, 43, 47, 48, 49, 50, 62, 10000, 10002};
    }

    // 群成员昵称映射
    if (g_cfg.contains("members") && g_cfg["members"].is_object()) {
        for (auto& [k, v] : g_cfg["members"].items()) {
            if (v.is_string()) g_memberNames[k] = v.get<std::string>();
        }
    }

    // --- 监听目标 ---
    if (g_cfg.contains("users") && g_cfg["users"].is_array()) {
        for (auto& u : g_cfg["users"]) {
            WatchTarget w;
            // 兼容旧配置的 session_id 字段
            w.id = u.value("wxid", u.value("session_id", std::string()));
            w.name = u.value("display_name", w.id);
            w.isGroup = u.value("is_group", w.id.find("@chatroom") != std::string::npos);
            if (w.id.empty() || w.id.find("xxx") != std::string::npos) continue;
            g_watch[w.id] = w;
        }
    }

    // 只有 CONFIG 模式才必须有监听对象；
    // ALL / PRIVATE 模式不需要 users，靠推送里的字段自己判断
    if (g_watch.empty() && (WatchMode)g_watchMode.load() == WatchMode::CONFIG) {
        printf("[W] %s\n", _("no_users").c_str());
        MessageBoxW(NULL, u8tow(_("no_users")).c_str(), L"WeChat Notifier", MB_OK);
        return 1;
    }

    // --- 启动横幅 ---
    printf("===========================================\n  %s\n===========================================\n", _("banner").c_str());
    {
        int wm = g_watchMode.load();
        printf("  %s: %s\n", _("watch_scope").c_str(),
               wm == 0 ? _("scope_all").c_str()
             : wm == 1 ? _("scope_private").c_str()
                       : _("scope_config").c_str());
    }
    for (auto& [id, w] : g_watch) {
        printf("  %s: %s (%s)%s\n", _("target").c_str(), w.name.c_str(), id.c_str(),
               w.isGroup ? " [group]" : "");
    }
    printf("  %s: %s\n", _("hook_api").c_str(), hookApi.c_str());
    printf("  %s: %d\n", _("listen").c_str(), g_listenPort);
    printf("  %s: %s\n",
           _("autostart").c_str(), g_autostart.load() ? "ON" : "OFF");
    printf("  %s: %s\n",
           _("silent_mode").c_str(), g_silent.load() ? "ON" : "OFF");

    // 图片通知需要知道微信把文件存哪了：自动探测一次，失败就只发文字
    if (g_imgPreview) detect_wx_roots();
    printf("  %s: %s (%d)\n", _("image_preview").c_str(),
           g_imgPreview.load() ? _("img_on").c_str() : _("img_off").c_str(),
           (int)g_wxRoots.size());
    if (g_imgPreview && g_wxRoots.empty())
        printf("  [!] 未探测到微信数据目录，图片通知会退化成文字；可在 config.json 填 wx_data_root\n");
    if (g_imgPreview) {
        std::wstring ff = find_ffmpeg();
        if (ff.empty())
            printf("  [!] 没找到 ffmpeg.exe，图片预览无法转成 JPEG；可放进程序目录或填 ffmpeg_path\n");
        else
            printf("  ffmpeg: %s\n", wtou8(ff).c_str());
    }
    // 只打母目录，不把账号目录（含自己的 wxid）写进日志
    for (auto& r : g_wxRoots) {
        fs::path p(r);
        printf("      %s\n", wtou8(p.parent_path().wstring()).c_str());
    }
    printf("===========================================\n\n");
    fflush(stdout);

    ensure_shortcut();
    setup_autostart(g_autostart);

    // --- 启动组件 ---
    std::thread toast_thread([]() { toast_worker_loop(); });
    std::thread tray_thread([]() { run_tray_loop(); });
    Sleep(300);
    if (g_silent) {
        HWND h = GetConsoleWindow();
        ShowWindow(h, SW_HIDE);
        FreeConsole();
    }

    std::thread srv_thread([]() { start_push_server(); });
    Sleep(200);

    if (!g_quietStart) enqueue_toast(_("banner"), _("started"));

    hook_watchdog();   // 阻塞，内含循环

    // --- 收尾 ---
    g_running = false;
    if (g_svr) g_svr->stop();
    g_toastCv.notify_all();          // 唤醒 toast 线程退出
    srv_thread.join();
    toast_thread.join();
    tray_thread.join();
    return 0;
}
