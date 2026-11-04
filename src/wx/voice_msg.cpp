#include "pch.h"
#include "voice_msg.h"
#include "wechat.h"          // GetWeChatWinBase / AllocTrampolineNear
#include "http_client.h"     // PostLog
#include "config.h"          // cfg::ENABLE_VOICE_SAVE / cfg::ENABLE_VOICE_CONVERT
#include "wx_offsets.h"      // WxReciveVoiceData / WxReciveVoiceDataCall / WxVoiceSvridOffset
#include "silk_dec.h"        // silk_dec::Decode（SILK→PCM）

#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <cstdint>

namespace {

// ---------- Hook 状态 & 原指令备份 ----------
static bool      g_installed       = false;
static uintptr_t g_voice_target    = 0;     // base + WxReciveVoiceData
static uintptr_t g_voice_over_call = 0;     // base + WxReciveVoiceDataCall
static uintptr_t g_voice_ret_addr  = 0;     // g_voice_target + 5
static void*     g_voice_trampoline = nullptr;
static BYTE      g_voice_orig_bytes[16] = {0};

// ---------- inline hook 底层（与 receive_msg.cpp 相同模式，各自独立备份） ----------
static bool Write5ByteJump(uintptr_t target, uintptr_t to)
{
    if (!target || !to) return false;
    intptr_t rel = static_cast<intptr_t>(to) - static_cast<intptr_t>(target) - 5;
    if (rel < INT32_MIN || rel > INT32_MAX) return false;

    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 5, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;
    if (g_voice_orig_bytes[0] == 0 && g_voice_orig_bytes[1] == 0 &&
        g_voice_orig_bytes[2] == 0 && g_voice_orig_bytes[3] == 0 && g_voice_orig_bytes[4] == 0) {
        memcpy(g_voice_orig_bytes, reinterpret_cast<LPCVOID>(target), 5);
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
    memcpy(reinterpret_cast<LPVOID>(target), g_voice_orig_bytes, 5);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(target), 5);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 5, old_protect, &old_protect);
    return true;
}

// ---------- 异步保存语音数据（避免阻塞微信语音回调线程） ----------
// 语音数据在 Hook 返回后可能被微信释放，所以必须先拷贝到堆再异步写盘
struct VoiceSaveParam {
    void*   data;     // malloc 的堆拷贝
    DWORD   length;
    int64_t svrid;
};

