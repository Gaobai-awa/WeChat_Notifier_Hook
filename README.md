# WeChat Notifier — Hook 版（dev）

把原来的「轮询 CipherTalk」通知器重构为「接收 Hook 推送」的通知器。

- 旧版：轮询 CipherTalk 的 `/v1/messages`，需要 `db_path`、SQLCipher `key_hex`、`session_id`，靠 `createTime` 去重
- 新版：接收 `version.dll` hook 的回调推送，**不需要任何偏移值、不需要数据库 key**

本分支（`dev`）在 `main` 的基础上增加了**监听范围**：可以只监听配置里的用户，也可以监听所有人或所有私聊，不需要事先知道对方 wxid。

---

## 架构差异

| | 旧版 (C) | 新版 (Hook) |
|---|---|---|
| 消息来源 | 轮询 `127.0.0.1:5031/v1/messages` | Hook 主动 POST 到本机 |
| 依赖 | CipherTalk 客户端 + DB key + 偏移 | 只要 `version.dll` 在位 |
| 触发延迟 | 一个检查周期 | 实时 |
| 解密数据库 | 需要 | 不需要 |
| 去重键 | `session_id` + `createTime` | `msgid`（hook 提供的服务端 ID） |
| 断线恢复 | 重新连 API | 每轮重新 `set_callback` |

数据流：

```
Weixin.exe (version.dll 注入)
   │  收到好友消息
   └─> POST http://127.0.0.1:8899/recv
          │
          └─> WeChatNotifier.exe
                 ├─ 解析 flat JSON
                 ├─ type 白名单过滤
                 ├─ 公众号 / 服务号排除
                 ├─ 按 watch_mode 判定是否通知
                 ├─ msgid 去重
                 └─ WinRT Toast 通知 + 控制台输出
```

---

## 监听范围（本分支新增）

`watch_mode` 决定哪些消息会弹通知：

| 值 | 名称 | 行为 |
|---|---|---|
| `0` | 所有人 | 群聊 + 私聊全收，不需要配置任何 wxid。标题用会话 wxid / 群 id，群消息带发言人 |
| `1` | 仅私聊 | 只收 `roomid` 为空的私聊，群消息全部丢弃 |
| `2` | Config 内用户 | 只收 `users` 里列出的对象（`main` 分支的原有行为），默认值 |

托盘右键 →「监听范围」二级菜单可随时切换，切换后立即写回 `config.json`。

补充约定：

- 公众号（`gh_` 开头）和公众平台助手（`mphelper`）在任何模式下都不通知，否则一天几百条会刷屏
- 只有在 `2`（Config 内用户）模式下才要求 `users` 非空；`0` / `1` 模式即使 `users` 为空也能正常启动
- `0` / `1` 模式下，如果 `users` 里恰好有对应条目，标题会用配置的名字，否则退回 wxid

---

## 构建

```bat
build.bat
```

产物：`build\out\Release\WeChatNotifier.exe`

生成器固定为 **`Visual Studio 18 2026`**（VS 18 Community + SDK 10.0.26100）。换别的版本直接改 `build.bat` 里的生成器名即可：

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
mkdir build; cd build
& $cmake .. -G "Visual Studio 18 2026" -A x64
& $cmake --build . --config Release
```

依赖只有两个 header-only 库，已在 `src/`：`json.hpp`（nlohmann/json 3.11.3）、`httplib.h`（cpp-httplib 0.12.6）。

---

## 配置

程序按以下顺序找 `config.json`：

1. exe 同目录
2. `build\out\Release\..\..\..\config.json`（即仓库根目录，方便调试）
3. 都不存在就在 exe 同目录生成一份模板并退出，提示编辑后重启

```json
{
  "hook_api": "http://127.0.0.1:30001",
  "listen_port": 8899,
  "watch_mode": 2,
  "users": [
    { "wxid": "wxid_xxxxxxxxxxxx", "display_name": "好友昵称", "is_group": false }
  ],
  "notify_types": [1, 3, 34, 37, 42, 43, 47, 48, 49, 50, 62, 10000, 10002],
  "autostart": false,
  "silent_mode": false,
  "language": "zh_cn",
  "members": {}
}
```

| 字段 | 说明 |
|---|---|
| `hook_api` | Hook 的 HTTP 服务地址，默认 `http://127.0.0.1:30001` |
| `listen_port` | 本程序接收推送的端口，**必须**与 hook 回调地址一致 |
| `watch_mode` | `0`=所有人 `1`=仅私聊 `2`=Config 内用户 |
| `users[].wxid` | 私聊填对方 wxid；群聊填 `xxxxx@chatroom` |
| `users[].display_name` | 通知标题显示的名字 |
| `users[].is_group` | 留空时按 id 里有没有 `@chatroom` 自动判断 |
| `notify_types` | 要通知的 `type`，1=文本，49=XML卡片/链接/文件 |
| `autostart` | 开机自启（写 Startup 快捷方式） |
| `silent_mode` | 隐藏控制台窗口 |
| `language` | `zh_cn` / `zh_tw` / `en_us` |
| `members` | 群成员 `wxid → 昵称`，用于群消息显示发言人 |

