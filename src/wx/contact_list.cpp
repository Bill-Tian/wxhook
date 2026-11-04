#include "pch.h"
#include "contact_list.h"
#include "wechat.h"          // GetWeChatWinBase / GetLoginState / WideToUtf8
#include "http_client.h"     // PostLog
#include "json.hpp"          // nlohmann::json
#include "wx_offsets.h"      // WxGetServiceMgr / WxGetContactList / Contact*

#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

using json = nlohmann::json;

// ── 微信内部字符串结构（x64 布局）──────
struct WxString {
    wchar_t* ptr;
    int      length;
    int      max_length;
};

// ── 联系人节点（仅保留实际使用的字段）──────────────────────────
struct Contact {
    WxString wxid;            // 微信ID/群ID
    WxString custom_account;  // 微信号
    WxString encrypt_name;    // 昵称（微信内部字段名）
    WxString remark;          // 备注
    WxString pinyin_all;      // 备注备选字段
    DWORD    del_flag;        // 删除标志
    DWORD    type;            // 联系人类型
    DWORD    verify_flag;     // 验证标志
};

// ── 线程安全：微信联系人函数非线程安全，HTTP 并发请求需加锁 ─────────
static CRITICAL_SECTION g_contact_cs;
static bool g_contact_cs_init = []() {
    InitializeCriticalSectionAndSpinCount(&g_contact_cs, 0x400);
    return true;
}();

