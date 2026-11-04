#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
64 位微信 DLL 注入工具 (适用于 WeChat 3.9.12.57 x64)
注意：请使用 64 位 Python 运行本脚本（必要时以管理员权限运行）。

用法：
  python tools/inject_x64.py                           # 自动查找 WeChat.exe 并注入默认路径下的 wxhook.dll
  python tools/inject_x64.py --dll x64/Release/wxhook.dll
  python tools/inject_x64.py --pid 12345 --dll x64/Debug/wxhook.dll
"""

import argparse
import ctypes
from ctypes import wintypes
import os
import struct
import subprocess
import sys

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04


def find_wechat_pid() -> int:
    output = subprocess.check_output(
        ["tasklist", "/FI", "IMAGENAME eq WeChat.exe", "/FO", "CSV", "/NH"],
         encoding="mbcs",
        errors="ignore",
    )
    for line in output.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == "wechat.exe":
            return int(parts[1])
    return 0


def find_default_dll() -> str:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates = [
        os.path.join(root, "x64", "Release", "wxhook.dll"),
        os.path.join(root, "x64", "Debug", "wxhook.dll"),
        os.path.join(root, "wxhook.dll"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    return candidates[0]


def inject_dll(pid: int, dll_path: str) -> bool:
    if struct.calcsize("P") * 8 != 64:
        print("[-] 错误：当前 Python 为 32 位，请使用 64 位 Python 注入 64 位 WeChat.exe")
        return False

    dll_abspath = os.path.abspath(dll_path)
    if not os.path.isfile(dll_abspath):
        print(f"[-] 找不到 DLL 文件: {dll_abspath}")
        return False

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE

    kernel32.VirtualAllocEx.argtypes = [
        wintypes.HANDLE,
        wintypes.LPVOID,
        ctypes.c_size_t,
        wintypes.DWORD,
        wintypes.DWORD,
    ]
    kernel32.VirtualAllocEx.restype = wintypes.LPVOID

    kernel32.WriteProcessMemory.argtypes = [
        wintypes.HANDLE,
        wintypes.LPVOID,
        wintypes.LPCVOID,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel32.WriteProcessMemory.restype = wintypes.BOOL

    kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    kernel32.GetModuleHandleW.restype = wintypes.HMODULE

    kernel32.GetProcAddress.argtypes = [wintypes.HMODULE, wintypes.LPCSTR]
    kernel32.GetProcAddress.restype = wintypes.LPVOID

    kernel32.CreateRemoteThread.argtypes = [
        wintypes.HANDLE,
        wintypes.LPVOID,
        ctypes.c_size_t,
        wintypes.LPVOID,
        wintypes.LPVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel32.CreateRemoteThread.restype = wintypes.HANDLE

    h_process = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not h_process:
        err = ctypes.get_last_error()
        print(f"[-] OpenProcess(PID={pid}) 失败，错误码={err}（请尝试以管理员身份运行）")
        return False

    try:
        dll_bytes = (dll_abspath + "\0").encode("utf-16le")
        remote_mem = kernel32.VirtualAllocEx(
            h_process, None, len(dll_bytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        )
        if not remote_mem:
            print(f"[-] VirtualAllocEx 失败，错误码={ctypes.get_last_error()}")
            return False

        written = ctypes.c_size_t(0)
        if not kernel32.WriteProcessMemory(
            h_process, remote_mem, dll_bytes, len(dll_bytes), ctypes.byref(written)
        ):
            print(f"[-] WriteProcessMemory 失败，错误码={ctypes.get_last_error()}")
            return False

        h_kernel32 = kernel32.GetModuleHandleW("kernel32.dll")
        load_library_w = kernel32.GetProcAddress(h_kernel32, b"LoadLibraryW")
        if not load_library_w:
            print("[-] 获取 LoadLibraryW 地址失败")
            return False

        thread_id = wintypes.DWORD(0)
        h_thread = kernel32.CreateRemoteThread(
            h_process, None, 0, load_library_w, remote_mem, 0, ctypes.byref(thread_id)
        )
        if not h_thread:
            print(f"[-] CreateRemoteThread 失败，错误码={ctypes.get_last_error()}")
            return False

        kernel32.WaitForSingleObject(h_thread, 5000)
        kernel32.CloseHandle(h_thread)
        kernel32.VirtualFreeEx(h_process, remote_mem, 0, MEM_RELEASE)
        print(f"[+] 成功将 {dll_abspath} 注入到 WeChat.exe (PID={pid})")
        return True
    finally:
        kernel32.CloseHandle(h_process)


def main():
    parser = argparse.ArgumentParser(description="64 位 WeChat DLL 注入工具")
    parser.add_argument("--pid", type=int, default=0, help="目标 WeChat.exe 进程 PID（默认自动查找）")
    parser.add_argument("--dll", type=str, default=find_default_dll(), help="wxhook.dll 路径")
    args = parser.parse_args()

    pid = args.pid or find_wechat_pid()
    if not pid:
        print("[-] 未找到正在运行的 WeChat.exe 进程，请先启动微信")
        sys.exit(1)

    if not inject_dll(pid, args.dll):
        sys.exit(1)


if __name__ == "__main__":
    main()

