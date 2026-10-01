# 涟漪 RippleTrace — Web 版说明

---

## 1. 启动

```bat
cd RippleTrace
serve.bat
```

双击 `serve.bat` 即可一键运行：它会检测并自动构建 → 启动本地服务 → 等待健康检查通过 → 打开浏览器。
关闭启动窗口即可停止服务。

默认地址 `http://127.0.0.1:8080/`，沙箱根目录为 `samples`。

指定根目录与端口：

```bat
serve.bat samples\tinybank          :: 只允许分析这个目录
serve.bat samples 9000              :: 换端口
```

或直接调用：

```bat
build\RippleTrace.exe serve samples --port 8080
```

**页面打开后会立即自动载入演示仓库并分析 `sha256_hex`**，不需要输入任何东西就能看到完整结果。

---

## 1.1 ⚠️ 不要直接双击 `index.html`

这是最容易踩的一个坑，我们实际收到过这个报错：

```
Failed to parse URL from /api/scan?path=samples%2Ftinybank
```

**原因**：直接双击 `index.html` 打开时，页面协议是 `file://`。
这种文档**没有 origin**，因此浏览器无法解析 `/api/scan` 这类相对地址
（`file://` 下相对 URL 会被拒绝或错误地解析成 `file:///api/scan`）。

**本质**：页面需要通过 **HTTP** 由 `RippleTrace` 自己发给你，才能和它背后的分析引擎通信。

**正确做法**：

```bat
serve.bat
```

然后用它打印出的地址访问，例如 `http://127.0.0.1:8080/`。

**页面已内建检测**：如果检测到 `file://` 打开，会直接显示一段中文说明和一个可点的正确地址，
而不是把那句天书般的 `Failed to parse URL` 抛给使用者。

> **为什么不做跨域支持？**
> 我们**故意**不给 API 加 CORS 头。一个允许任意网页调用的本地 HTTP 服务，
> 意味着你访问的任何网站都能通过它读取沙箱内的源码树。为了少一次启动步骤而开这个口子，
> 代价不对等。所以选择"必须由服务器提供页面"这条更安全的路径。

---

## 2. 界面说明

### 2.1 顶部输入区

| 控件 | 作用 |
|---|---|
| 代码库目录 | 要分析的路径（沙箱根目录下） |
| 扫描仓库 | 构建调用图并显示统计 |
| 载入演示 | 一键回到演示仓库并自动分析 |

沙箱根目录显示在输入框下方——**不是装饰**，它是服务端实际允许访问的边界。

### 2.2 扫描结果

- **统计卡片**：函数数 / 已解析调用边 / 未解析调用点 / 测试函数 / 入口点 / 头文件内定义 / 歧义短名
- **影响半径排行**：按扇入排序。**点击任意一行即可直接分析该函数**——这是最快的探索路径

### 2.3 影响面图（核心）

SVG 分层图，**左侧是变更点，向右按调用深度逐层展开调用者**：

```
变更点  →  1 跳调用者  →  2 跳  →  3 跳  →  4 跳
```

- 节点颜色 = 风险等级（变更点红 / CRITICAL 深红 / HIGH 橙 / MEDIUM 青 / LOW 绿）
- 测试函数用深蓝底 + `TEST` 标记
- 节点副标题显示风险等级、是否为测试、扇入数
- **点击任意节点**：高亮它到变更点的调用链，并在图下方显示证据行

`sha256_hex` 的实际分层结果：

| 深度 | 节点数 |
|---|---|
| 0（变更点） | 1 |
| 1 跳 | 4 |
| 2 跳 | 2 |
| 3 跳 | 4 |
| 4 跳 | 3 |
| **合计受影响** | **13** |

### 2.4 四个结果标签页

| 标签页 | 内容 |
|---|---|
| 受影响函数 | 风险 / 分数（带进度条）/ 层级 / 函数与文件行号 / 调用链 / 扇入 / 风险成因 |
| 建议重跑的测试 | 按距变更跳数升序，跳数越小越优先 |
| 变更交互 | 多个变更函数之间是否存在依赖，能否分别验证 |
| 局限说明 | 未解析调用点等边界，并明确说明**结果是下界** |

---

## 3. HTTP API