括号内的 `//` 注释行会被自动剥离，模板里可以带注释。`wxid` 里带 `xxx` 的条目会被当作占位符跳过。

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
| `wxid` | 会话对方（私聊），不是自己 |
| `sender` | 发送者 wxid |
| `roomid` | 群 id；空字符串 = 私聊，非空 = 群聊（此时 `sender` 是发言人） |
| `type` | 1=文本，49=XML卡片，51=同步信令（噪声，必须过滤） |
| `msgid` | 服务端消息 ID，用于去重 |
| `content` | 文本内容或原始 XML |

程序也兼容旧式的 `msglist` 包裹格式（`msgtype`/`msg`/`fromid`/`toid`/`msgsvrid`），自动归一化后走同一条处理链。

---

## 消息类型处理

**除纯文本外，所有类型都带 `[tag中文]` 前缀**，一眼能看出这是图片还是链接，不会跟普通文字混淆。

### 主类型（推送里的 `type`）

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

前缀后面跟正文：`<title>` 加 `<des>`（描述与标题相同时不重复）。正文按 UTF-8 字符边界截断到 300 字。

### 关于白名单

`notify_types` 默认已包含上表所有类型，图片/语音/视频都会弹通知。早期默认只有 `[1, 49]`，所以图片视频一声不响。

---

## 可靠性设计

### callback 每轮重注册

`/set_callback` 的状态**只存在 hook 的进程内存里**，以下情况都会丢失：

- WeChat 重启
- 别的工具（Postman、测试脚本）调用了 `set_callback`
- hook 内部状态被清

所以 watchdog 每 15 秒 ping 一次 `/GetSelfProfile`，**每次 ping 成功都重新 `set_callback`**。成本可忽略，但免疫上面所有丢失场景。

### 离线快速恢复

hook 不可达时改成 5 秒一轮，WeChat 一启动就能立刻抢回。

### 端口占用检测

用 `bind_to_port()` 而不是 `listen()`——后者在端口被占时**静默阻塞**，程序看起来在跑但收不到任何推送。现在会弹窗报错并退出。

### 去重

`msgid` 环形缓冲保留最近 2000 个。`msgid=0` 的消息不参与去重（直接放行）。去重放在范围判定之后，被范围过滤掉的消息不占用缓冲。

### Toast 异步队列

推送处理跑在 httplib 的工作线程里。如果在那里同步等 PowerShell（每次最多 6 秒），工作线程会被占死，后续推送全部排队。

所以分成两段：

- `handle_push` 只做解析，然后 `enqueue_toast()` 入队，立刻返回
- 独立的 `toast_worker_loop()` 线程消费队列，串行拉起 PowerShell

队列上限 32 条，真刷屏时丢最老的，不会无限增长。

---

## 最容易踩的坑：`wxid` 字段的语义

**`wxid` 是「会话对方」，不是「自己」。** 这个字段名极具误导性。

真实推送样本：

```json
{"event_type":1001,"msgid":1000000000000000002,"type":1,"timestamp":1700000001,
 "wxid":"wxid_xxxxxxxxxxxx","sender":"wxid_xxxxxxxxxxxx","roomid":"","content":"吃了"}
```

这是「好友发来的消息」，`wxid` 和 `sender` **相同**，都指向对方。

| 场景 | `wxid` | `sender` | 关系 |
|---|---|---|---|
| 私聊 · 对方发来 | 对方 | 对方 | `sender == wxid` |
| 私聊 · 自己发出 | 对方 | 自己 | `sender != wxid` |
| 群聊 · 任意人 | 群内某会话 | 发言人 | 看 `roomid` 判群 |

早期版本把 `isSelf` 判定写反了，结果**每一条对方发来的消息都被当成自己发的丢掉了**——程序跑着、`health` 正常、`hook_online: true`、callback 也注册成功，但一条通知都不弹。这是最难查的一类 bug：所有链路看起来都是通的。

