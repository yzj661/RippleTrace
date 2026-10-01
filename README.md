# 涟漪 RippleTrace

**变更影响面分析器（C++ / Java / Python）** — 输入改了哪几行，输出谁会被影响、为什么、该重跑哪些测试。

```
涟漪 RippleTrace 0.1.0 (concept build) - change-impact analyzer for C++ / Java / Python
```

---

## 这是什么

市面上已经有很多"AI 读你的代码给建议"的工具。**涟漪 RippleTrace 不重复做那件事。**

大模型看不见你的项目结构——你问它"我改了这个函数会影响谁"，它给不出可靠答案，
你也无法验证。涟漪 RippleTrace 构建整个仓库的**函数级调用图**，在图上做反向可达性遍历，
为每一条"受影响"的结论给出**带文件行号的调用链证据**。

```
$ RippleTrace impact samples/tinybank --fn sha256_hex

  impacted functions : 17   (max depth reached 4)
  by risk            : 0 critical, 0 high, 10 medium, 7 low
  tests to re-run    : 11

Why (shortest call path change -> impact)
  [MEDIUM] AuditLog::record -> audit_fingerprint -> sha256_hex
  [MEDIUM] AdviceService::advise -> sha256_hex
  [LOW]    test_reconciliation_report -> build_reconciliation
           -> AuditLog::chain_digest -> sha256_hex
```

这条链不是猜测：每一跳都是调用图中已解析的真实边，**可以去源码里逐行核对**。

---

## 快速开始

**网页版**（最直观，双击即可）：

```bat
cd RippleTrace
serve.bat
```

会自动构建、打开浏览器，并在前台启动本地服务，默认地址 `http://127.0.0.1:8080/`。
页面打开后**自动载入演示仓库并分析 `sha256_hex`**，不用输入任何东西就能看到完整结果。

也可以指定沙箱根目录和端口：

```bat
serve.bat samples\tinybank          :: 只允许分析这个目录
serve.bat samples 9000              :: 换端口
```

> ⚠️ **不要直接双击 `web\index.html`**。`file://` 打开的页面没有 origin，
> 无法解析 `/api/...` 相对地址，会报 `Failed to parse URL`。
> 页面必须由 `RippleTrace` 自己通过 HTTP 提供——`serve.bat` 就是干这个的。
> （页面已内建检测，误开时会直接显示中文指引。）

**命令行版**：

```bat
cd RippleTrace
run.bat scan   samples\tinybank
run.bat impact samples\tinybank --fn sha256_hex
run.bat diff   samples\tinybank --diff samples\tinybank\changes.patch
run.bat tests  samples\tinybank --fn trim
```

`run.bat` 会在缺少 `build\RippleTrace.exe` 时自动构建，也可以直接调用 exe：

```bat
build.bat
build\RippleTrace.exe serve samples --port 8080
build\RippleTrace.exe impact samples\tinybank --fn sha256_hex
```

需要 MSVC（`build.bat` 会自动定位 `vcvars64.bat`；已用 VS 2026 / MSVC 14.51 验证）。
也可用 CMake：

```bat
cmake -B build-cmake -S . && cmake --build build-cmake --config Release
```

**零第三方依赖**——不需要任何包管理器。命令行是 `winsock` 原生 HTTP 服务，
前端是单个自包含 HTML 文件。

> ⚠️ **维护提示**：所有 `.bat` 文件必须保持**纯 ASCII**。
> `cmd.exe` 按 OEM 代码页（中文 Windows 上是 GBK）解析批处理文件，
> UTF-8 中文注释会被错误解码，断出的 token 可能被当作命令执行。
> 我们实际踩过这个坑：一条中文注释导致 `'細杩涘叆浜や簰鑿滃崟' 不是内部或外部命令`。
> 中文说明请写在本 README 或 `docs/` 里，不要放进 `.bat`。

---

## AI 层：让大模型解释，但不让它决定

这是本项目对"人工智能赋能"的落实方式——**不是套一个聊天框，而是明确分工**：

```
静态分析（确定性）              大模型（语义）
──────────────────              ──────────────
哪些函数受影响                   每个受影响点该核查什么
怎么连起来的（调用链）            用自然语言解释整片影响面
危险程度排序
```

**关键约束：模型不允许决定影响面。**

系统提示词硬性禁止增删影响集、禁止编造标识符；而 `enforceGrounding()`
会在代码层面**校验模型返回的每个函数 id**——不在计算出的影响集里的一律丢弃，
并在报告中说明丢弃原因，而不是展示给使用者。

启用方式（两种，任选其一，都只在服务端生效）：

```bat
:: 方式一（推荐）：配置文件。任何方式启动服务都能读到
copy rippletrace.env.example rippletrace.env
:: 然后编辑 rippletrace.env，填入 RIPPLETRACE_AI_KEY

:: 方式二：环境变量。只对当前这个终端窗口有效
set RIPPLETRACE_AI_KEY=sk-...
```

