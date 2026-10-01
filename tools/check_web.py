# -*- coding: utf-8 -*-
"""
Validate web/index.html without a browser.

Checks that would otherwise only surface as a runtime error in DevTools:
  1. HTML tag balance and duplicate ids
  2. every $("id") / getElementById("id") reference has a matching id="..."
  3. JS syntax (node --check)
  4. every /api/... path the page calls is one the server actually serves
  5. every JSON field the page reads exists in a real API response

Run the server first, then:
    python tools/check_web.py [port]
"""
import os
import re
import sys
import json
import subprocess
import urllib.error
import urllib.request
from html.parser import HTMLParser

# The Windows console defaults to a legacy code page, which mangles the Chinese
# report text. Force UTF-8 on our own streams; the caller passes -X utf8.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
HTML = os.path.join(PROJECT, "web", "index.html")
WEBSRC = os.path.join(PROJECT, "src", "web.cpp")
PORT = sys.argv[1] if len(sys.argv) > 1 else "8099"
BASE = "http://127.0.0.1:%s" % PORT

TAB_IDS = ("impacted", "tests", "conflicts", "ai", "notes")

problems = []
notes = []

src = open(HTML, encoding="utf-8").read()

# ---------------------------------------------------------------- 1. tags
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr"}


class Checker(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.ids = []
        self.scripts = []
        self._in_script = False
        self._buf = []

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        if "id" in d:
            self.ids.append(d["id"])
        if tag == "script":
            self._in_script = True
            self._buf = []
        if tag not in VOID:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag == "script":
            self._in_script = False
            self.scripts.append("".join(self._buf))
        if tag in VOID:
            return
        if not self.stack:
            problems.append("多余的闭合标签 </%s>" % tag)
            return
        if self.stack[-1] != tag:
            problems.append("标签不匹配: 期望 </%s>，实际 </%s>" % (self.stack[-1], tag))
            if tag in self.stack:
                while self.stack and self.stack.pop() != tag:
                    pass
        else:
            self.stack.pop()

    def handle_data(self, data):
        if self._in_script:
            self._buf.append(data)


c = Checker()
c.feed(src)
if c.stack:
    problems.append("未闭合的标签: " + ", ".join(c.stack))

ids = set(c.ids)
dup = sorted({i for i in ids if c.ids.count(i) > 1})
if dup:
    problems.append("重复的 id: " + ", ".join(dup))
notes.append("HTML id 共 %d 个，script 块 %d 个" % (len(ids), len(c.scripts)))

js = "\n".join(c.scripts)

# ------------------------------------------------------- 2. id references
refs = set(re.findall(r'\$\("([A-Za-z0-9_\-]+)"\)', js))
refs |= set(re.findall(r'getElementById\("([A-Za-z0-9_\-]+)"\)', js))
missing = sorted(r for r in refs if r not in ids)
if missing:
    problems.append("JS 引用了不存在的 id: " + ", ".join(missing))
else:
    notes.append("JS 中 %d 个静态 id 引用全部存在" % len(refs))

if re.search(r'\$\("tab-" \+ t\)', js):
    for t in TAB_IDS:
        if "tab-%s" % t not in ids:
            problems.append("动态 id tab-%s 不存在" % t)
    notes.append("动态 tab-* id 校验通过（%d 个标签页）" % len(TAB_IDS))

# ------------------------------------------------------------ 3. JS syntax
jspath = os.path.join(PROJECT, "_webcheck.js")
with open(jspath, "w", encoding="utf-8") as f:
    f.write(js)
node = subprocess.run(["node", "--check", jspath], capture_output=True, text=True)
if node.returncode != 0:
    problems.append("JS 语法错误:\n" + (node.stderr or "").strip()[:900])
else:
    notes.append("JS 语法检查通过 (node --check)")
os.remove(jspath)

# --------------------------------------------------------- 4. API coverage
called = set(re.findall(r'"(/api/[a-z]+)', js))
websrc = open(WEBSRC, encoding="utf-8").read()
served = set(re.findall(r'path == "(/api/[a-z]+)"', websrc))
if "/api/impact" in websrc and "/api/diff" in websrc:
    served |= {"/api/impact", "/api/diff"}
unknown = sorted(p for p in called if p not in served)
if unknown:
    problems.append("页面调用了服务端未实现的接口: " + ", ".join(unknown))
else:
    notes.append("页面调用的 %d 个接口服务端均已实现: %s"
                 % (len(called), ", ".join(sorted(called))))
if "/api/explain" in served:
    notes.append("AI 解释接口 /api/explain 已实现并暴露")

# The AI tab used to learn that the layer was unconfigured only by failing:
# the user clicked and got a 503. Readiness is now shown up front, sourced from
# /api/health, so these two facts must stay true in the page.
if "renderAiStatus" in js and '"aiStatus"' in js and '"aiStatus"' in src:
    notes.append("AI 标签页在点击前显示就绪状态（renderAiStatus -> #aiStatus）")
else:
    problems.append("AI 就绪状态横幅缺失：renderAiStatus / #aiStatus 未同时出现")
if "rippletrace.env" in js:
    notes.append("AI 未配置时页面直接给出 rippletrace.env 创建指引")
else:
    problems.append("AI 未配置时页面没有给出 rippletrace.env 指引")
if "SERVER_INFO" in js and "ai" in js:
    notes.append("页面从 /api/health 读取 AI 状态")

# ------------------------------------------- 5. JSON field contract (live)


def get(path):
    with urllib.request.urlopen(BASE + path, timeout=25) as r:
        return json.loads(r.read().decode("utf-8"))


def post(path, payload):
    req = urllib.request.Request(
        BASE + path,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=25) as r:
        return json.loads(r.read().decode("utf-8"))


def has(obj, *keys):
    cur = obj
    for k in keys:
        if isinstance(cur, dict) and k in cur:
            cur = cur[k]
        elif isinstance(cur, list) and cur and isinstance(cur[0], dict) and k in cur[0]:
            cur = cur[0][k]
        else:
            return False
    return True


try:
    # Ask the server what it was started on rather than assuming: the sandbox
    # root is whatever directory the operator chose, and "samples/tinybank"
    # only exists when that choice was the project itself.
    health = get("/api/health")
    target = "samples/tinybank" if health.get("hasSamples") else "."

    # /api/health must report AI readiness (and where it came from) so the page
    # can explain an unconfigured server instead of only failing on click. It
    # must do so without ever echoing the key.
    health_fields = [
        ("health.sandboxRoot", health, ["sandboxRoot"]),
        ("health.hasSamples", health, ["hasSamples"]),
        ("health.ai.enabled", health, ["ai", "enabled"]),
        ("health.ai.model", health, ["ai", "model"]),
        ("health.ai.endpoint", health, ["ai", "endpoint"]),
        ("health.ai.keySource", health, ["ai", "keySource"]),
        ("health.ai.language", health, ["ai", "language"]),
        ("health.ai.helperAvailable", health, ["ai", "helperAvailable"]),
        ("health.ai.configCandidates", health, ["ai", "configCandidates"]),
    ]
    bad_health = [n for n, o, ks in health_fields if not has(o, *ks)]
    if bad_health:
        problems.append("/api/health 缺少字段: " + ", ".join(bad_health))
    else:
        notes.append("/api/health 上报 AI 就绪状态（enabled=%s, keySource=%s, language=%s）"
                     % (health["ai"].get("enabled"), health["ai"].get("keySource"),
                        health["ai"].get("language")))
        # The page renders this code through languageLabel(); an unknown value
        # would silently show the wrong language name in the banner.
        if health["ai"].get("language") not in ("zh", "en", "auto"):
            problems.append("/api/health 的 ai.language 不是 zh/en/auto: %r"
                            % (health["ai"].get("language"),))

    health_text = json.dumps(health, ensure_ascii=False)
    if "apikey" in health_text.lower() or "sk-" in health_text:
        problems.append("/api/health 泄漏了 API Key")
    else:
        notes.append("安全：/api/health 不含 API Key")

    scan = get("/api/scan?path=" + target)
    # Pick a real function from the scan so the impact request works on any repo.
    pick = None
    for f in scan.get("topFanIn", []):
        if not f.get("isTest"):
            pick = f["id"]
            break
    if pick is None and scan.get("functions"):
        pick = scan["functions"][0]["id"]
    imp = post("/api/impact", {"path": target, "fn": [pick]})

    expectations = [
        ("scan.stats.functions", scan, ["stats", "functions"]),
        ("scan.stats.unresolved", scan, ["stats", "unresolved"]),
        ("scan.topFanIn[].fanIn", scan, ["topFanIn", "fanIn"]),
        ("scan.functions[].isTest", scan, ["functions", "isTest"]),
        ("impact.summary.impacted", imp, ["summary", "impacted"]),
        ("impact.summary.maxDepth", imp, ["summary", "maxDepth"]),
        ("impact.summary.critical", imp, ["summary", "critical"]),
        ("impact.layers[].depth", imp, ["layers", "depth"]),
        ("impact.layers[].nodes[].risk", imp, ["layers", "nodes", "risk"]),
        ("impact.impacted[].score", imp, ["impacted", "score"]),
        ("impact.impacted[].reasons", imp, ["impacted", "reasons"]),
        ("impact.impacted[].path", imp, ["impacted", "path"]),
        # `tests` may legitimately be EMPTY (a repo with no tests), in which
        # case indexing into the first element is not a valid expectation.
        ("impact.tests", imp, ["tests"]),
        ("impact.impacted[].depth", imp, ["impacted", "depth"]),
        ("impact.impacted[].fanIn", imp, ["impacted", "fanIn"]),
        ("impact.impacted[].isTest", imp, ["impacted", "isTest"]),
        ("impact.impacted[].isPublicApi", imp, ["impacted", "isPublicApi"]),
        ("impact.summary.high", imp, ["summary", "high"]),
        ("impact.summary.low", imp, ["summary", "low"]),
        ("impact.summary.maxDepth", imp, ["summary", "maxDepth"]),
        ("impact.summary.resolvedCallees", imp, ["summary", "resolvedCallees"]),
        ("scan.stats.tests", scan, ["stats", "tests"]),
        ("scan.topFanIn[].isTest", scan, ["topFanIn", "isTest"]),
        ("impact.conflicts", imp, ["conflicts"]),
        ("impact.notes", imp, ["notes"]),
    ]
    bad = [n for n, o, ks in expectations if not has(o, *ks)]
    if bad:
        problems.append("页面读取的字段在响应中不存在: " + ", ".join(bad))
    else:
        notes.append("%d 个前端依赖的 JSON 字段全部存在" % len(expectations))

    # The AI tab must degrade gracefully when the layer is unconfigured, so a
    # 503 here is a PASS: it proves the error path is reachable and typed.
    try:
        ai = post("/api/explain", {})
        if has(ai, "ai"):
            notes.append("AI 接口返回结构正常（usedAi=%s, points=%d, rejected=%d）"
                         % (ai["ai"].get("usedAi"),
                            len(ai["ai"].get("points") or []),
                            ai["ai"].get("rejectedCount", 0)))
            if has(ai, "ai", "points") and has(ai, "ai", "rejectedClaims"):
                notes.append("AI 接口含 points 与 rejectedClaims（接地性在 UI 可见）")
            # The page labels the two prose fields 核查什么 / 为什么, so with the
            # default language the text under them has to be Chinese. Checking
            # for a CJK codepoint is enough to catch a model that ignored the
            # instruction and answered in English anyway.
            if health["ai"].get("language") == "zh":
                prose = " ".join(
                    [ai["ai"].get("summary") or ""] +
                    [(p.get("whatToCheck") or "") + (p.get("whyItMatters") or "")
                     for p in (ai["ai"].get("points") or [])])
                if not prose.strip():
                    notes.append("AI 未返回可读文本（本仓库无可解释内容），跳过语言检查")
                elif any("\u4e00" <= ch <= "\u9fff" for ch in prose):
                    notes.append("AI 回答为中文（核查什么 / 为什么 两栏）")
                else:
                    problems.append("ai.language=zh 但 AI 回答里没有中文字符")
        else:
            problems.append("AI 接口缺少 ai 段")
    except urllib.error.HTTPError as e:
        # Decode leniently: an error body may not be valid UTF-8, and failing
        # here would mask the real diagnosis.
        body = e.read().decode("utf-8", "replace")
        if e.code == 503 and "RIPPLETRACE_AI_KEY" in body:
            notes.append("AI 未配置时返回 503 并说明如何启用（预期行为）")
        else:
            problems.append("AI 接口异常: HTTP %s %s" % (e.code, body[:200]))
except Exception as e:
    problems.append("接口校验请求失败: %s (服务在端口 %s 上运行吗?)" % (e, PORT))

# ---------------------------------------------------------------- report
print("=" * 68)
print(" web/index.html 静态校验")
print("=" * 68)
for n in notes:
    print("  [ok]   " + n)
if problems:
    print()
    for p in problems:
        print("  [FAIL] " + p)
    print("\n结论: 发现 %d 个问题" % len(problems))
    sys.exit(1)
print("\n结论: 全部通过")