`serve` 同时是一组 JSON 接口，可供 CI、IDE 插件或自建前端调用。

| 方法 | 路径 | 请求 | 返回 |
|---|---|---|---|
| GET | `/api/health` | — | `{"ok":true,"tool":"rippletrace","version":"0.1.0"}` |
| GET | `/api/scan?path=<dir>` | query | 统计 + 全部函数 + `topFanIn` 排行 |
| POST | `/api/impact` | `{"path":"...","fn":["a","b"]}` | 影响面报告 |
| POST | `/api/diff` | `{"path":"...","patch":"x.patch"}` 或 `{"patchText":"..."}` | 影响面报告 |

### 影响面报告结构

```json
{
  "ok": true,
  "requested": ["sha256_hex"],
  "unknown": [],
  "summary": {
    "changed": 1, "impacted": 13, "maxDepth": 4,
    "tests": 10, "conflicts": 0,
    "critical": 0, "high": 0, "medium": 6, "low": 7,
    "resolvedCallees": 107, "unresolvedCallees": 253
  },
  "layers": [
    { "depth": 0, "nodes": [ { "id": "...", "risk": "CHANGED", "score": 100 } ] },
    { "depth": 1, "nodes": [ { "id": "...", "risk": "MEDIUM", "score": 44, "fanIn": 1,
                               "isTest": true, "isPublicApi": false } ] }
  ],
  "impacted": [
    { "id": "...", "file": "...", "line": 32, "depth": 1, "fanIn": 1,
      "risk": "MEDIUM", "score": 44, "isTest": true, "isPublicApi": false,
      "reasons": ["direct caller of a changed function", "entry point"],
      "path": ["test_sha256_abc", "sha256_hex"] }
  ],
  "tests": [ { "id": "...", "file": "...", "distance": 1 } ],
  "conflicts": [ { "a": "...", "b": "...", "sharedCallers": 1, "note": "..." } ],
  "notes": ["253 call site(s) could not be resolved ..."]
}
```

`layers` 是**服务端按深度分好组**的节点，前端因此不需要自己算布局——
把布局语义放在服务端，前端只负责画。

### 调用示例

```bat
curl http://127.0.0.1:8080/api/health
curl "http://127.0.0.1:8080/api/scan?path=samples/tinybank"
curl -X POST http://127.0.0.1:8080/api/impact ^
     -H "Content-Type: application/json" ^
     -d "{\"path\":\"samples/tinybank\",\"fn\":[\"sha256_hex\"]}"
```

---

## 4. 安全设计：分析沙箱

**问题**：一个能读取任意路径的本地 HTTP 服务是危险的默认配置。
如果 `path` 参数不做限制，任何能访问该端口的人都可以遍历你的文件系统。

**做法**：

| 措施 | 说明 |
|---|---|
| 根目录沙箱 | 启动时确定一个根目录，请求路径必须落在其内部，越权返回 **403** |
| 路径规范化 | 先 `weakly_canonical` 再比较，防止 `..` 绕过 |
| 请求体上限 | 32 MB，超出即拒绝 |
| 收发超时 | 15 秒，避免异常客户端拖死服务 |
| 仅监听本机 | 默认绑定 `127.0.0.1`，不对外暴露 |

实测：

```
$ curl "http://127.0.0.1:8099/api/scan?path=C:/Windows"
HTTP 403  {"ok":false,"error":"path is outside the directory this server was started with: ..."}
```

---

## 5. 实现说明

| 部分 | 做法 | 理由 |
|---|---|---|
| HTTP 服务 | 原生 winsock，手写 HTTP/1.1 | 保持"零第三方依赖"这条性质 |
| 请求读取 | **按 `Content-Length` 读**，不是读到 EOF | 读到 EOF 会与客户端**死锁**（见下） |
| 并发模型 | 顺序处理 | 本地单用户工具，线程池只增加故障面 |
| 图缓存 | 缓存最近一次分析结果 | 同一仓库的连续交互不必重复解析 |
| 前端 | 单个自包含 HTML | 无构建步骤、无 CDN 依赖、可离线演示 |
| 布局语义 | 服务端算好 `layers` | 前端只负责画，逻辑集中在 C++ 侧 |