static DWORD WINAPI SaveVoiceThread(LPVOID lp)
{
    VoiceSaveParam* p = (VoiceSaveParam*)lp;
    if (!p) return 1;

    CreateDirectoryA("C:\\VoiceLogs", NULL);

    char silkPath[MAX_PATH];
    sprintf_s(silkPath, MAX_PATH, "C:\\VoiceLogs\\%lld.silk", (long long)p->svrid);

    // 已存在则跳过（避免重复保存）
    if (GetFileAttributesA(silkPath) != INVALID_FILE_ATTRIBUTES) {
        PostLog("语音保存: 文件已存在，跳过");
        free(p->data);
        delete p;
        return 0;
    }

    // ---------- 1. 写 .silk 原始文件 ----------
    HANDLE hFile = CreateFileA(silkPath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, p->data, p->length, &written, NULL);
        CloseHandle(hFile);
        PostLog(std::string("语音保存成功: ") + silkPath +
                " (size=" + std::to_string(p->length) + ")");
    } else {
        PostLog("语音保存: CreateFileA 失败");
    }

    // ---------- 2. SILK → WAV 转换 ----------
    if (cfg::ENABLE_VOICE_CONVERT) {
        std::vector<unsigned char> pcm;
        int sampleRate = 0;

        if (silk_dec::Decode(p->data, p->length, pcm, sampleRate) && !pcm.empty()) {
            char wavPath[MAX_PATH];
            sprintf_s(wavPath, MAX_PATH, "C:\\VoiceLogs\\%lld.wav", (long long)p->svrid);

            // WAV 文件头（44 字节，PCM 16-bit mono）
#pragma pack(push, 1)
            struct WavHeader {
                char     riff[4];       // "RIFF"
                uint32_t riffSize;      // 文件大小 - 8
                char     wave[4];       // "WAVE"
                char     fmt[4];        // "fmt "
                uint32_t fmtSize;       // 16
                uint16_t fmtTag;        // 1 = PCM
                uint16_t channels;      // 1 = mono
                uint32_t sampleRate;    // 24000
                uint32_t byteRate;      // sampleRate * channels * bits/8
                uint16_t blockAlign;    // channels * bits/8
                uint16_t bits;          // 16
                char     data[4];       // "data"
                uint32_t dataSize;      // PCM 字节数
            };
#pragma pack(pop)
            static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");

            WavHeader hdr;
            memcpy(hdr.riff, "RIFF", 4);
            hdr.riffSize = (uint32_t)(36 + pcm.size());
            memcpy(hdr.wave, "WAVE", 4);
            memcpy(hdr.fmt, "fmt ", 4);
            hdr.fmtSize    = 16;
            hdr.fmtTag     = 1;
            hdr.channels   = 1;
            hdr.sampleRate = (uint32_t)sampleRate;
            hdr.byteRate   = (uint32_t)sampleRate * 1 * 2;
            hdr.blockAlign = 1 * 2;
            hdr.bits       = 16;
            memcpy(hdr.data, "data", 4);
            hdr.dataSize   = (uint32_t)pcm.size();

            HANDLE hWav = CreateFileA(wavPath, GENERIC_WRITE, 0, NULL,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hWav != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(hWav, &hdr, sizeof(hdr), &written, NULL);
                WriteFile(hWav, pcm.data(), (DWORD)pcm.size(), &written, NULL);
                CloseHandle(hWav);
                PostLog(std::string("语音转换成功: ") + silkPath + " -> " + wavPath);
            } else {
                PostLog("语音转换: CreateFileA(wav) 失败");
            }
        } else {
            PostLog("语音转换: SILK 解码失败，仅保留 .silk");
        }
    }

    free(p->data);
    delete p;
    return 0;
}

