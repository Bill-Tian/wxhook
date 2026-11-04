#pragma once

#include <string>
#include <cstdint>
#include <windows.h>

// WeChatWin.dll 基址（DLL 注入到微信进程后该模块已加载，64 位下返回 uintptr_t）
uintptr_t GetWeChatWinBase();

// 在 target_addr 附近 (+/-1.5GB 内) 分配可执行内存页，供 x64 5 字节相对跳转 (E8/E9 rel32) 跳板使用
void* AllocTrampolineNear(uintptr_t target_addr, size_t size = 64);

// 读取微信内部 MSVC x64 std::string（兼容 SSO 内联缓冲与堆指针，纯 POD + SEH 保护）
bool ReadWxStdStringPod(uintptr_t addr, char* out_buf, size_t out_size);

// 登录状态：
//   调用 base + WxGetAccountService 取 AccountService 指针，读取其 +WxLoginStateOffset 的值。
//   返回 -1 表示读取失败；0 表示未登录；>0 视为已登录。
int GetLoginState();

// 宽字符 -> UTF-8 字符串
std::string WideToUtf8(const wchar_t* w);

// 获取微信缓存目录（我的文档目录，如 C:\Users\xxx\Documents）
// 图片/视频的相对路径需要拼接该目录 + \WeChat Files\ 才是完整路径
std::wstring GetWeChatCachePath();
