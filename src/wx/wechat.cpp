#include "pch.h"
#include "wechat.h"
#include "wx_offsets.h"
#include <ShlObj.h>
#include <cstring>

uintptr_t GetWeChatWinBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(L"WeChatWin.dll"));
}

void* AllocTrampolineNear(uintptr_t target_addr, size_t size) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    uintptr_t aligned = target_addr & ~(gran - 1);

    // 在 target_addr 前后 1.5GB 范围内搜索空闲页，确保 32 位有符号偏移不溢出
    const uintptr_t max_dist = 0x60000000ULL;
    for (uintptr_t offset = gran; offset < max_dist; offset += gran) {
        if (aligned > offset) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(aligned - offset), size,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
        void* p = VirtualAlloc(reinterpret_cast<void*>(aligned + offset), size,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return nullptr;
}

bool ReadWxStdStringPod(uintptr_t addr, char* out_buf, size_t out_size) {
    if (!out_buf || out_size == 0) return false;
    out_buf[0] = '\0';
    if (!addr) return false;

    __try {
        // MSVC x64 std::string 内存布局：
        // +0x00: char buf[16] 或 char* ptr
        // +0x10: uint64_t size
        // +0x18: uint64_t capacity (SSO 时为 0xF，堆分配时 >= 0x10)
        uint64_t size     = *reinterpret_cast<uint64_t*>(addr + 0x10);
        uint64_t capacity = *reinterpret_cast<uint64_t*>(addr + 0x18);

        const char* src = nullptr;
        if (capacity >= 0x10 && capacity < 0x100000) {
            src = *reinterpret_cast<const char**>(addr);
        } else {
            src = reinterpret_cast<const char*>(addr);
        }

        if (!src) return false;
        size_t copy_len = (size > 0 && size < out_size) ? static_cast<size_t>(size) : strnlen(src, out_size - 1);
        if (copy_len >= out_size) copy_len = out_size - 1;
        memcpy(out_buf, src, copy_len);
        out_buf[copy_len] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_buf[0] = '\0';
        return false;
    }
}

int GetLoginState() {
    uintptr_t base = GetWeChatWinBase();
    if (!base) return -1;

    uintptr_t account_service_addr = base + WxGetAccountService;
    uintptr_t service_addr = 0;

    __try {
        using FuncType = uintptr_t(__fastcall*)();
        FuncType f = reinterpret_cast<FuncType>(account_service_addr);
        service_addr = f();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }

    if (!service_addr) return -1;

    int state = 0;
    __try {
        uint8_t flag = *reinterpret_cast<uint8_t*>(service_addr + WxLoginStateOffset);
        DWORD legacy = *reinterpret_cast<DWORD*>(service_addr + 0x700);
        state = (flag != 0) ? static_cast<int>(flag) : static_cast<int>(legacy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return state;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len - 1, '\0'); // 去掉末尾 \0
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring GetWeChatCachePath() {
    wchar_t szPath[MAX_PATH] = { 0 };
    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, szPath);
    if (SUCCEEDED(hr)) {
        return std::wstring(szPath);
    }
    return L"";
}