```bat
:: 先看看模型会收到什么（不调用、不需要 key）
build\RippleTrace.exe prompt samples\tinybank --fn sha256_hex

:: 启用 AI 解释
build\RippleTrace.exe impact samples\tinybank --fn sha256_hex --ai
```

也支持 `--ai-endpoint` / `--ai-model` 指向任意 OpenAI 兼容服务。

配置优先级：**环境变量 > `rippletrace.env` > `.env`**。
排在后面的只是兜底，所以临时换一个 key 只要在命令行里设一下环境变量即可，
不必去改文件。

**为什么要有配置文件**：环境变量只在"启动服务的那一个终端"里有效，换个窗口、
或者用资源管理器双击 `serve.bat`，配置就丢了。`rippletrace.env` 放在项目目录里，
任何启动方式都能读到；而且**服务每次请求都会重读它**，所以启动之后才发现没配
key 的用户，创建文件再点一次按钮就行，不需要重启服务。

**接地性验证**（可现场复现）：`tools\fake_llm_server.py` 是一个故意返回错误内容的
假模型端点，启动后让涟漪 RippleTrace 调用它，可以看到模型编造的影响点被拒绝：

```
grounding: impact set is authoritative; 2 point(s) kept, 2 rejected
  ! 'trim' - it exists in the repository but is NOT in the computed impact set,
    so the model was inferring impact
  ! 'totally_made_up_function' - no such function in the repository
```

**回答语言**：模型写的三个文本字段（`summary` / `what_to_check` / `why_it_matters`）
**默认用简体中文**——页面上"核查什么""为什么"两栏本来就是中文标签，
回答再是英文就只是给读者添一道翻译工序。函数名、文件路径、代码标识符
**在任何模式下都不翻译**，否则引用就无法核对。

```bat
set RIPPLETRACE_AI_LANG=en            REM 或写进 rippletrace.env
RippleTrace impact <repo> --fn <name> --ai --ai-lang en
```

`zh`（默认）· `en` · `auto`（跟随提问语言）三选一；写错的值**回落到默认**，
不会因为一个拼写错误就丢掉整份回答。

**离线可用**：没有 key 时核心功能完全不受影响，AI 层是可选增强。
`tools\test.bat` 提供 **179 项离线单元测试**覆盖 JSON 解析器、UTF-8 合法性、
`rippletrace.env` 解析、回答语言归一化与接地校验逻辑。

---

## 命令

| 命令 | 作用 |
|---|---|
| `serve <root>` | **启动 Web 界面**（浏览器可视化影响面图） |
| `scan <repo>` | 构建调用图，输出统计与"最危险的函数"排行 |
| `impact <repo> --fn <name>` | 单个/多个函数的变更影响面 |
| `diff <repo> --diff <patch>` | 从 unified diff 自动推导变更集 |
| `tests <repo> --fn <name>` | 只输出推荐重跑的测试清单 |
| `prompt <repo> --fn <name>` | 打印交给大模型的 grounded prompt（不调用） |

常用开关：`--repo` `--max-depth` `--max-rows` `--port` `--json` `--dot` `--test-list`
`--ai` `--no-ai` `--ai-key` `--ai-endpoint` `--ai-model` `--ai-lang` `--print-prompt`
`--no-color` `--quiet` `--allow-cross-language` `--include-non-build`

完整说明见 [`docs/功能清单.md`](docs/功能清单.md)。

### HTTP API

`serve` 同时暴露一组 JSON 接口，可供 CI 或自建前端调用：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 健康检查 |
| GET | `/api/scan?path=<dir>` | 扫描，返回统计 + 函数列表 + 扇入排行 |
| POST | `/api/impact` | 影响面，body `{"path":"...","fn":["a"]}` |
| POST | `/api/diff` | 补丁驱动，body `{"path":"...","patch":"x.patch"}` |
| POST | `/api/explain` | AI 核查建议，body `{"path":"..."}`（须先跑过一次 impact） |

**沙箱**：服务启动时指定根目录，所有分析请求只能指向该目录内部，越权返回 403。
一个能读任意路径的本地 HTTP 服务是危险的默认配置，这个限制让"演示给别人看"变得放心。

