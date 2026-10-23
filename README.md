# WeChat_Notifier_Hook（dev 预览版）

基于 [Wechat_hook](https://github.com/aixed/WeChat-Hook) 与微信 [4.1.10.27](https://github.com/Rodert/wechat-win-versions/releases/download/v4.1.10.27/WeChatSetup-4.1.10.27.exe) 制作，让微信在收到消息时弹出**原生 Windows 通知**。

本分支是预览版，在稳定版基础上增加了：

- **监听范围**：可以只收私聊、收所有人，或者像原来一样只收指定的人——前两种不需要事先知道对方的 wxid
- **图片通知**：图片消息直接在通知里显示缩略图，不再只有一行 `[image图片]`

> 通知使用的 AUMID 就是 `WeChat`，所以在通知中心里显示为「微信」本身，和系统自带的微信通知混在一起，不会多出一个陌生的应用名。

---

## 功能

- 实时接收消息推送，无轮询延迟
- 三种监听范围：所有人 / 仅私聊 / 指定用户，托盘里可随时切换
- 图片消息在通知里直接显示缩略图
- 私聊、群聊都能通知，群消息标题带发言人昵称
- 消息类型白名单：文本、图片、语音、视频、卡片、位置、系统消息等
- 卡片消息按类型细分：链接 / 文件 / 音乐 / 小程序 / 引用回复 / 视频号
- 同一条消息只弹一次（按消息 ID 去重）
- 托盘菜单：连接状态、监听范围、开机自启、静默运行、退出
- 多语言：简体中文 / 繁体中文 / English
- 单文件 exe，静态链接 CRT，目标机不需要装 VC 运行库

---

## 环境要求

### 必要

- Windows 8 / 10 / 11（x64）
- 微信 4.1.10.27，并已安装 [Wechat_hook](https://github.com/aixed/WeChat-Hook) 的 `version.dll`
  - 装好后 hook 会监听 `http://127.0.0.1:30001`

### 可选

- 想用「Config 内用户」范围时，需要监听对象的 wxid（`wxid_` 开头的原始微信号；群聊是 `xxxxx@chatroom` 形式的群 id）
- 想在通知里看到图片，需要一个 `ffmpeg.exe`：放进程序目录、或写进配置项的 `ffmpeg_path`、或加进系统 PATH，三者有一个即可

---

## 快速开始

1. 下载 `WeChatNotifier.exe`，放到任意目录，例如 `D:\WeChatNotifier\`
2. 首次运行会在 exe 同目录生成 `config.json`，按需要选一种监听范围
3. 再运行一次，控制台出现 `Hook 已连接 -> http://127.0.0.1:8899/recv` 就说明通了

### 收所有消息

不知道对方 wxid 也能用，适合先确认链路是否正常：

```json
{
    "hook_api": "http://127.0.0.1:30001",
    "listen_port": 8899,
    "watch_mode": 0,
    "notify_types": [1, 3, 34, 37, 42, 43, 47, 48, 49, 50, 62, 10000, 10002]
}
```

### 只收私聊，不收群

```json
"watch_mode": 1
```

### 只收指定的人 / 群

```json
"watch_mode": 2,
"users": [
    { "wxid": "wxid_xxxxxxxxxxxx", "display_name": "好友昵称", "is_group": false }
]
```

`wxid` 里带 `xxx` 的条目会被当成占位符跳过，所以模板原样不动不会误监听。

### 无控制台启动

```bat
WeChatNotifier.exe -q
```

或把 `silent_mode` 设为 `true`。托盘右键「静默运行」也能开关（关闭时会以可见窗口重启自己）。

---

## 监听范围

| 值 | 名称 | 行为 |
|---|---|---|
| `0` | 所有人 | 群聊 + 私聊全收，不需要配置任何 wxid |
| `1` | 仅私聊 | 只收私聊，群消息全部丢弃 |
| `2` | Config 内用户 | 只收 `users` 里列出的对象，稳定版的原有行为 |

几点需要知道：

- 托盘右键 →「监听范围」可随时切换，切换后立即写回 `config.json`
- 公众号和服务号在任何模式下都不通知，避免刷屏
- 只有 `2` 模式要求 `users` 非空；`0` / `1` 模式即使不填也能正常启动
- `0` / `1` 模式下，没在配置里的对象标题会显示 wxid 本身——程序拿不到通讯录，昵称只能靠 `users` 或 `members` 补

---

## 配置项

| 字段 | 说明 |
|---|---|
| `hook_api` | Hook 的 HTTP 服务地址，默认 `http://127.0.0.1:30001` |
| `listen_port` | 本程序接收推送的端口，**必须**与 hook 回调地址一致 |
| `watch_mode` | `0`=所有人 `1`=仅私聊 `2`=Config 内用户 |
| `users[].wxid` | 私聊填对方 wxid；群聊填 `xxxxx@chatroom` |
| `users[].display_name` | 通知标题显示的名字 |
| `users[].is_group` | 留空时按 id 里有没有 `@chatroom` 自动判断 |
| `notify_types` | 要通知的消息类型，见下方类型表 |
| `image_preview` | 图片消息是否在通知里显示缩略图，默认 `true` |
| `ffmpeg_path` | `ffmpeg.exe` 的完整路径，图片通知用；留空则自动找程序目录 / `bin\` / PATH |
| `debug_payload` | 把收到的原始推送写进程序目录的 `payload.log`，排查问题用，默认 `false` |
| `wx_data_root` | 微信数据目录，留空自动探测（一般不用填） |
| `members` | 群成员 `wxid → 昵称`，用于群消息显示发言人 |
| `autostart` | 开机自启（在「启动」目录创建快捷方式） |
| `silent_mode` | 隐藏控制台窗口 |
| `language` | `zh_cn` / `zh_tw` / `en_us` |

`config.json` 里允许写 `//` 注释行，读取时会自动剥离。

---

## 通知内容长什么样

除纯文本外，消息都带一个中文方括号前缀，一眼能看出这是图片还是链接，不会跟普通文字混淆。

| 类型 | 通知正文 |
|---|---|
| 文字 | 原文，不带前缀。多行会保留换行，连续空行合并 |
| 图片 | 缩略图 + `[image图片]`，实在取不到图时只留这行前缀 |
| 语音 | `[audio语音]` |
| 视频 | `[video视频]` |
| 表情 | `[emoji表情]` |
| 位置 | `[location位置]` |
| 通话 | `[call通话]` |
| 好友申请 | `[friend申请加好友]` |
| 名片 | `[contact名片]` |
| 系统消息 | `[system系统消息]` |
| 链接卡片 | `[link链接] 标题 + 描述` |
| 文件 | `[file文件] 文件名 + 大小` |
| 音乐 | `[music音乐]` |
| 小程序 | `[miniapp小程序]` |
| 引用回复 | `[quote引用回复]` |
| 聊天记录 | `[chatrecord聊天记录]` |
| 视频号 | `[channels视频号]` |
| 其他卡片 | `[卡片]` 或按卡片的 app 名显示 |

正文太长时会截断到 300 字，不会把中文或 emoji 切成半个。

---

## 图片通知

图片消息会带上缩略图，通知长这样：

```
┌────────────────────────┐
│ 好友昵称                │
│ [image图片]             │
│ ┌────────────────────┐ │
│ │      缩略图         │ │
│ └────────────────────┘ │
└────────────────────────┘
```

怎么拿到图的：

微信 4.x 的图片存在 `msg\attach\<会话>\<月份>\Img\*.dat`，是加密的，而且**每个用户的密钥都不一样**，所以程序里没有写死任何密钥。取图顺序是：

1. 读微信**自己已经解好**的预览缓存：`cache\<月份>\Message\<会话>\ImageTemp\<序号>_<时间>_mid_temp_convert`
2. 这份缓存是 `wxgf` 容器包着的 HEVC 帧，程序剥掉头，交给 `ffmpeg` 取一帧存成 JPEG
3. 都没有时再调 hook 的 `/Decode_Pic` 试一次
4. 仍然失败就退回纯文字通知，不会卡住后面排队的消息

认图不靠猜：微信给每条图片消息生成的预览文件，文件名里的秒数就等于消息时间戳，文件大小又正好等于推送正文里的 `hevc_mid_size`。两个特征一叠加就能唯一定位到这条消息自己的图，不会套上一条的。

几点需要知道：

- **不会碰微信进程**：不解密数据库、不读内存、不重启微信，只读微信自己的缓存文件，微信可以一直开着
- 图片是微信预览缓存的分辨率（通常三五百像素），不是原图
- 取图比文字慢一点：消息到达后约 0.5～2 秒才弹通知（在等缓存落盘），文字消息没有这个延迟
- 临时图放在 `%temp%\WxNotifier\`，2 小时后自动清理；这个目录本身也在系统临时目录里，随时可以整个删掉
- 微信数据目录默认自动探测，装在非系统盘也不用管；探测失败时启动横幅会提示，手动填 `wx_data_root` 即可
- 不想要图片：`"image_preview": false`

---

## 托盘菜单

托盘图标右键：

- **状态** — `Hook 已连接` / `Hook 未连接`（灰显，不可点）
- **监听范围** — 所有人 / 仅私聊 / Config 内用户
- **开机自启动** — 切换，立即创建或删除启动项
- **静默运行** — 开启隐藏控制台；关闭则以可见窗口重启自己
- **退出** — 停服务、删托盘图标

---

## 常见问题

**一条通知都不弹**

1. 检查程序是否在跑：`(Invoke-WebRequest http://127.0.0.1:8899/health -UseBasicParsing).Content`，`hook_online` 应为 `true`
2. 检查 hook 是否在线：`(Invoke-WebRequest http://127.0.0.1:30001/GetSelfProfile -Method POST -Body "{}" -UseBasicParsing).Content`
3. 检查 `watch_mode` 是否和你的预期一致；`2` 模式下 `users` 必须填对
4. 检查 `notify_types` 里有没有这个类型

**自己发出的消息没有通知**

hook 只推送「收到」的消息，自己发出去的消息不会产生回调，这是 hook 侧的行为。

**标题显示的是 wxid 而不是昵称**

程序不读通讯录，昵称只来自 `users[].display_name` 和 `members`。没配置的对象只能显示 wxid。

**图片消息没有图，只有 `[image图片]`**

按顺序查：

1. 看启动横幅有没有 `ffmpeg: <路径>`。没有的话把 `ffmpeg.exe` 放进程序目录，或在 `ffmpeg_path` 里写绝对路径——缺 ffmpeg 时图片一律转不出来
2. 看横幅的「图片通知: 开 (N)」，N 是探测到的微信账号数，0 说明没找到数据目录，手动填 `wx_data_root`
3. 图片刚发过来的瞬间缓存可能还没落盘，程序会重试几次，仍失败就退化成纯文字
4. 确认 `image_preview` 没被关掉
5. 想看得更细：把 `debug_payload` 设为 `true`，程序会把收到的原始推送写进 `payload.log`

**连着收几张图，会不会张冠李戴**

不会。每条图片消息的预览文件靠「文件名秒数 == 消息时间戳」+「文件大小 == 推送里的 `hevc_mid_size`」双重确认，同一份文件也不会重复给两条消息用。真要碰上两张一模一样的图被连发，微信会各自生成一份预览，仍然是分开的。

**提示端口被占用**

`listen_port` 被别的程序占了（常见是旧的接收脚本）。关掉占用者，或改成别的端口，同时保证 hook 回调地址一致。

**PowerShell 里看接口返回的中文是乱码**

`Invoke-WebRequest` 会把 UTF-8 按 Latin-1 解码，换用 `HttpWebRequest` + `StreamReader(..., [System.Text.Encoding]::UTF8)` 就正常。

---

## 编译

需要 Visual Studio（含 C++ 桌面开发）与 CMake：

```bat
build.bat
```

产物：`build\out\Release\WeChatNotifier.exe`

生成器固定为 `Visual Studio 18 2026`，换别的版本直接改 `build.bat` 里的生成器名即可：

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
mkdir build; cd build
& $cmake .. -G "Visual Studio 18 2026" -A x64
& $cmake --build . --config Release
```

依赖只有两个 header-only 库，已放在 `src/` 里，无需联网：

- `src/json.hpp` — nlohmann/json 3.11.3
- `src/httplib.h` — cpp-httplib 0.12.6

---

## 文件结构

```
WeChat_Notifier_Hook/
├─ src/
│  ├─ main.cpp        主程序
│  ├─ json.hpp        nlohmann/json
│  └─ httplib.h       cpp-httplib
├─ resource.rc        图标资源
├─ weixin.ico
├─ config.json        配置模板
├─ CMakeLists.txt
├─ build.bat
└─ LICENSE
```

默认不写任何日志文件，消息只输出到控制台（静默模式下不可见）。只有把 `debug_payload` 设为 `true` 时才会在程序目录生成 `payload.log`，排查完记得关掉。

---

## 免责声明

仅供学习参考，请勿传播，请在下载后 24 小时内删除。

本项目依赖第三方 [Wechat_hook](https://github.com/aixed/WeChat-Hook)，使用前请自行确认是否符合当地法律法规与软件许可协议。因使用本程序产生的一切后果由使用者自行承担。

## License

[MIT](LICENSE)
