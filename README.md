# WeChat_Notifier_Hook

基于 [Wechat_hook](https://github.com/aixed/WeChat-Hook) 与微信 [4.1.10.27](https://github.com/Rodert/wechat-win-versions/releases/download/v4.1.10.27/WeChatSetup-4.1.10.27.exe) 制作，让微信在收到消息时弹出**原生 Windows 通知**。

不需要解密数据库、不需要数据库密钥、不需要任何内存偏移：消息由 `version.dll` hook 主动 POST 到本程序，本程序过滤后调用 WinRT Toast 弹通知。

> 通知使用的 AUMID 就是 `WeChat`，所以在通知中心里显示为「微信」本身，和系统自带的微信通知混在一起，不会多出一个陌生的应用名。

---

## 功能

- 实时接收 hook 推送，无轮询延迟
- 私聊 / 群聊按 `wxid`、`xxxxx@chatroom` 精确监听
- 消息类型白名单（文本、图片、语音、视频、卡片、位置……）
- `msgid` 环形去重，同一消息重复推送只弹一次
- 卡片消息按 `<appmsg><type>` 细分：链接 / 文件 / 音乐 / 视频 / 小程序 / 引用回复 / 视频号
- 群消息标题带发言人昵称（`群名 · 昵称`）
- 托盘菜单：连接状态、开机自启、静默运行、退出
- 多语言：`zh_cn` / `zh_tw` / `en_us`
- 单文件 exe，静态链接 CRT，目标机不需要装 VC 运行库

---

## 环境要求

### 必要

- Windows 8 / 10 / 11（x64）
- 微信 4.1.10.27，并已安装 [Wechat_hook](https://github.com/aixed/WeChat-Hook) 的 `version.dll`
  - 装好后 hook 会监听 `http://127.0.0.1:30001`

### 可选

- 你要监听的人的 wxid（`wxid_` 开头的原始微信号）
  - 不填就监听不到任何人，程序启动时会提示「未配置监听对象」
  - 想监听群就填 `xxxxx@chatroom` 形式的群 id

---

## 快速开始

1. 下载发行版 `WeChatNotifier.exe`（或自行编译，见下文）
2. 放到任意目录，例如 `D:\WeChatNotifier\`
3. 首次运行会在 exe 同目录生成 `config.json` 模板，编辑它：

```json
{
    "hook_api": "http://127.0.0.1:30001",
    "listen_port": 8899,
    "users": [
        { "wxid": "wxid_xxxxxxxxxxxx", "display_name": "好友昵称", "is_group": false }
    ],
    "notify_types": [1, 3, 34, 37, 42, 43, 47, 48, 49, 50, 62, 10000, 10002],
    "members": {},
    "autostart": false,
    "silent_mode": false,
    "language": "zh_cn"
}
```

4. 再运行一次 `WeChatNotifier.exe`
5. 控制台出现 `Hook 已连接 -> http://127.0.0.1:8899/recv` 就说明通了

> `wxid` 里带 `xxx` 的条目会被当成占位符跳过，模板原样不动是不会误监听的。

### 无控制台启动

```bat
WeChatNotifier.exe -q
```

或把 `silent_mode` 设为 `true`。托盘右键「静默运行」也能开关（关闭时会以可见窗口重启自己）。

---

## 配置项

| 字段 | 说明 |
|---|---|
| `hook_api` | hook 的 HTTP 服务地址，默认 `http://127.0.0.1:30001` |
| `listen_port` | 本程序接收推送的端口，**必须**与 hook 回调地址一致 |
| `users[].wxid` | 私聊填对方 wxid；群聊填 `xxxxx@chatroom` |
| `users[].display_name` | 通知标题显示的名字 |
| `users[].is_group` | 留空时按 id 里有没有 `@chatroom` 自动判断 |
| `notify_types` | 要通知的 `type` 白名单，见下方类型表 |
| `members` | 群成员 `wxid → 昵称`，用于群消息显示发言人 |
| `autostart` | 开机自启（在「启动」目录创建快捷方式） |
| `silent_mode` | 隐藏控制台窗口 |
| `language` | `zh_cn` / `zh_tw` / `en_us` |

配置文件的查找顺序只有一处权威位置：**exe 同目录**。找不到时会尝试从仓库根目录（`build\out\Release` 往上三级）复制一份方便调试，否则生成模板并退出。

`config.json` 里允许写 `//` 注释行，读取时会自动剥离。

---

## 数据流

```
Weixin.exe (version.dll 注入)
   │  收到好友消息
   └─> POST http://127.0.0.1:8899/recv
          │
          └─> WeChatNotifier.exe
                 ├─ 解析扁平 JSON
                 ├─ type 白名单过滤（51 同步信令直接丢弃）
                 ├─ roomid / wxid 匹配监听目标
                 ├─ msgid 去重（最近 2000 条）
                 ├─ 生成 [tag] 前缀 + 正文
                 └─ 入队 → 独立线程拉起 PowerShell 弹 WinRT Toast
```

hook 的 `/set_callback` 状态只存在 hook 进程内存里，微信重启、别的工具抢占回调都会丢。所以 watchdog 每 15 秒 ping 一次 `/GetSelfProfile`，**每次 ping 成功都重新 `set_callback`**；hook 不可达时改成 5 秒一轮，微信一启动就能立刻抢回。

---

## Hook 推送格式

单条推送，**扁平结构**（不是 `msglist` 包裹）：

```json
{
  "event_type": 1001,
  "msgid": 1000000000000000001,
  "type": 1,
  "timestamp": 1700000000,
  "wxid": "wxid_xxxxxxxxxxxx",
  "sender": "wxid_xxxxxxxxxxxx",
  "roomid": "",
  "content": "嗯"
}
```

| 字段 | 含义 |
|---|---|
| `wxid` | 会话对方（私聊） |
| `sender` | 发送者 wxid |
| `roomid` | 群 id；空字符串 = 私聊，非空 = 群聊（此时 `sender` 是发言人） |
| `type` | 消息类型，见下表 |
| `msgid` | 服务端消息 ID，用于去重 |
| `content` | 文本内容或原始 XML |

`/recv` 只硬性要求 `type` 字段，缺 `content` 的图片/语音推送也会进处理链（生成 `[image图片]` 这类占位符），不会被整条丢掉。

也兼容旧式的 `msglist` 包裹格式（`msgtype`/`msg`/`fromid`/`toid`/`msgsvrid`），自动归一化后走同一条处理链。

---

## 消息类型与前缀

除纯文本外，所有类型都带 `[tag中文]` 前缀。

| type | 输出 |
|---|---|
| 1 | 原文，无前缀。`\r` 转空格，连续空行压成一个 |
| 3 | `[image图片]` |
| 34 | `[audio语音]` |
| 37 | `[friend申请加好友]` |
| 42 | `[contact名片]` |
| 43 | `[video视频]` |
| 47 | `[emoji表情]` |
| 48 | `[location位置]` |
| 49 | 按 appmsg 子类型细分，见下表 |
| 50 | `[call通话]` |
| 62 | `[video视频]`（视频的另一路类型） |
| 10000 | `[system系统消息]` |
| 10002 | `[system系统通知]` |
| 51 | 直接丢弃（同步信令，不是真消息） |
| 其他 | `[type=N]` |

### 卡片消息（type=49）按 `<appmsg><type>` 细分

| appmsg type | 输出前缀 |
|---|---|
| 5 | `[link链接]` |
| 6 | `[file文件]` |
| 3 | `[music音乐]` |
| 4 | `[video视频]` |
| 8 | `[emoji表情]` |
| 17 | `[location位置]` |
| 19 | `[chatrecord聊天记录]` |
| 33 / 36 | `[miniapp小程序]` |
| 57 | `[quote引用回复]` |
| 51 / 52 / 48 | `[channels视频号]` |
| 其他 | 有 `<appname>` 就用它，否则 `[卡片]` |

前缀后面跟正文：`<title>` 加 `<des>`（描述与标题相同时不重复）。正文按 UTF-8 字符边界截断到 300 字，不会切坏多字节字符。

---

## 如何使用

托盘图标右键：

- **状态** — `Hook 已连接` / `Hook 未连接`（灰显，不可点）
- **开机自启动** — 切换，立即创建/删除「启动」目录的快捷方式
- **静默运行** — 开启隐藏控制台；关闭则以可见窗口重启自己
- **退出** — 停服务、删托盘图标

健康检查：

```powershell
# 本程序
(Invoke-WebRequest http://127.0.0.1:8899/health -UseBasicParsing).Content

# hook 是否在线
(Invoke-WebRequest http://127.0.0.1:30001/GetSelfProfile -Method POST -Body "{}" -UseBasicParsing).Content
```

手动灌一条假消息测试：

```powershell
function Send($json) {
  $b=[System.Text.Encoding]::UTF8.GetBytes($json)
  $r=[System.Net.HttpWebRequest]::Create("http://127.0.0.1:8899/recv")
  $r.Method="POST";$r.ContentType="application/json; charset=utf-8"
  $r.ContentLength=$b.Length;$r.Proxy=$null
  $s=$r.GetRequestStream();$s.Write($b,0,$b.Length);$s.Close()
  $p=$r.GetResponse();$sr=New-Object System.IO.StreamReader($p.GetResponseStream(),[System.Text.Encoding]::UTF8)
  $t=$sr.ReadToEnd();$sr.Close();$p.Close();return $t
}

Send '{"type":1,"msgid":1,"sender":"wxid_xxxxxxxxxxxx","roomid":"","wxid":"wxid_xxxxxxxxxxxx","content":"测试"}'
```

---

## 容易踩的坑：`wxid` 字段的语义

**`wxid` 是「会话对方」，不是「自己」。** 这个字段名极具误导性。

| 场景 | `wxid` | `sender` | 关系 |
|---|---|---|---|
| 私聊 · 对方发来 | 对方 | 对方 | `sender == wxid` |
| 私聊 · 自己发出 | 对方 | 自己 | `sender != wxid` |
| 群聊 · 任意人 | 群内某会话 | 发言人 | 看 `roomid` 判群 |

早期版本把 `isSelf` 判定写反了，结果每一条对方发来的消息都被当成自己发的丢掉——程序跑着、`health` 正常、`hook_online: true`、callback 也注册成功，但一条通知都不弹。这是最难查的一类 bug：所有链路看起来都是通的。

如果又出现「什么都不弹」，第一件事是打印原始 payload，看 `wxid` 和 `sender` 的实际值，别信字段名。

`/GetSelfProfile` 返回的 wxid 会漂移（返回的是当前聚焦会话的信息，不是稳定身份），**不要用它判断自己是谁**。

---

## 编译

需要 Visual Studio 2022/2026（含 C++ 桌面开发）与 CMake：

```bat
build.bat
```

产物：`build\out\Release\WeChatNotifier.exe`

默认使用 `Visual Studio 18 2026` 生成器；换别的版本请直接改 `build.bat` 里的生成器名：

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
mkdir build; cd build
& $cmake .. -G "Visual Studio 18 2026" -A x64
& $cmake --build . --config Release
```

依赖只有两个 header-only 库，已放在 `src/` 里，无需联网：

- `src/json.hpp` — nlohmann/json 3.11.3
- `src/httplib.h` — cpp-httplib 0.12.6

链接 `windowsapp`（WinRT Toast）、`winhttp`、`ws2_32`、`shell32`、`ole32`、`user32`、`advapi32`。

---

## 文件结构

```
WeChat_Notifier_Hook/
├─ src/
│  ├─ main.cpp        主程序（~930 行）
│  ├─ json.hpp        nlohmann/json
│  └─ httplib.h       cpp-httplib
├─ resource.rc        图标资源
├─ weixin.ico
├─ config.json        配置模板
├─ CMakeLists.txt
├─ build.bat
└─ LICENSE
```

程序不写任何日志文件，消息只输出到控制台（静默模式下不可见）。

---

## 已知限制

- **自己发出的消息不产生推送。** hook 只推「收到」的消息，`/SendTextMsg` 发出后回调端没有数据。
- **推送格式可能与 hook 上游源码不符。** 手上的 dll 是 `4.1.10.27` 编译版，推送是扁平结构；以实际抓包为准。
- **端口被占会直接报错退出。** 用 `bind_to_port()` 而不是 `listen()`，后者在端口被占时会静默阻塞，程序看起来在跑但一条推送都收不到。

---

## 免责声明

仅供学习参考，请勿传播，请在下载后 24 小时内删除。

本项目依赖第三方 [Wechat_hook](https://github.com/aixed/WeChat-Hook)，使用前请自行确认是否符合当地法律法规与软件许可协议。因使用本程序产生的一切后果由使用者自行承担。

## License

[MIT](LICENSE)