### 踩过的坑：读到 EOF 导致死锁

第一版 `recvAll()` 用循环 `recv` 直到返回 0 来判断请求结束。**这是错的**：
HTTP 客户端会保持连接打开、等待我们的响应，于是双方互等，请求全部超时。

改为标准做法：先读到 header 结束（`\r\n\r\n`），解析 `Content-Length`，
再精确读取 body。同时加上 `SO_RCVTIMEO` / `SO_SNDTIMEO`。

---

## 6. 无浏览器环境下的校验

开发环境可能没有可用浏览器，因此前端契约用 `tools/check_web.py` 做静态校验，
检查的都是**只会在浏览器 DevTools 里才暴露**的那类错误：

```bat
python tools\check_web.py      :: 需要服务器已在 8099 端口运行
```

| 检查项 | 说明 |
|---|---|
| HTML 标签平衡 | 未闭合 / 多余 / 不匹配的标签，重复 id |
| id 引用完整性 | JS 里每个 `$("id")` 都能在 HTML 中找到 |
| JS 语法 | `node --check` |
| 接口对齐 | 页面调用的每个 `/api/*` 服务端都实现了 |
| JSON 字段契约 | 页面读取的 16 个字段在真实响应中确实存在 |

实测输出：

```
  [ok]   HTML id 共 17 个，script 块 1 个
  [ok]   JS 中 17 个静态 id 引用全部存在
  [ok]   动态 tab-* id 校验通过
  [ok]   JS 语法检查通过 (node --check)
  [ok]   页面调用的 3 个接口服务端均已实现
  [ok]   16 个前端依赖的 JSON 字段全部存在
结论: 全部通过
```

**这套检查不能替代人眼确认视觉效果**，但能证明"页面不会因为契约错误而白屏"——
这是没有浏览器时能拿到的最强保证。

---

## 7. AI 解释层（网页版）

「AI 核查建议」标签页里有一个按钮：**让 AI 解释这次变更**。点它才会调用模型，不会自动触发。

- **API key 只在服务端读取**（`RIPPLETRACE_AI_KEY` 环境变量，或项目目录下的 `rippletrace.env` 文件），前端从不接触，接口也不接受浏览器传来的 key
- **就绪状态在点击之前就显示**：页面加载时会读 `/api/health` 的 `ai` 段，在 AI 标签页顶部直接显示"已就绪（模型 X，key 来自哪里，回答语言是什么）"或"未配置 + 该创建哪个文件"，而不是等用户点了按钮才回一个 503
- **未配置时**返回 503，提示里给出 `rippletrace.env` 的创建方法；服务**每次请求都会重新读取配置**，所以创建文件后直接再点一次按钮即可，无需重启

### 回答语言

模型写的三个文本字段（`summary` / `what_to_check` / `why_it_matters`）**默认用简体中文**。
页面上的两栏标签本来就是中文——「核查什么」和「为什么」——下面挂一段英文，
只是把翻译工作推给读者。函数名、文件路径、行号、代码标识符**在任何模式下都不翻译**，
因为把它们翻掉就等于让引用无法核对。

```
RIPPLETRACE_AI_LANG=zh     简体中文（默认）
RIPPLETRACE_AI_LANG=en     English
RIPPLETRACE_AI_LANG=auto   跟随提问语言
```

`zh-CN` / `zh_Hans` / `Chinese` / `en-US` / `English` 这类写法都能识别；
**无法识别的值回落到 `zh`**，不会因为一个拼写错误就丢掉整份回答。
命令行侧也可以用 `--ai-lang <code>` 单次覆盖。

AI 标签页顶部的就绪横幅会显示当前语言（`回答语言 简体中文`），
所以"为什么这次是英文/中文"在页面上就能看到答案，不必去翻配置。

### 配置文件优先于环境变量

```
优先级：环境变量 > rippletrace.env > .env
查找顺序：rippletrace.env, .env, ../rippletrace.env, ../.env
```

环境变量排在最前面，是为了让"临时换一个 key 跑一次"不必去改文件；
但配置文件才是推荐的常规做法——环境变量只在启动服务的那一个终端里有效，
换个窗口或用资源管理器双击 `serve.bat` 就丢了。