// ── POD 函数：调用微信内部获取联系人列表（x64 函数指针）──────────────
// 输出 contact[0]=start, contact[1]=end, contact[2]=cap，调用者遍历 [start, end)
static int GetAllContactInternal(uintptr_t* out_start, uintptr_t* out_end) {
    uintptr_t base = GetWeChatWinBase();
    if (!base) return 0;

    using GetServiceMgrFn  = uintptr_t(__fastcall*)();
    using GetContactListFn = uint8_t(__fastcall*)(uintptr_t mgr, uintptr_t* out_vector);

    auto get_instance     = reinterpret_cast<GetServiceMgrFn>(base + WxGetServiceMgr);
    auto contact_get_list = reinterpret_cast<GetContactListFn>(base + WxGetContactList);

    uintptr_t contact[3] = { 0, 0, 0 };
    int success = 0;

    __try {
        uintptr_t mgr = get_instance();
        uint8_t ret = contact_get_list(mgr, contact);
        success = static_cast<int>(ret);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    if (out_start) *out_start = contact[0];
    // MSVC x64 std::vector: [0]=_Myfirst, [1]=_Mylast, [2]=_Myend
    if (out_end)   *out_end   = (contact[1] != 0) ? contact[1] : contact[2];
    return success;
}

// ── POD 函数：SEH 保护读取单个联系人节点（64-bit 偏移）─────────────────
// 返回 true 表示成功读取，false 表示内存访问异常
static bool ReadContactNodeSEH(uintptr_t node_addr, Contact* out) {
    __try {
        out->wxid.ptr                  = *reinterpret_cast<wchar_t**>(node_addr + ContactWxidPtr);
        out->wxid.length               = *reinterpret_cast<int*>(node_addr + ContactWxidLen);
        out->wxid.max_length           = *reinterpret_cast<int*>(node_addr + ContactWxidMax);

        out->custom_account.ptr        = *reinterpret_cast<wchar_t**>(node_addr + ContactCustomAccountPtr);
        out->custom_account.length     = *reinterpret_cast<int*>(node_addr + ContactCustomAccountLen);
        out->custom_account.max_length = *reinterpret_cast<int*>(node_addr + ContactCustomAccountMax);

        out->remark.ptr                = *reinterpret_cast<wchar_t**>(node_addr + ContactRemarkPtr);
        out->remark.length             = *reinterpret_cast<int*>(node_addr + ContactRemarkLen);
        out->remark.max_length         = *reinterpret_cast<int*>(node_addr + ContactRemarkMax);

        out->encrypt_name.ptr          = *reinterpret_cast<wchar_t**>(node_addr + ContactEncryptNamePtr);
        out->encrypt_name.length       = *reinterpret_cast<int*>(node_addr + ContactEncryptNameLen);
        out->encrypt_name.max_length   = *reinterpret_cast<int*>(node_addr + ContactEncryptNameMax);

        out->pinyin_all.ptr            = *reinterpret_cast<wchar_t**>(node_addr + ContactPinyinAllPtr);
        out->pinyin_all.length         = *reinterpret_cast<int*>(node_addr + ContactPinyinAllLen);
        out->pinyin_all.max_length     = *reinterpret_cast<int*>(node_addr + ContactPinyinAllMax);

        out->del_flag                  = *reinterpret_cast<DWORD*>(node_addr + ContactDelFlag);
        out->type                      = *reinterpret_cast<DWORD*>(node_addr + ContactType);
        out->verify_flag               = *reinterpret_cast<DWORD*>(node_addr + ContactVerifyFlag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── 对象层：循环读取联系人到 vector ──────────────────────────────
// 单节点读取失败则停止遍历，已读部分仍可返回（容错降级）
static void ReadContactsToVector(uintptr_t start, uintptr_t end, std::vector<Contact>& vec) {
    while (start + ContactNodeSize <= end) {
        Contact temp = {};
        if (!ReadContactNodeSEH(start, &temp)) {
            PostLog("获取好友列表: 读取节点异常 offset=" + std::to_string(start));
            break;
        }
        vec.push_back(temp);
        start += ContactNodeSize;
    }
}

// ── 安全将 WxString 转为 UTF-8 字符串 ─────────────────────────────
// ptr 为空或 length 非法时返回空字符串；length 上限 4096 防止越界
static std::string SafeWxToUtf8(const WxString& s) {
    if (!s.ptr || s.length <= 0) return {};
    int len = s.length;
    if (len > 4096) len = 4096;
    std::wstring w(s.ptr, s.ptr + len);
    return WideToUtf8(w.c_str());
}

std::string GetContactListResponse() {
    // 1) 校验登录状态
    int state = GetLoginState();
    if (state <= 0) {
        PostLog("获取好友列表: 未登录 state=" + std::to_string(state));
        json j;
        j["code"]  = 401;
        j["msg"]   = "not logged in";
        j["state"] = state;
        return j.dump();
    }

    // 2) 加锁调用微信获取联系人列表
    uintptr_t start = 0, end = 0;
    int success = 0;
    {
        EnterCriticalSection(&g_contact_cs);
        success = GetAllContactInternal(&start, &end);
        LeaveCriticalSection(&g_contact_cs);
    }

    if (success == 0 || start == 0 || end == 0 || start >= end) {
        PostLog("获取好友列表: 调用失败 success=" + std::to_string(success));
        json j;
        j["code"] = 500;
        j["msg"]  = "get contact list failed";
        j["ret"]  = success;
        return j.dump();
    }

    // 3) 读取联系人到 vector（单节点 SEH 保护，出错停止读取）
    std::vector<Contact> vec;
    ReadContactsToVector(start, end, vec);

    PostLog("获取好友列表: count=" + std::to_string(vec.size()));

    // 4) 构造 JSON
    json result_json = json::array();

    // 数量异常大时降级：只返回 wxid 列表（防止返回数据过大）
    if (vec.size() > 10000) {
        for (size_t i = 0; i < vec.size(); i++) {
            result_json.push_back(SafeWxToUtf8(vec[i].wxid));
        }
    } else {
        for (size_t i = 0; i < vec.size(); i++) {
            const Contact& c = vec[i];
            std::string wxid = SafeWxToUtf8(c.wxid);

            json item;
            item["wxid"]     = wxid;
            item["nickname"] = SafeWxToUtf8(c.encrypt_name);

            // 群聊（@chatroom）：wxcount/remark 为空
            if (wxid.find("@chatroom") != std::string::npos) {
                item["wxcount"] = "";
                item["remark"]  = "";
            } else {
                item["wxcount"] = SafeWxToUtf8(c.custom_account);
                std::string remark = SafeWxToUtf8(c.pinyin_all);
                if (remark.empty()) {
                    remark = SafeWxToUtf8(c.remark);
                }
                item["remark"]  = remark;
            }
            result_json.push_back(item);
        }
    }

    // 5) 返回结果
    json j;
    j["code"]   = 0;
    j["msg"]    = "ok";
    j["count"]  = vec.size();
    j["result"] = result_json;

    std::string data;
    try {
        data = j.dump();
    } catch (...) {
        PostLog("获取好友列表: JSON 序列化失败");
        json err;
        err["code"] = 500;
        err["msg"]  = "json dump failed";
        return err.dump();
    }
    return data;
}
