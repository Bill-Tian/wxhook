#include "pch.h"
#include "receive_msg.h"
#include "wechat.h"          // GetWeChatWinBase / AllocTrampolineNear / WideToUtf8
#include "http_client.h"     // PostLog / PostJson
#include "config.h"          // cfg::MSG_*
#include "image_decoder.h"   // DecodeImageAsync（图片落盘解密）
#include "json.hpp"          // nlohmann::json
#include "wx_offsets.h"      // WxReciveMessage / WxReciveMessageCall / MsgTypeOffset ...

#include <string>
#include <cstring>
#include <cstdint>

using json = nlohmann::json;

namespace {

// Hook 运行状态 & 被覆盖指令的备份
static bool      g_installed      = false;
static uintptr_t g_target_addr    = 0;     // GetWeChatWinBase() + WxReciveMessage
static uintptr_t g_over_call_addr = 0;     // GetWeChatWinBase() + WxReciveMessageCall
static uintptr_t g_ret_addr       = 0;     // g_target_addr + 5
static void*     g_trampoline     = nullptr;
static BYTE      g_orig_bytes[16] = {0};   // 被覆盖的原指令备份（前 5 字节）

// ---------- inline hook 底层：修改目标入口的 5 字节为 JMP rel32 ----------
static bool Write5ByteJump(uintptr_t target, uintptr_t to)
{
    if (!target || !to) return false;
    intptr_t rel = static_cast<intptr_t>(to) - static_cast<intptr_t>(target) - 5;
    if (rel < INT32_MIN || rel > INT32_MAX) return false;

    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 5, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;
    if (g_orig_bytes[0] == 0 && g_orig_bytes[1] == 0 &&
        g_orig_bytes[2] == 0 && g_orig_bytes[3] == 0 && g_orig_bytes[4] == 0) {
        memcpy(g_orig_bytes, reinterpret_cast<LPCVOID>(target), 5);
    }
    BYTE* p = reinterpret_cast<BYTE*>(target);
    p[0] = 0xE9;  // JMP rel32
    *reinterpret_cast<int32_t*>(&p[1]) = static_cast<int32_t>(rel);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(target), 5);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 5, old_protect, &old_protect);
    return true;
}

static bool Restore5ByteJump(uintptr_t target)
{
    if (!target) return false;
    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 5, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;
    memcpy(reinterpret_cast<LPVOID>(target), g_orig_bytes, 5);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(target), 5);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 5, old_protect, &old_protect);
    return true;
}

// ---------- 微信结构解析辅助：纯 SEH，不引入任何 C++ 对象（避免 C2712） ----------
static uintptr_t SafeReadPtr(uintptr_t addr)
{
    if (!addr) return 0;
    uintptr_t ptr = 0;
    __try {
        ptr = *reinterpret_cast<uintptr_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return ptr;
}

static const wchar_t* SafeReadWxWideCharPtr(uintptr_t addr)
{
    if (!addr) return nullptr;
    uintptr_t ptr = SafeReadPtr(addr);
    if (!ptr) return nullptr;
    const wchar_t* w = nullptr;
    __try {
        w = reinterpret_cast<const wchar_t*>(ptr);
        (void)w[0];  // 探测可访问性
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return w;
}

// 无 std::string 版宽串转 UTF-8，直接写入调用方提供的 char 缓冲
static bool WideToUtf8Buf(const wchar_t* w, char* out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!w) return true;
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    if ((size_t)need > out_size) need = (int)out_size - 1;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, need, nullptr, nullptr);
    out[need] = '\0';
    return true;
}

static bool SafeReadWxWideToUtf8Buf(uintptr_t addr, char* out_buf, size_t buf_size)
{
    const wchar_t* w = SafeReadWxWideCharPtr(addr);
    return WideToUtf8Buf(w, out_buf, buf_size);
}

// 消息类型枚举
enum MsgType : int {
    WxTextMsg            = 1,
    WxImageMsg           = 3,
    WxVoiceMsg           = 34,
    WxVideoMsg           = 43,
    WxEmojiMsg           = 47,
    WxLocationMsg        = 48,
    WxAppMsg             = 49,
    WxSysSyncMsg         = 51,
    WxVoipMsg            = 50,
    WxRecallNotification = 10000,
};

