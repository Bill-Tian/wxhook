#include "pch.h"
#include "send_msg.h"
#include "wechat.h"           // GetWeChatWinBase / GetLoginState / WideToUtf8
#include "http_client.h"      // PostLog
#include "json.hpp"           // nlohmann::json
#include "wx_offsets.h"       // WxSendMessageMgr / WxSendTextMsg / WxFreeChatMsg

#include <string>
#include <cstring>
#include <cstdint>

using json = nlohmann::json;

// ── 微信内部字符串结构（x64 布局，共 0x20 字节）──────────
//  +0x00  wchar_t* ptr     指向宽字符串数据 (8B)
//  +0x08  int      len     长度（字符数，不含 \0）(4B)
//  +0x0C  int      maxlen  最大容量 (4B)
//  +0x10  int64_t  pad1    UTF-8 缓存指针 (8B)
//  +0x18  int64_t  pad2    UTF-8 长度 (8B)
struct WeChatString {
    wchar_t* ptr;
    int      len;
    int      maxlen;
    int64_t  pad1;
    int64_t  pad2;
};

// ── 线程安全：微信发送函数非线程安全，HTTP 并发请求需加锁 ─────────
static CRITICAL_SECTION g_send_cs;
static bool g_send_cs_init = []() {
    InitializeCriticalSectionAndSpinCount(&g_send_cs, 0x400);
    return true;
}();

// ── POD 函数：x64 函数指针调用微信发送消息 ──────────────────────────
static int DoSendText(wchar_t* wxid_buf, wchar_t* msg_buf) {
    WeChatString to_user  = { wxid_buf, static_cast<int>(wcslen(wxid_buf)), static_cast<int>(wcslen(wxid_buf)), 0, 0 };
    WeChatString text_msg = { msg_buf,  static_cast<int>(wcslen(msg_buf)),  static_cast<int>(wcslen(msg_buf)),  0, 0 };
    char at_list[0x20]    = { 0 };

    uintptr_t base = GetWeChatWinBase();
    if (!base) return -1;

    using SendMessageMgrFn = uintptr_t(__fastcall*)();
    using SendTextMsgFn    = uintptr_t(__fastcall*)(
        void* chat_msg_out,
        WeChatString* to_wxid,
        WeChatString* content,
        void* at_user_list,
        int type,
        int flag,
        int p7,
        uint64_t p8
    );
    using FreeChatMsgFn    = void(__fastcall*)(void* chat_msg);

    auto get_mgr       = reinterpret_cast<SendMessageMgrFn>(base + WxSendMessageMgr);
    auto send_text_msg = reinterpret_cast<SendTextMsgFn>(base + WxSendTextMsg);
    auto free_chat_msg = reinterpret_cast<FreeChatMsgFn>(base + WxFreeChatMsg);

    // chat_msg 缓冲：64 位下 ChatMsg 大小为 0x460 字节，预留 0x480 字节并 16 字节对齐
    alignas(16) char chat_msg[0x480] = { 0 };
    int success = -1;

    __try {
        get_mgr();
        uintptr_t ret = send_text_msg(chat_msg, &to_user, &text_msg, at_list, 1, 1, 0, 0);
        success = (ret != 0) ? 1 : 0;
        free_chat_msg(chat_msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        success = -1;
    }
    return success;
}

// UTF-8 → wstring
static std::wstring Utf8ToWString(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    return w;
}

std::string SendMsgResponse(const std::string& body) {
    // 1) 先校验登录状态
    int state = GetLoginState();
    if (state <= 0) {
        PostLog("发送消息: 未登录 state=" + std::to_string(state));
        json j;
        j["code"]  = 401;
        j["msg"]   = "not logged in";
        j["state"] = state;
        return j.dump();
    }

    // 2) 解析 JSON
    std::string wxid_utf8, content_utf8;
    try {
        json j = json::parse(body);
        wxid_utf8    = j.value("wxid", "");
        content_utf8 = j.value("content", "");
    } catch (...) {
        PostLog("发送消息: JSON 解析失败");
        json j;
        j["code"] = 400;
        j["msg"]  = "invalid json";
        return j.dump();
    }

    if (wxid_utf8.empty() || content_utf8.empty()) {
        PostLog("发送消息: wxid 或 content 为空");
        json j;
        j["code"] = 400;
        j["msg"]  = "wxid and content required";
        return j.dump();
    }

    // 3) UTF-8 → wchar_t 并拷贝到堆
    std::wstring wxid_w    = Utf8ToWString(wxid_utf8);
    std::wstring content_w = Utf8ToWString(content_utf8);

    wchar_t* wxid_buf = new wchar_t[wxid_w.size() + 1];
    wmemcpy(wxid_buf, wxid_w.c_str(), wxid_w.size() + 1);

    wchar_t* msg_buf = new wchar_t[content_w.size() + 1];
    wmemcpy(msg_buf, content_w.c_str(), content_w.size() + 1);

    // 4) 加锁调用微信发送函数
    int success = -1;
    {
        EnterCriticalSection(&g_send_cs);
        success = DoSendText(wxid_buf, msg_buf);
        LeaveCriticalSection(&g_send_cs);
    }

    delete[] wxid_buf;
    delete[] msg_buf;

    // 5) 返回结果
    PostLog("发送消息: wxid=" + wxid_utf8 +
            " success=" + std::to_string(success));

    json j;
    if (success != 0 && success != -1) {
        j["code"] = 0;
        j["msg"]  = "ok";
    } else {
        j["code"] = 500;
        j["msg"]  = "send failed";
        j["ret"]  = success;
    }
    return j.dump();
}
