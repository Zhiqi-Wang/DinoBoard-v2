# Vibe Coding 入门 — DinoBoard-v2

> 给用 AI 辅助开发本项目的新人（及他们使用的 AI 助手）的一页引导。
> 目标：第一次接触本仓库就能避开大多数踩坑点。

---

## 你要开发什么？

先回答自己：你属于哪一类？

| 想做的事 | 属于 |
|---------|------|
| 加一个新游戏 | **游戏开发** — 你不需要懂 C++ MCTS/DAG 细节，只需要实现 `IGameState` / `IGameRules` / `IFeatureEncoder`（以及若有隐藏信息再加 tracker/events），然后照着 TicTacToe/Splendor 之一抄模板 |
| 改 MCTS / 训练 pipeline / ONNX 推理 / Web AI 后端 | **框架开发** — 你需要通读 `CLAUDE.md` 和 `docs/MCTS_ALGORITHM.md` 才能动手 |
| 改 Web 前端（HTML/CSS/JS） | **前端开发** — 读 `docs/WEB_DESIGN_PRINCIPLES.md` 和 `platform/static/general/app.js` |
| 修 bug | 先读 `docs/KNOWN_ISSUES.md` 看这个 bug 是不是已经修过 / 是已知设计取舍 |

---

## 让 AI 先读这些文件（按顺序）

### 必读（所有开发者，~15 分钟）

1. **`CLAUDE.md`** — 项目长期遵守的设计原则。**违反这些原则的 PR 会被拒**。重点：
   - Training 必须 C++ 跑，Python 只做网络训练
   - 禁止任何 "silent fallback" `catch(...) { return false; }` 或 `dict.get(k, default)` 掩盖错误
   - Tracker / encoder 接口结构上不能读 opp private
   - Value head 必须 N 维 perspective-relative
   - Web 后端禁止拿全局锁做重计算，pybind11 必须 `gil_scoped_release`

2. **`docs/GAME_FEATURES_OVERVIEW.md`** — 框架提供什么能力的高层地图。读过后你知道"启发式引导、tail solver、belief tracker、auxiliary scorer"各是什么、什么时候注册

3. **`docs/KNOWN_ISSUES.md`** — 已修 bug 和设计取舍合集。分「框架层」和「游戏层」两组
   - 动框架代码之前读「框架层」所有条目
   - 加新游戏之前读「游戏层」的 BUG-017（Splendor tracker 偷看）和 BUG-023（Love Letter Guard），这两个是游戏开发最常见的坑

### 按需读

- **加新游戏**：`docs/GAME_DEVELOPMENT_GUIDE.md`（逐字段接口文档，很长，**搜索你需要的小节**而不是通读）+ `docs/NEW_GAME_TEST_GUIDE.md`（9 步验收清单）
- **改 MCTS**：`docs/MCTS_ALGORITHM.md`（ISMCTS-v2 七个互锁属性，DAG+UCT2 数学动机）
- **改前端**：`docs/WEB_DESIGN_PRINCIPLES.md`
- **看最近在干什么**：`docs/devlog/2026-05-*.md`（日期倒序，最新的最相关）

---

## 开发时要注意的硬规则

这些都是血泪教训，违反一条就会浪费几小时调试。

### 0. 拿到任务后的第一个动作：确认基线是绿的

```bash
python3 setup.py build_ext --inplace 2>&1 | tail -20
pytest tests/ -q 2>&1 | tail -5
```

**MUST** 在构建和测试基线都绿的情况下才开始写新代码。如果基线已经红，先定位是你拉下来的代码问题还是环境问题，**NEVER** 在红状态下改代码——你会分不清哪些错是你引入的。

### 1. ONNX 模型必须和 encoder feature_dim 一致

如果你改了某游戏的 encoder（加/减字段），**MUST** 同步更新以下：
- `games/<name>/config/game.json` 的 `feature_dim`
- 重新训练并部署模型到 `games/<name>/model/<variant>.onnx`
- 回归测试 `tests/test_deployed_models_match_encoder.py` 会卡住——**NEVER** skip 它

**教训**：TicTacToe 曾经 encoder 加了 `first_player` 字段没同步 ONNX，UI 显示「AI 思考中」永远不落子。

### 2. Tracker / encoder 接口的"不能读"是硬约束不是建议

- `IBeliefTracker` 的 `init` 和 `observe_public_event` **接口签名没有 `IGameState*`** — **NEVER** 加进去
- `IFeatureEncoder::encode_public` **MUST NOT** 读任何玩家 private
- `IFeatureEncoder::encode_private(p)` 只能读 p 自己的 private，**NEVER** 读其他玩家

**编译期有部分强制**（前者），**运行期结构有测试强制**（后者，`test_encoder_respects_hash_scope`）。

### 3. 配置 key 用 `cfg[key]` 不用 `cfg.get(key, default)`