// ---------- POD 原始字段缓冲（SEH 保护读取，全程无 C++ 对象） ----------
struct RawMsg {
    int      msgtype;
    int      isphonemsg;
    uint64_t msgid;
    char     wxid[512];
    char     sender[512];
    char     source[512];
    char     content[65536];  // 扩容至 64KB，完整容纳公众号多图文 XML（<mmreader>）
    char     filepath[1024];
    bool     ok;
    bool     skip;  // WxSysSyncMsg 静默跳过
};

// 本函数只包含 POD + __try，绝不引入 std::string 等有析构的对象（强制规避 C2712）
static bool ReadRawMsgPod(uintptr_t msg_base, RawMsg& raw)
{
    if (!msg_base) return false;
    memset(&raw, 0, sizeof(raw));

    __try {
        raw.msgtype    = *reinterpret_cast<int*>(msg_base + MsgTypeOffset);
        raw.isphonemsg = *reinterpret_cast<int*>(msg_base + IsPhoneMsg);
        raw.msgid      = *reinterpret_cast<uint64_t*>(msg_base + MsgIdOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (raw.msgtype == WxSysSyncMsg) {
        raw.skip = true;
        return false;
    }

    __try {
        SafeReadWxWideToUtf8Buf(msg_base + WxidOffset,          raw.wxid,    sizeof(raw.wxid));
        SafeReadWxWideToUtf8Buf(msg_base + GroupMsgSenderOffset, raw.sender,  sizeof(raw.sender));
        SafeReadWxWideToUtf8Buf(msg_base + MsgSourceOffset,     raw.source,  sizeof(raw.source));
        SafeReadWxWideToUtf8Buf(msg_base + MsgContentOffset,    raw.content, sizeof(raw.content));

        switch (raw.msgtype) {
        case WxImageMsg:
            SafeReadWxWideToUtf8Buf(msg_base + ImagePathOffset, raw.filepath, sizeof(raw.filepath));
            break;
        case WxVideoMsg:
            SafeReadWxWideToUtf8Buf(msg_base + VideoPathOffset, raw.filepath, sizeof(raw.filepath));
            break;
        case WxAppMsg:
            if (raw.content[0] != '\0' &&
                (strstr(raw.content, "<type>5</type>")  != nullptr ||
                 strstr(raw.content, "<type>33</type>") != nullptr)) {
                SafeReadWxWideToUtf8Buf(msg_base + XmlImagePathOffset, raw.filepath, sizeof(raw.filepath));
            } else if (raw.content[0] != '\0' &&
                       strstr(raw.content, "<type>6</type>") != nullptr) {
                SafeReadWxWideToUtf8Buf(msg_base + FilePathOffset,     raw.filepath, sizeof(raw.filepath));
            } else {
                SafeReadWxWideToUtf8Buf(msg_base + XmlImagePathOffset, raw.filepath, sizeof(raw.filepath));
            }
            break;
        default:
            break;
        }
        if (raw.filepath[0] == '\0') {
            strncpy_s(raw.filepath, sizeof(raw.filepath), "NULL", _TRUNCATE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    raw.ok = true;
    return true;
}

// 访问性预检：独立小函数，纯 __try，无 C++ 对象
static bool ProbeMsgBase(uintptr_t msg_base)
{
    if (!msg_base) return false;
    __try {
        (void)*reinterpret_cast<volatile DWORD*>(msg_base + MsgTypeOffset);
        (void)*reinterpret_cast<volatile uintptr_t*>(msg_base + MsgContentOffset);
        (void)*reinterpret_cast<volatile uintptr_t*>(msg_base + WxidOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ---------- 解析 & 上报：此处允许使用 C++ 对象 ----------
struct PostMsgParam {
    int         msgtype;
    int         isphonemsg;
    uint64_t    msgid;
    std::string wxid;
    std::string sender;
    std::string source;
    std::string content;
    std::string filepath;
};

static DWORD WINAPI PostMsgThread(LPVOID lp)
{
    PostMsgParam* p = (PostMsgParam*)lp;
    if (!p) return 1;
    try {
        // 日志也放到异步线程里，避免阻塞微信消息接收回调
        PostLog(std::string("接收消息: type=") + std::to_string(p->msgtype) +
                " wxid=" + p->wxid +
                (p->sender.empty() ? std::string() : std::string(" sender=") + p->sender));

        // 图片消息(type=3)：把 .dat 路径扔进异步解码队列（非阻塞）
        std::string fullpathUtf8;  // 完整路径的 UTF-8 版，用于 JSON
        if (p->msgtype == WxImageMsg && !p->filepath.empty() && p->filepath != "NULL") {
            // UTF-8 → wstring（filepath 是相对路径）
            int need = MultiByteToWideChar(CP_UTF8, 0, p->filepath.c_str(), -1, nullptr, 0);
            if (need > 0) {
                std::wstring relPath(need - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, p->filepath.c_str(), -1, &relPath[0], need);
                // 相对路径 → 完整路径：缓存目录 + \WeChat Files\ + 相对路径
                std::wstring fullPath = GetWeChatCachePath() + L"\\WeChat Files\\" + relPath;
                DecodeImageAsync(fullPath);
                // 完整路径 → UTF-8（JSON 里带上，方便接收端找解密后的图片）
                fullpathUtf8 = WideToUtf8(fullPath.c_str());
            }
        }

        json j;
        j["msgtype"]     = p->msgtype;
        j["isphonemsg"]  = p->isphonemsg;
        j["msgid"]       = p->msgid;
        j["wxid"]        = p->wxid;
        j["sender"]      = p->sender;
        j["source"]      = p->source;
        j["content"]     = p->content;
        j["filepath"]    = p->filepath;
        j["fullpath"]    = fullpathUtf8;
        j["isgroup"]     = !p->sender.empty();
        std::string body = j.dump();
        PostJson(cfg::MSG_HOST, cfg::MSG_PORT, cfg::MSG_PATH, body);
    } catch (...) {
        PostLog("接收消息: 线程 POST /message 异常");
    }
    delete p;
    return 0;
}

static void DispatchMsgAsync(PostMsgParam* param)
{
    if (!param) return;
    HANDLE h = CreateThread(nullptr, 0, PostMsgThread, param, 0, nullptr);
    if (h) CloseHandle(h);
    else   delete param;
}

// 封装 std::string + DispatchMsgAsync：无 __try，可安心使用 C++ 对象
static void ProcessParsedMsg(const RawMsg& raw)
{
    PostMsgParam* out = new PostMsgParam();
    out->msgtype     = raw.msgtype;
    out->isphonemsg  = raw.isphonemsg;
    out->msgid       = raw.msgid;
    out->wxid        = raw.wxid[0] ? raw.wxid : std::string("NULL");
    out->sender      = raw.sender;
    out->source      = raw.source;
    out->content     = raw.content;
    out->filepath    = raw.filepath;

    DispatchMsgAsync(out);
}

// 轻量异步日志：把 PostLog 扔到独立线程，绝不阻塞微信回调线程
struct LogParam {
    char msg[512];
};

static DWORD WINAPI PostLogThread(LPVOID lp)
{
    LogParam* p = (LogParam*)lp;
    if (p) {
        PostLog(p->msg);
        delete p;
    }
    return 0;
}

static void PostLogAsync(const char* msg)
{
    LogParam* p = new LogParam();
    strncpy_s(p->msg, sizeof(p->msg), msg, _TRUNCATE);
    HANDLE h = CreateThread(nullptr, 0, PostLogThread, p, 0, nullptr);
    if (h) CloseHandle(h);
    else   delete p;
}

// x64 __fastcall 回调：RCX = ChatMsg* (msg_base)
// 本函数不包含任何 __try / __except（避免对象展开冲突 C2712）
void __fastcall OnRecvMessageFromStack(uintptr_t msg_base)
{
    if (!ProbeMsgBase(msg_base)) return;

    // RawMsg 含 64KB 缓冲，采用堆分配避免撑爆微信回调线程栈
    RawMsg* raw = new RawMsg();
    if (!ReadRawMsgPod(msg_base, *raw)) {
        bool skip = raw->skip;
        delete raw;
        if (skip) return;  // WxSysSyncMsg 静默跳过
        PostLogAsync("接收消息: 读取原始字段失败 (SEH 捕获)");
        return;
    }

    ProcessParsedMsg(*raw);
    delete raw;
}

// 构建 x64 机器码跳板：
// 1. push rcx; sub rsp, 0x28 (保持 16 字节栈对齐 + 32 字节 Shadow Space)
// 2. movabs rax, OnRecvMessageFromStack; call rax
// 3. add rsp, 0x28; pop rcx
// 4. movabs rax, g_over_call_addr; call rax (执行被覆盖的 ChatMsg::~ChatMsg)
// 5. movabs rax, g_ret_addr; jmp rax        (跳回原指令 + 5)
static bool BuildRecvMessageTrampoline()
{
    if (!g_trampoline) {
        g_trampoline = AllocTrampolineNear(g_target_addr, 64);
        if (!g_trampoline) return false;
    }

    uint8_t code[48] = {};
    size_t i = 0;

    // push rcx
    code[i++] = 0x51;
    // sub rsp, 0x28
    code[i++] = 0x48; code[i++] = 0x83; code[i++] = 0xEC; code[i++] = 0x28;
    // movabs rax, OnRecvMessageFromStack
    code[i++] = 0x48; code[i++] = 0xB8;
    uintptr_t fn_addr = reinterpret_cast<uintptr_t>(&OnRecvMessageFromStack);
    memcpy(&code[i], &fn_addr, 8); i += 8;
    // call rax
    code[i++] = 0xFF; code[i++] = 0xD0;
    // add rsp, 0x28
    code[i++] = 0x48; code[i++] = 0x83; code[i++] = 0xC4; code[i++] = 0x28;
    // pop rcx
    code[i++] = 0x59;
    // movabs rax, g_over_call_addr
    code[i++] = 0x48; code[i++] = 0xB8;
    memcpy(&code[i], &g_over_call_addr, 8); i += 8;
    // call rax
    code[i++] = 0xFF; code[i++] = 0xD0;
    // movabs rax, g_ret_addr
    code[i++] = 0x48; code[i++] = 0xB8;
    memcpy(&code[i], &g_ret_addr, 8); i += 8;
    // jmp rax
    code[i++] = 0xFF; code[i++] = 0xE0;

    memcpy(g_trampoline, code, i);
    FlushInstructionCache(GetCurrentProcess(), g_trampoline, i);
    return true;
}

} // namespace

// ---------------- 对外入口 ----------------
bool InstallReceiveMessageHook()
{
    if (g_installed) return true;

    uintptr_t base = GetWeChatWinBase();
    if (!base) {
        PostLog("接收消息 Hook: 无法获取 WeChatWin 基址");
        return false;
    }
    g_target_addr    = base + WxReciveMessage;
    g_over_call_addr = base + WxReciveMessageCall;
    g_ret_addr       = g_target_addr + 5;

    if (!BuildRecvMessageTrampoline()) {
        PostLog("接收消息 Hook: 分配 x64 跳板内存失败");
        return false;
    }

    if (!Write5ByteJump(g_target_addr, reinterpret_cast<uintptr_t>(g_trampoline))) {
        PostLog("接收消息 Hook: 写入入口跳转失败");
        return false;
    }
    g_installed = true;
    PostLog("接收消息 Hook 安装成功");
    return true;
}

void UninstallReceiveMessageHook()
{
    if (!g_installed || !g_target_addr) return;
    if (Restore5ByteJump(g_target_addr)) {
        PostLog("接收消息 Hook 卸载成功");
    } else {
        PostLog("接收消息 Hook: 卸载失败（原字节恢复失败）");
    }
    g_installed = false;
}