`rippletrace.env` 用 `rippletrace.env.example` 做模板：

```bat
copy rippletrace.env.example rippletrace.env
:: 然后编辑 rippletrace.env，至少填一行 RIPPLETRACE_AI_KEY=...
```

### 运输链：原生优先，必要时自动回退

AI 请求按以下顺序尝试，全部自动：

| 顺序 | 方式 | 说明 |
|---|---|---|
| 1 | WinHTTP 直连（IPv4 only） | 常规环境走这条 |
| 2 | WinHTTP 走系统代理 | 公司网络需要代理时 |
| 3 | **helper 子进程**（`tools/llm_transport.py`） | 前两条都失败时 |

第 3 条不是冗余设计。我们在开发机上遇到一个真实环境限制：

```
curl: (35) schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS
WinHTTP: 12185 ERROR_WINHTTP_UNRECOGNIZED_SCHEME
         （对任何 https 主机都失败，包括 www.microsoft.com）
```

**这台机器上 Windows 原生 TLS 栈不可用**，而 Python 自带的 OpenSSL 可以。
所以 helper 回退不是"以防万一"，而是**让工具在这种受限环境里也能工作**。

安全说明：key 通过**子进程的 stdin** 传递（放在 JSON 请求体里），
**不作为命令行参数**，因此不会出现在进程列表中。

可用 `RIPPLETRACE_AI_HELPER` / `RIPPLETRACE_AI_HELPER_SCRIPT` 覆盖解释器与脚本路径。

### 实测结果（真实 DeepSeek API）

| 项 | 结果 |
|---|---|
| 命令行调用 | ✅ 成功，8 条核查点，0 条被拒绝 |
| 网页版调用 | ✅ 成功，**耗时 9.9 秒** |
| prompt 大小 | 5 776 字符 |
| 模型输出质量 | 引用了具体 `file:line`，并正确指出"审计指纹变更会导致历史记录校验失效" |

真实模型输出示例（节选）：

> **audit_fingerprint** — 核查 `src/audit.cpp:9` 处喂给 `sha256_hex` 的确切字节与
> 期望的摘要格式；确认改动没有改变编码、长度或十六进制大小写。
> **为什么**：它是变更函数的直接调用者，也是摘要在审计链上变成指纹的唯一位置；
> 任何格式漂移都会传播到每一条审计记录。

这段文字是模型写的，但**每个事实（函数名、文件、行号、扇入、调用链）都来自静态分析**。

---

## 8. 排错

| 现象 | 原因 | 解决 |
|---|---|---|
| `Failed to parse URL from /api/scan?...` | 直接双击打开了 `index.html`（`file://`），页面没有 origin | 用 `serve.bat`，访问它打印的 `http://127.0.0.1:8080/` |
| 页面顶部显示"未通过服务器打开" | 同上，页面已自动检测并给出指引 | 同上 |
| `cannot bind 127.0.0.1:8080 (port already in use?)` | 端口被占用 | `serve.bat samples 9000` 换端口 |
| `no C/C++ sources found under ...` | 该目录下没有源码，或扩展名不在扫描列表 | 换目录；扩展名列表见 `repo.h` |
| `path is outside the directory this server was started with` | 沙箱限制，正常行为 | 用 `serve.bat <那个目录>` 把根目录指过去 |
| 浏览器没自动弹出 | `start` 在你的环境里没生效 | 手动访问终端打印的 URL |
| AI 标签页提示"AI 解释层未配置" | 服务端既没有 `RIPPLETRACE_AI_KEY` 环境变量，也没有 `rippletrace.env` 文件 | 按页面上的指引创建 `rippletrace.env`，然后**直接再点一次按钮**即可，不用重启（AI 尚未启用时会重读配置）；启动横幅里也会列出在找哪几个文件 |
| AI 报 `12185` 且 helper 也失败 | 原生 TLS 不可用，且没找到 helper 脚本 | 确认 `tools/llm_transport.py` 存在、`python` 在 PATH 上；可设 `RIPPLETRACE_AI_HELPER_SCRIPT` 指定路径 |
| `tools/check_web.py` 输出中文乱码 | Windows 控制台默认非 UTF-8 | 用 `python -X utf8 tools\check_web.py` |