CLAUDE.md 明令禁止的「silent fallback」。如果 key 可能缺失，用 `.get()` 但 **MUST** 对值是否合法做明确检查和报错，**NEVER** 让默认值静默生效。

### 4. 新游戏接入时的必做清单

**构建系统接线（最容易漏的一步）**：

- `setup.py` 的 `sources` 列表 **MUST** 加入该游戏的所有 `.cpp`（至少 `<game>_state.cpp`、`<game>_rules.cpp`、`<game>_net_adapter.cpp`、`<game>_register.cpp`）。**漏加不会报错**——`pip install -e .` 会过，`import dinoboard_engine` 会过，但游戏根本注册不上（register 代码没被编译进去），`available_games()` 里看不到它。这是本项目最常见的沉默失败。
- `engine/CMakeLists.txt` 如果使用 CMake 构建也要同步。

**注册与配置**：

- `tests/conftest.py::CANONICAL_GAMES` 加 game_id
- 多人变体（`{game}_3p` / `{game}_4p`）各需一份独立的部署模型
- `game.json` 的 `feature_dim` / `action_space` **MUST** 和 `engine.game_metadata(game_id)` 返回的一致（以 C++ 为准，不一致就改 JSON）

**验收**：

- `pytest tests/ -q` 全过才算合格。**NEVER** 用 `pytest.skip` / `pytest.xfail` 放过红的测试——真修或者真删测试

### 5. 改完代码记得

```bash
python3 setup.py build_ext --inplace   # 重建 C++ 扩展
pytest tests/ -q                        # 跑全套测试
```

### 6. 文档的"维护义务"

- 加/改功能 → 更新 `docs/GAME_FEATURES_OVERVIEW.md`
- 修 bug → 在 `docs/KNOWN_ISSUES.md` 加一条（模板抄现有条目）
- 每天做完活 → 写 `docs/devlog/YYYY-MM-DD.md`（简短即可）

---

## 让 AI 工作时的最佳模式

### 命令 AI 前先让它读这些

> "先读 CLAUDE.md 了解设计原则。然后读 docs/GAME_FEATURES_OVERVIEW.md 了解框架能力。如果你要动 MCTS/搜索代码，再读 docs/MCTS_ALGORITHM.md。不要凭空设计，先找现有模式（游戏 TicTacToe/Quoridor/Splendor/LoveLetter/Coup 哪个和你要做的最像）。"

### 让 AI 写新游戏时的模板命令

> "我要加一个游戏 `<X>`，机制是 `<Y>`。请严格按以下顺序：
>
> **Step 1 — 获取规则**
>    - 优先读 `games/<X>/` 目录下用户放置的规则书/参考资料（PDF、Markdown、txt、图片都可以）——如果目录下已经有这类文件，以它为准
>    - 如果目录为空或资料不全，用 WebSearch / WebFetch 上网搜索该游戏的官方规则（中英文版本都可以，优先官方/BGG/维基百科来源）
>    - 读完规则后，**MUST** 先用 3-5 句话向我复述你理解的游戏核心机制（玩家数、回合结构、胜负条件、是否含隐藏信息/随机性），让我确认你没理解错再动手
>
> **Step 2 — 选模板 + 先搭骨架**
>    - 选一个最像的现有游戏作为模板（隐藏信息 → Love Letter/Coup/Splendor；纯确定 → Quoridor；最小 → TicTacToe）
>    - **先搭骨架再填实体**：写 `<game>_state.cpp`（空结构）+ `<game>_rules.cpp`（stub：legal_actions 返回 [0]、do_action 空实现）+ `<game>_net_adapter.cpp` + `<game>_register.cpp` + `config/game.json`
>    - 把所有 `.cpp` 加到 `setup.py` 的 `sources` 列表
>    - 跑 `python3 setup.py build_ext --inplace`，确认编译通过、`dinoboard_engine.available_games()` 看得到新游戏
>    - **这一步通过之后再填真实规则逻辑**。不要一次写 2000 行才第一次编译
>
> **Step 3 — 填实体 + 对齐测试**
>    - 照着 `docs/GAME_DEVELOPMENT_GUIDE.md` 的 checklist 实现 state、rules、encoder 的真实逻辑
>    - 把 game_id 加入 `tests/conftest.py::CANONICAL_GAMES`
>    - 小步迭代：写一点 → 跑相关测试 → 再写一点
>
> **Step 4 — 全量验收**
>    - `pytest tests/ -q` 全过
>    - 隐藏信息游戏额外确认 `test_api_belief_matches_selfplay[<X>]` 绿
>
> **Step 5 — 文档落地**
>    - 写 `docs/devlog/<today>.md` 记录关键决策（encoder 维度选择、belief tracker 设计等）
>    - 如果踩了新的坑，加到 `docs/KNOWN_ISSUES.md`"

**规则来源优先级**（AI 助手必须按此顺序尝试）：