static bool ReadVoiceSvridSEH(uintptr_t voice_struct, int64_t* out_svrid)
{
    if (!voice_struct || !out_svrid) return false;
    __try {
        *out_svrid = *reinterpret_cast<int64_t*>(voice_struct + WxVoiceSvridOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// x64 __fastcall 回调：
// 在 WxReciveVoiceData (0x22879F9) 处：
//   R15 = 语音结构指针 ([R15 + 0x48] = msgSvrID)
//   RDX = 语音数据指针 (data)
//   R8  = 语音数据长度 (length)
void __fastcall SaveVoiceFile64(uintptr_t voice_struct, void* data, size_t length)
{
    if (!data || length == 0 || length > 0x1000000) return;

    int64_t svrid = 0;
    if (!ReadVoiceSvridSEH(voice_struct, &svrid)) return;

    // 拷贝语音数据到堆（原始缓冲区在 Hook 返回后可能失效）
    void* copy = malloc(length);
    if (!copy) return;
    memcpy(copy, data, length);

    VoiceSaveParam* param = new VoiceSaveParam();
    param->data   = copy;
    param->length = static_cast<DWORD>(length);
    param->svrid  = svrid;

    HANDLE h = CreateThread(nullptr, 0, SaveVoiceThread, param, 0, nullptr);
    if (h) CloseHandle(h);
    else { free(copy); delete param; }
}

// 构建 x64 机器码跳板：
// 在 0x1822879f9 (call AutoBuffer::Write) 处：
//   RCX = AutoBuffer*, RDX = data, R8 = length, R15 = voice_struct
// 跳板流程：
// 1. push rcx; push rdx; push r8; push r9 (保存 4 个参数寄存器，共 32B，栈仍保持 16B 对齐)
// 2. sub rsp, 0x20 (分配 32B Shadow Space)
// 3. mov rcx, r15  (arg1 = voice_struct, arg2 = RDX = data, arg3 = R8 = length)
// 4. movabs rax, SaveVoiceFile64; call rax
// 5. add rsp, 0x20; pop r9; pop r8; pop rdx; pop rcx
// 6. movabs rax, g_voice_over_call; call rax (执行原 AutoBuffer::Write)
// 7. movabs rax, g_voice_ret_addr; jmp rax   (跳回原指令 + 5)
static bool BuildVoiceDataTrampoline()
{
    if (!g_voice_trampoline) {
        g_voice_trampoline = AllocTrampolineNear(g_voice_target, 64);
        if (!g_voice_trampoline) return false;
    }

    uint8_t code[64] = {};
    size_t i = 0;

    // push rcx; push rdx; push r8; push r9
    code[i++] = 0x51;
    code[i++] = 0x52;
    code[i++] = 0x41; code[i++] = 0x50;
    code[i++] = 0x41; code[i++] = 0x51;
    // sub rsp, 0x20
    code[i++] = 0x48; code[i++] = 0x83; code[i++] = 0xEC; code[i++] = 0x20;
    // mov rcx, r15 (49 8B CF)
    code[i++] = 0x49; code[i++] = 0x8B; code[i++] = 0xCF;
    // movabs rax, SaveVoiceFile64
    code[i++] = 0x48; code[i++] = 0xB8;
    uintptr_t fn_addr = reinterpret_cast<uintptr_t>(&SaveVoiceFile64);
    memcpy(&code[i], &fn_addr, 8); i += 8;
    // call rax
    code[i++] = 0xFF; code[i++] = 0xD0;
    // add rsp, 0x20
    code[i++] = 0x48; code[i++] = 0x83; code[i++] = 0xC4; code[i++] = 0x20;
    // pop r9; pop r8; pop rdx; pop rcx
    code[i++] = 0x41; code[i++] = 0x59;
    code[i++] = 0x41; code[i++] = 0x58;
    code[i++] = 0x5A;
    code[i++] = 0x59;
    // movabs rax, g_voice_over_call
    code[i++] = 0x48; code[i++] = 0xB8;
    memcpy(&code[i], &g_voice_over_call, 8); i += 8;
    // call rax
    code[i++] = 0xFF; code[i++] = 0xD0;
    // movabs rax, g_voice_ret_addr
    code[i++] = 0x48; code[i++] = 0xB8;
    memcpy(&code[i], &g_voice_ret_addr, 8); i += 8;
    // jmp rax
    code[i++] = 0xFF; code[i++] = 0xE0;

    memcpy(g_voice_trampoline, code, i);
    FlushInstructionCache(GetCurrentProcess(), g_voice_trampoline, i);
    return true;
}

} // namespace

// ---------------- 对外入口 ----------------
bool InstallVoiceDataHook()
{
    if (g_installed) return true;

    uintptr_t base = GetWeChatWinBase();
    if (!base) {
        PostLog("语音 Hook: 无法获取 WeChatWin 基址");
        return false;
    }
    g_voice_target    = base + WxReciveVoiceData;
    g_voice_over_call = base + WxReciveVoiceDataCall;
    g_voice_ret_addr  = g_voice_target + 5;

    if (!BuildVoiceDataTrampoline()) {
        PostLog("语音 Hook: 分配 x64 跳板内存失败");
        return false;
    }

    if (!Write5ByteJump(g_voice_target, reinterpret_cast<uintptr_t>(g_voice_trampoline))) {
        PostLog("语音 Hook: 写入入口跳转失败");
        return false;
    }
    g_installed = true;
    PostLog("语音 Hook 安装成功");
    return true;
}

void UninstallVoiceDataHook()
{
    if (!g_installed || !g_voice_target) return;
    if (Restore5ByteJump(g_voice_target)) {
        PostLog("语音 Hook 卸载成功");
    } else {
        PostLog("语音 Hook: 卸载失败（原字节恢复失败）");
    }
    g_installed = false;
}
