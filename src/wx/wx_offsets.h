#pragma once
#include <cstdint>

// ============================================================
// wx_offsets.h — 微信 3.9.12.57 (x64) 偏移地址
// WeChatWin.dll (PE32+ x86-64, ImageBase = 0x180000000)
// ============================================================

// ── 账号服务与登录状态 ──
#define WxGetAccountService   0x1B5CA40    // AccountService::Instance()
#define WxLoginStateOffset    0x7F8        // 登录状态标志 (uint8_t, >0 已登录)

// ── 接收消息 Hook (SyncMgr::doAddMsg) ──
#define WxReciveMessage       0x231D7E6    // 接收消息 Hook 点 (call ChatMsg::~ChatMsg)
#define WxReciveMessageCall   0x1B5CAC0    // 被覆盖的原始 call (ChatMsg::~ChatMsg)

// ── 个人信息 (AccountService 静态基址偏移) ──
#define WxID                  0x5A2AA40    // 微信ID (MSVC x64 std::string)
#define WxQRUrlOffset         0x5A30C28    // 登录二维码 URL 后缀 (MSVC x64 std::string)
#define WxCount               (WxID + 0x88)   // 微信号
#define WxNickName            (WxID + 0x168)  // 昵称
#define WxNation              (WxID + 0xE8)   // 国家
#define WxProvince            (WxID + 0x108)  // 省份
#define WxCity                (WxID + 0x128)  // 城市
#define WxPhoneNumber         (WxID + 0xA8)   // 手机号
#define WxCacheDir            (WxID - 0x20)   // 缓存目录 Unicode 指针 (wchar_t*)
#define WxSex                 (WxID + 0x2A8)  // 性别 (rbx + 0x320)
#define WxSexAlt              (WxID + 0x318)  // 性别备选偏移 (rbx + 0x390)

// ── 聊天记录 ChatMsg 字段偏移 (64-bit) ──
#define MsgIdOffset           0x30         // 消息 svrid (uint64_t)
#define MsgTypeOffset         0x38         // 消息类型 (int)
#define IsPhoneMsg            0x3C         // 是否手机/自身发送 (int)
#define WxidOffset            0x48         // 微信ID/群ID (WxString)
#define MsgContentOffset      0x88         // 消息内容 (WxString)
#define GroupMsgSenderOffset  0x220        // 群消息发送者 (WxString)
#define VideoPathOffset       0x260        // 视频路径 (WxString)
#define ImagePathOffset       0x280        // 图片路径 (WxString)
#define FilePathOffset        0x280        // 文件路径 (WxString)
#define XmlImagePathOffset    0x2A0        // XML文章图片路径 (WxString)
#define MsgSourceOffset       0x2C8        // 消息来源 (WxString)

// ── 图片/视频自动下载补丁 (ChatMgr::handlePreDownload) ──
// 原指令为 3 字节: 45 33 C9 (xor r9d, r9d) -> 补丁为: 41 B1 01 (mov r9b, 1)
#define WxPatchAutoDownloadImage  0x214BBF9
#define WxPatchAutoDownloadVideo  0x214C61F

// ── 语音数据接收 Hook (MultiDBMsgMgr::AddVoice) ──
#define WxReciveVoiceData     0x22879F9    // call AutoBuffer::Write
#define WxReciveVoiceDataCall 0x3CCE530    // AutoBuffer::Write
#define WxVoiceSvridOffset    0x48         // [R15 + 0x48] = msgSvrID (uint64_t)

// ── 发送消息 (SendMessageMgr) ──
#define WxSendMessageMgr      0x1B5B210    // 获取发送消息管理器
#define WxSendTextMsg         0x22D4A90    // 发送文本消息 (SendMessageMgr::sendMsg)
#define WxFreeChatMsg         0x1B5CAC0    // 释放 ChatMsg (ChatMsg::~ChatMsg)

// ── 获取好友列表 (ContactMgr::getList) ──
#define WxGetServiceMgr       0x1B5B210    // 获取服务管理器
#define WxGetContactList      0x21ACBE0    // 获取联系人列表

// ── 联系人节点 Contact 内存布局 (64-bit, 每个节点 0x6A8 字节) ──
// 64 位下 WxString 布局: +0x00 wchar_t* ptr (8B), +0x08 DWORD len (4B), +0x0C DWORD maxlen (4B)
#define ContactNodeSize           0x6A8
#define ContactWxidPtr            0x10
#define ContactWxidLen            0x18
#define ContactWxidMax            0x1C
#define ContactCustomAccountPtr   0x30
#define ContactCustomAccountLen   0x38
#define ContactCustomAccountMax   0x3C
#define ContactDelFlag            0x70
#define ContactType               0x74
#define ContactVerifyFlag         0x78
#define ContactRemarkPtr          0x80
#define ContactRemarkLen          0x88
#define ContactRemarkMax          0x8C
#define ContactEncryptNamePtr     0xA0
#define ContactEncryptNameLen     0xA8
#define ContactEncryptNameMax     0xAC
#define ContactPinyinAllPtr       0x168
#define ContactPinyinAllLen       0x170
#define ContactPinyinAllMax       0x174