如果以后又出现「什么都不弹」，第一件事是打印原始 payload 看 `wxid` 和 `sender` 的实际值，别信字段名。

---

## 已知限制

- **自己发出的消息不产生推送。** hook 只推「收到」的消息，`/SendTextMsg` 发出后 callback 端没有任何数据。
- **`/GetSelfProfile` 返回的 wxid 会漂移。** 它返回的是当前聚焦会话的信息，不是稳定身份，初始化时可能返回垃圾值。不要用它判断自己是谁。
- **推送格式可能与 hook 上游源码不符。** 手上的是 `4.1.10.27` 编译版，推送为扁平结构；以上游仓库源码为准的结论未必成立，以实际抓包为准。
- **`/recv` 只硬性要求 `type` 字段。** 缺 `content` 的图片/语音推送也会进处理链，由 `format_content` 生成 `[image图片]` 这类占位符。
- **`0` / `1` 模式拿不到昵称。** 未配置的对象只能显示 wxid 或群 id，这是 hook 不提供通讯录接口导致的。

---

## 验证记录

| 场景 | 结果 |
|---|---|
| 普通文本 | ✅ `[好友昵称] 今晚吃什么呢` |
| 多行消息 | ✅ 空行压缩为单换行 |
| 单引号 `it's 'quoted'` | ✅ 不炸 PowerShell |
| 500 字长文 | ✅ 精确截到 300 字 |
| emoji + `<html> & $var` | ✅ 原样保留 |
| XML 链接卡片 | ✅ 提取 title + des |
| XML 文件卡片 | ✅ `report.pdf` + `2.4 MB` |
| type=51 信令 | ✅ 丢弃 |
| type=3 图片（不在白名单时） | ✅ 丢弃 |
| 非监听对象（`watch_mode=2`） | ✅ 丢弃 |
| 群聊已配置成员 | ✅ `[测试群 · 小明]` |
| 群聊未配置成员 | ✅ `[测试群 · wxid_unknown]` |
| 同 msgid 连发 6 次 | ✅ 只通知 1 次 |
| 空 content | ✅ 丢弃 |
| msgid=0 | ✅ 放行 |
| 非法 JSON | ✅ 返回 `{"code":0,"msg":"bad json"}` |
| 端口被占 | ✅ 弹窗报错退出 |
| callback 被外部改到 8900 | ✅ 15 秒内自动抢回 |
| `watch_mode=0` 收到未配置的私聊 | ✅ 正常通知，标题为 wxid |
| `watch_mode=0` 收到群消息 | ✅ 正常通知，标题 `群id · 发言人` |
| `watch_mode=1` 收到群消息 | ✅ 丢弃 |
| 公众号 `gh_xxx` 推送 | ✅ 丢弃 |
| 托盘切换监听范围 | ✅ 立即生效并写回配置 |

---

## 手动测试

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

健康检查：

```powershell
# 本程序
(Invoke-WebRequest http://127.0.0.1:8899/health -UseBasicParsing).Content

# hook 是否在线
(Invoke-WebRequest http://127.0.0.1:30001/GetSelfProfile -Method POST -Body "{}" -UseBasicParsing).Content
```

> PowerShell 的 `Invoke-WebRequest` 会把 UTF-8 响应按 Latin-1 解码，中文变乱码。用 `HttpWebRequest` + `StreamReader(..., [System.Text.Encoding]::UTF8)` 才准。

---

## 右键菜单

托盘图标右键：

- **状态** — `Hook 已连接` / `Hook 未连接`（灰显，不可点）
- **监听范围** — 所有人 / 仅私聊 / Config 内用户，三级子菜单，切换后立即写回配置
- **开机自启动** — 切换，立即创建/删除 Startup 快捷方式
- **静默运行** — 开启隐藏控制台；关闭则以可见窗口重启自己
- **退出** — 停服务、删托盘图标

---

## 文件

```
.
├─ src\
│  ├─ main.cpp        主体（~1030 行）
│  ├─ json.hpp        nlohmann/json
│  └─ httplib.h       cpp-httplib
├─ resource.rc        图标资源
├─ weixin.ico
├─ config.json        配置模板
├─ CMakeLists.txt
├─ build.bat
└─ README.md          本文件
```

> 不写任何日志文件。消息只输出到控制台（静默模式下控制台不可见）。排查问题时可以临时把 `dump_raw` 加回去。
