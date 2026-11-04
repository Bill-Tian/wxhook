#include "pch.h"
#include "self_info.h"
#include "wechat.h"
#include "http_client.h"      // PostLog
#include "wx_offsets.h"       // WxID / WxCount / ... 偏移宏
#include "json.hpp"           // nlohmann::json

#include <string>
#include <cstring>

using json = nlohmann::json;

namespace {

// 个人信息原始字段（POD 缓冲，便于在 SEH 保护下读取）
struct SelfInfoRaw {
    char wxcount[0x300];
    char wxid[0x300];
    char nation[0x300];
    char province[0x300];
    char city[0x300];
    char phonenumber[0x300];
    char nickname[0x300];
    wchar_t cachedir[0x300];
    DWORD sex;
    bool valid;
    SelfInfoRaw() { memset(this, 0, sizeof(*this)); valid = false; }
};

// SEH 保护读取，避免单点崩溃导致整个微信进程退出（64位 MSVC std::string 兼容）
bool ReadSelfInfoRaw(SelfInfoRaw& out) {
    uintptr_t base = GetWeChatWinBase();
    if (!base) return false;

    __try {
        ReadWxStdStringPod(base + WxCount,       out.wxcount,     sizeof(out.wxcount));
        ReadWxStdStringPod(base + WxID,          out.wxid,        sizeof(out.wxid));
        ReadWxStdStringPod(base + WxNation,      out.nation,      sizeof(out.nation));
        ReadWxStdStringPod(base + WxProvince,    out.province,    sizeof(out.province));
        ReadWxStdStringPod(base + WxCity,        out.city,        sizeof(out.city));
        ReadWxStdStringPod(base + WxPhoneNumber, out.phonenumber, sizeof(out.phonenumber));
        ReadWxStdStringPod(base + WxNickName,    out.nickname,    sizeof(out.nickname));

        // 缓存目录（64 位 Unicode 指针）
        const wchar_t* cache_ptr = *reinterpret_cast<const wchar_t**>(base + WxCacheDir);
        if (cache_ptr) {
            swprintf_s(out.cachedir, L"%s", cache_ptr);
        }

        DWORD sex = *reinterpret_cast<DWORD*>(base + WxSex);
        if (sex != 1 && sex != 2) {
            sex = *reinterpret_cast<DWORD*>(base + WxSexAlt);
        }
        out.sex = sex;
        out.valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.valid = false;
        return false;
    }
}

} // namespace

std::string GetLoginStateResponse() {
    int state = GetLoginState();
    PostLog("登录状态查询 state=" + std::to_string(state));
    json j;
    j["state"]  = state;
    j["logged"] = (state > 0);
    return j.dump();
}

std::string GetSelfInfoResponse() {
    // 1) 先获取登录状态
    int state = GetLoginState();
    PostLog("获取个人信息: 登录状态 state=" + std::to_string(state));
    if (state <= 0) {
        json j;
        j["code"]  = 401;
        j["msg"]   = "not logged in";
        j["state"] = state;
        return j.dump();
    }

    // 2) 已登录，读取个人信息字段
    SelfInfoRaw raw;
    if (!ReadSelfInfoRaw(raw)) {
        PostLog("获取个人信息: 读取字段异常 (SEH 捕获)");
        json j;
        j["code"] = 500;
        j["msg"]  = "read self info failed";
        return j.dump();
    }

    // 字段字节按 UTF-8 处理
    json j;
    j["wxcount"]     = raw.wxcount[0] ? std::string(raw.wxcount) : std::string("NULL");
    j["wxid"]        = std::string(raw.wxid);
    j["nation"]      = std::string(raw.nation);
    j["province"]    = std::string(raw.province);
    j["city"]        = std::string(raw.city);
    j["phonenumber"] = std::string(raw.phonenumber);
    j["nickname"]    = std::string(raw.nickname);
    j["cachedir"]    = WideToUtf8(raw.cachedir);
    switch (raw.sex) {
    case 1:  j["wxsex"] = "男";     break;
    case 2:  j["wxsex"] = "女";     break;
    default: j["wxsex"] = "未设置"; break;
    }

    std::string out;
    try {
        out = j.dump();
    } catch (const std::exception& e) {
        PostLog(std::string("获取个人信息: json dump 失败 ") + e.what());
        json f;
        f["code"] = 500;
        f["msg"]  = std::string("json dump failed: ") + e.what();
        out = f.dump();
    }

    PostLog(std::string("获取个人信息: 读取完成 wxid=") +
            (raw.wxid[0] ? raw.wxid : "NULL"));
    return out;
}