| 优先级 | 来源 | 适用情况 |
|------|------|---------|
| 1 | `games/<game>/` 目录下用户放置的规则书文件 | 用户已准备材料；最权威，以用户版本为准 |
| 2 | WebFetch 已知官方规则 URL（如游戏公司官网的 PDF） | 用户只给了游戏名，目录为空 |
| 3 | WebSearch "<game name> rules" / "<游戏名> 规则" | 没有已知官方链接；**注意交叉验证多个来源**，桌游规则在爱好者网站上常有错漏 |
| 4 | 让用户补充 | 前三项都找不到可靠规则；**永远不要凭游戏名猜规则** |

**AI 助手的硬约束**：在没有可验证的规则来源之前，**不许开始写 `<game>_rules.cpp`**。凭印象写规则是本项目最严重的失败模式——rules 错了整个训练全白费。

### 让 AI 改框架代码时的红线

> "你要改的是 `engine/` 或 `bindings/` 下的代码。先读 `CLAUDE.md` 的 'No Fallbacks, No Silent Degradation' 一节。**任何以下改动需要先和我确认**：
> - 改 `IBeliefTracker` / `IFeatureEncoder` / `IGameState` 接口
> - 改 MCTS / ONNX evaluator 的核心流程
> - 删除某个看起来"没用"的函数（可能在 Python 侧被 pybind11 binding 调用）
> - 跳过 `catch` 捕获异常而 return false"

### AI 容易犯的错（实测高频）

- **过度设计**：加一堆未来可能用的抽象。本项目原则是「三个相似的地方比一个过早抽象好」
- **Python 重实现 C++ 逻辑**：NEVER，哪怕「只是为了测试」也不行
- **用 `.get(k, default)` 读配置**：NEVER，会掩盖 bug
- **跳过失败的 assertion**：`pytest.skip(...)` / `pytest.xfail` 作为放过失败测试的手段是作弊，MUST 真修或真删
- **忘记加 .cpp 到 setup.py sources**：这是最隐蔽的坑——编译过、import 过、但 `available_games()` 里看不到新游戏
- **凭游戏名猜规则**：NEVER——必须读规则书或上网查，写错一条规则整个训练都白费
- **一次写几百行再编译**：MUST 先搭 stub 骨架，确认能编译 + 能注册，再填实体逻辑

---

## 项目当前状态速览

- **6 个完整游戏**：tictactoe / quoridor / splendor / azul / loveletter / coup
- **大量参数化测试**，所有 canonical game 自动覆盖，0 skip 0 fail
- **ISMCTS-v2 重构**完成（2026-05-04）：DAG + UCT2 + 结构化 encoder 拆分
- **Coup 启发式 belief tracker**（手写加权联合采样）作为"诈唬游戏怎么做"的范例
- **待做**：概率化 belief network（framework 层增强，不急）、训练端 GPU 支持（当前纯 CPU）

---

## 提问时给 AI 的元 prompt

复制粘贴以下一段作为你和 AI 第一次对话的 system message / first message：

```
你正在开发 DinoBoard-v2（/Users/chihchi/claude/DinoBoard-v2）— 一个 AlphaZero 风格的
棋牌游戏框架。开始任何工作之前：

1. 阅读 CLAUDE.md（必读，是项目硬约束）
2. 阅读 docs/VIBE_CODING_ONBOARDING.md（本文，包含所有硬规则 + 每种任务的流程）
3. 阅读 docs/GAME_FEATURES_OVERVIEW.md（框架能力的高层视图）
4. 根据任务类型再读对应专题文档：
   - 加游戏 → docs/GAME_DEVELOPMENT_GUIDE.md + docs/NEW_GAME_TEST_GUIDE.md（隐藏信息游戏还要看 docs/MCTS_ALGORITHM.md 的 belief tracker 部分）
   - 改 MCTS → docs/MCTS_ALGORITHM.md
   - 改前端 → docs/WEB_DESIGN_PRINCIPLES.md
   - 修 bug → docs/KNOWN_ISSUES.md 看是不是已知

**以下情况 MUST 先和我确认，其他直接做**：
- 要改任何接口签名（IBeliefTracker/IFeatureEncoder/IGameState/IGameRules）
- 要添加新的 optional component 类型
- 不确定某个游戏的规则细节（NEVER 凭印象猜）
- 任务描述有多种合理解释

硬规则（NEVER 违反）：
- NEVER silent fallback：`catch(...) { return false; }` 或 `dict.get(k, default)` 掩盖 bug
- NEVER 在 Python 里重实现游戏/搜索逻辑，C++ 已做的不要重写
- NEVER 忘记把新游戏的 .cpp 加到 setup.py 的 sources 列表
- Tracker / encoder 的 public/private 分离是结构约束，不是建议
- 改完 MUST `python3 setup.py build_ext --inplace && pytest tests/ -q` 全过
- 改/加功能 MUST 更新 docs/ 下对应文档

不确定的地方主动问，不要假设。
```