---

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/功能清单.md`](docs/功能清单.md) | **20 项功能逐条说明** + CLI / HTTP API 参考 + 已知边界 + 实测数据 |
| [`docs/技术选型与架构.md`](docs/技术选型与架构.md) | 架构分层、关键算法、技术栈选型理由、演进路线 |
| [`docs/演示脚本.md`](docs/演示脚本.md) | 7 分钟演示逐幕脚本，含预期输出与讲解词 |
| [`docs/Web版说明.md`](docs/Web版说明.md) | Web 界面与 HTTP API 的使用与设计说明 |
| [`docs/demo_output.txt`](docs/demo_output.txt) | 已捕获的完整演示输出（离线可复现） |

---

## 项目结构

```
RippleTrace/
├── build.bat / CMakeLists.txt      一键构建
├── run.bat                         CLI 交互菜单
├── serve.bat                       启动 Web 界面
├── src/
│   ├── tokenizer.*   词法分析（去注释/字符串/预处理）
│   ├── parser.*      函数与调用点提取（IRepoParser 接口）
│   ├── graph.*       调用图 + 作用域链名称解析
│   ├── impact.*      反向 BFS / 风险评分 / 测试选择 / 冲突检测
│   ├── diff.*        unified diff 解析 + 旧侧行号归属
│   ├── repo.*        文件发现与读取
│   ├── report.*      文本 / JSON / DOT 渲染
│   ├── web.*         原生 HTTP 服务 + JSON API + 沙箱
│   └── main.cpp      CLI
├── web/index.html                  单文件前端（分层影响面图）
├── samples/tinybank/               演示目标仓库（27 文件 / 71 函数 / 22 测试）
│   └── changes.patch               演示补丁（改底层 trim()）
├── tools/
│   ├── capture_demo.bat            重新生成演示输出
│   └── check_web.py                前端契约静态校验（无浏览器）
└── docs/                           四份交付文档
```

约 **7 700 行 C++**（29 个文件 / 15 个模块）+ **1 083 行**单文件 Web 前端 + **804 行**单元测试，
外加约 **1 900 行**的演示目标仓库（C++ / Java / Python 各一份）。

---

## 实测数据

| 指标 | 数值 |
|---|---|
| 扫描文件 / 函数 | 27 / 97 |
| 已解析调用边 | 164 |
| 未解析调用点 | 245（**主要是方法调用** —— 启发式解析无法确定接收者类型。其中 1192 个是裸标识符，`std::` 限定调用只有 187 个；**已在报告中披露**） |
| 识别测试函数 | 22（入口点也是 22） |
| `sha256_hex` 影响面 | 17 函数 / 4 层 / 11 个测试 |
| `trim` 补丁归属 | 精确 1 个变更函数 → 影响 18 函数 / 10 个测试 |
| 第三方真实项目 | 俄罗斯方块项目：19/19 函数定义、71/71 调用点，与 tree-sitter 对标 100% 一致 |

---

## 设计取舍（重要）

**这个工具把"诚实"当第一等公民。** 三个具体体现：

1. **宁可漏，不可错。** 名称解析只在确定时连边；歧义短名不解析，计入未解析并披露。
   错误的影响面比不完整的影响面更危险——前者会让你以为安全。

2. **不确定性量化。** 报告的 Limitations 段落会写明"245 个调用点未解析，
   结果是下界"，而不是假装图是完整的。

3. **风险评分可解释。** 每个分数都列出成因（扇入 / 距离 / 公有面 / 无测试覆盖），
   使用者能看到"为什么危险"，而不只是一个数字。

---

## 已知边界

| 限制 | 现状 | 生产化路径 |
|---|---|---|
| 启发式解析 | token + 花括号扫描，非完整 C++ 前端 | 实现 `IRepoParser` 接 libclang，**分析层零改动** |
| 非 C++ 语言 | Java / Python 走同一套启发式（花括号 / 缩进），不是各自语言的完整前端 | 每个语言各接一个 tree-sitter 语法，`IRepoParser` 接口不变 |
| 模板 / 重载 | 无法解析实例化与重载决议 | libclang AST + USR 作为节点键 |
| 虚函数 / 函数指针 | 动态派发路径不可见 | 保守传播 + "疑似虚调用"标记 |
| 跨 TU 宏 | 宏展开后的调用点不可见 | 预处理后再分析 |
| 非 UTF-8 源文件 | 按 UTF-8 → ANSI → 936 → 1252 顺序探测，全失败则替换为 U+FFFD | 读取项目 `.editorconfig` 的 charset 声明 |
| 大规模仓库 | 未做增量与并行 | 增量重解析 + 多线程 + 预编译头 |

**这些限制不影响核心价值主张**：分析层（图遍历、风险评分、调用链重建）是确定性的
且已验证正确，不确定性全部集中在解析层——而解析层是可替换的。

---

## 演进路线

| 版本 | 目标 |
|---|---|
| **v0.1（当前）** | **启发式解析 + 图分析 + 风险评分 + AI 解释层，支持 C++ / Java / Python** |
| v0.2 | 接入 libclang，用 USR 作为节点键 |
| v0.3 | 虚函数保守传播 |
| v0.4 | CI 集成（GitHub Action / Jenkins 插件，`--json` 驱动门禁） |
| v1.0 | tree-sitter 前端接入同一分析层，支持多语言 |

v0.5 是真正有价值的一步：**不是用大模型替代静态分析，而是让静态分析给大模型
提供它自己算不出来的结构事实**。结构归静态分析，语义归大模型——这才是这个方向正确的分工。
