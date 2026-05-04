# DinoBoard — 功能概览

给游戏开发者的快速参考。详细实现请看 [GAME_DEVELOPMENT_GUIDE.md](GAME_DEVELOPMENT_GUIDE.md)。

---

## 核心组件

一个完整的游戏 = `state.cpp` + `rules.cpp` + `net_adapter.cpp` + `register.cpp` + `config/game.json` + `web/`

| 文件 | 提供的接口 | 职责 |
|------|-----------|------|
| **`<game>_state.cpp`** | `IGameState` | 游戏状态：当前玩家、是否终局、胜者、clone、`hash_public_fields` / `hash_private_fields`、多步动作起点标志 |
| **`<game>_rules.cpp`** | `IGameRules` | 合法动作、执行动作、撤销动作（隐藏信息游戏若要支持残局求解还需 `do_action_deterministic`） |
| **`<game>_net_adapter.cpp`** | `IFeatureEncoder` + `IBeliefTracker` | 状态 → 神经网络输入张量 + 合法掩码（encoder）；观察历史 → 可能世界采样（belief tracker，隐藏信息游戏才需要） |
| **`<game>_register.cpp`** | `GameBundle` 工厂 + `GameRegistrar` | 把上面的组件打包注册到框架，提供变体（如 `splendor_2p` / `splendor_3p`）、`heuristic_picker`、`public_event_extractor` 等可选组件 |
| **`config/game.json`** | — | 训练超参数配置（simulations / lr / 网络结构 / warm start 等） |
| **`web/<game>.js`** | `createApp(...)` | 玩家交互界面，验收训练结果 |

---

## 搜索与决策

AI 由两个核心模块协作组成：**MCTS + 双头神经网络**（policy/value）负责搜索与评估，**Belief Tracker** 负责维护当前玩家的信息认知。搜索时，belief tracker 提供"我知道什么"，MCTS 在此基础上构造可能世界并搜索最优动作。整条 AI 链路只依赖观察历史和 belief，不读取游戏的真实隐藏状态——这使得同一套 AI 既能用于 selfplay 训练，也能对接外部游戏服务器（AI 只接收"谁打了什么"的观察序列）。

### MCTS（ISMCTS）

神经网络引导的 Monte Carlo Tree Search，原生支持 N 人游戏（2-4 人）。**算法详解见 [MCTS_ALGORITHM.md](MCTS_ALGORITHM.md)**。此处只列要点：

- **Root 采样 determinization**：每次 simulation 从 belief 采一个完整世界（opp 手 + deck 顺序），之后 descent 纯 deterministic
- **DAG 而非 tree**：全局 `unordered_map<StateHash64, int>` 表，同一 info set 从不同路径到达共享节点
- **节点 key 按 acting player 视角**：`state.state_hash_for_perspective(state.current_player())`——每个决策节点真正代表一个合法 info set
- **UCT2** UCB 公式：`sqrt()` 分子底用"刚经过的入边的 visit_count"，不是 node global visit_count，避免 DAG 多父路径下的 over-exploration 偏差
- **Step counter 防环**：`IGameState::step_count_` 单调递增，DAG 结构性 acyclic
- **Encoder 对齐 hash scope**：encoder 只读 `public + current player's private`，和 hash 口径一致

主要配置：`simulations`、`c_puct`、`temperature`（支持 schedule）、Dirichlet 噪声。

### 残局求解（Tail Solve）

MCTS 前用 alpha-beta 尝试精确求解，证明必赢则跳过 MCTS。使用 paranoid 假设（所有对手联合针对当前玩家），支持任意玩家数。仅在 proven win 时替代 MCTS。内置 `AlphaBetaTailSolver`，通过 `depth_limit` + `node_budget` 控制预算。

**适用范围**：确定性游戏开箱即用。**随机或隐藏信息游戏需要开发者实现 `do_action_deterministic`**——把随机结果 / 隐藏字段用占位符替代，并在规则引擎里正确处理占位符语义（占位符不可打出、不触发能力等）。做不出来就不注册 `tail_solver`，框架不强制。当前 Splendor / Azul 实现了这个接口，Love Letter / Coup 没实现。详见 [Guide §3.2](GAME_DEVELOPMENT_GUIDE.md#32-可选实现do_action_deterministic残局求解需要) 和 [Guide §9.2](GAME_DEVELOPMENT_GUIDE.md#92-itailsolver--残局求解器)。

### 随机性与隐藏信息

物理随机和信息不对称在 ISMCTS 里被**统一处理**，不需要为两者分别设计机制。核心机制：

1. **Root 采样吞掉所有未来随机**。每次 sim 开头 `belief_tracker->randomize_unseen(sim_state, rng)` 把 opp 手牌 + 未来抽牌 + 任何不确定字段一次性采样成具体值。之后 descent 完全确定
2. **观察者可见的后果自然通过 hash 分叉**。Splendor tier 翻新卡（公开）→ 不同 sim 不同 tableau → 不同 `hash_public_fields` → 不同决策节点（DAG 里自然分化）
3. **观察者不可见的后果自然通过 hash 合并**。opp 抽牌（private to opp）→ observer 回合节点 key 里不含 opp 手 → 不同采样世界汇合到同一节点，visit 正确聚合

这套设计没有显式的 chance node 机制：在 info-set keying + DAG 下，chance node 的分叉/合并由 hash 等价关系自动完成。

**必要条件**：对信息不对称游戏注册 `belief_tracker` + `hash_private_fields(p)` + `public_event_extractor` + `initial_observation_extractor`。详见 [Guide §11](GAME_DEVELOPMENT_GUIDE.md#11-隐藏信息与-belief-tracker)。

**训练增强：Peek 模式**

Peek 是训练增强手段，用全知状态搜索（跳过 root 采样）帮助训练早期学稳策略。通过 `game.json` 的 `peek_steps` 参数控制：前 `peek_steps` 步训练用 peek（MCTS 看真相），之后切回 ISMCTS。默认 0（始终 ISMCTS）。详见 [Guide §9](GAME_DEVELOPMENT_GUIDE.md#9-训练可选特性)。

**现有游戏的组合**：

| 游戏 | 物理随机 | 信息不对称 | 需要注册 |
|------|---------|-----------|------|
| TicTacToe / Quoridor | 无 | 无 | 仅 state / rules / encoder |
| Azul | 袋中顺序未来随机（对称） | 无非对称 hidden | + belief_tracker（无 hash_private） |
| Splendor | 翻牌 | 盲压暗牌 | + belief_tracker + hash_private + events |
| Love Letter | 抽牌 | 手牌 | + belief_tracker + hash_private + events |
| Coup | 抽/洗牌 | 暗牌 | + belief_tracker + hash_private + events |

---

## AI API — 双重身份

框架在 `platform/ai_service/` 提供一个 observation-only 的 AI 推理 API：外部调用者只传动作 ID 和公开事件，API 内部只维护自己的 state，响应只有动作 ID 和元数据（任何 state 字段都不跨越边界）。端点：`POST /ai/sessions` / `POST /ai/sessions/{id}/observe` / `POST /ai/sessions/{id}/decide` / `DELETE /ai/sessions/{id}`。

这套 API 有**两重用途**：

1. **对内——证明 AI 决策链路不读真实状态**：CLAUDE.md 的核心设计原则要求 AI 只依赖观察序列。唯一能硬性证明分离的办法就是：把 AI 放到独立 session，用和 ground truth **不同的 seed** 初始化，只靠公共事件同步，然后检查两边的 belief 是否逐步等价。`test_api_belief_matches_selfplay` 就是这个验证。

2. **对外——把本框架训练的 AI 接入第三方游戏系统**：任何第三方（网站、APP、桌游平台）只要实现"观察事件 → API 请求"的翻译层，就能把 AI 当作对手或助手——不需要共享任何游戏 state 代码，不需要暴露 C++ 引擎。整体 API 文档在 [docs/AI_API.md](AI_API.md)，每个游戏的专属动作编码和事件格式在 `docs/games/<game>_api.md`。

**新游戏必须同时写两套文档**：`docs/GAME_DEVELOPMENT_GUIDE.md` 已经介绍如何在框架内注册游戏，另外还要写一份面向第三方接入者的 `docs/games/<game>_api.md`，说明动作 ID 编码、事件格式、调用示例。模板见 `docs/games/tictactoe_api.md`（纯公开游戏最简例）和 `docs/games/splendor_api.md`（对称随机 + 非对称隐藏的复杂例子）。

### 两层验收（新游戏必须全通过）

| 层次 | 测试 | 作用 |
|------|------|------|
| 1. API 契约 | `tests/test_ai_api_separation.py::test_full_game_via_api[<game>]` | API 边界无 state 泄漏；AI 端到端能完成对局 |
| 2. Belief 等价（随机游戏） | `tests/test_api_belief_matches_selfplay.py::*[<game>]` | 独立 seed 启动的 AI，belief / 公开 state / legal actions 都和自博弈对齐 |

如果 AI 代码有任何对真实 state 的隐藏读取，第二层的三个断言至少会触发一个——所以 **跑通这两层就是信息论层面证明了分离**。新游戏不通过这两层不算验收合格。详见 [Guide §17](GAME_DEVELOPMENT_GUIDE.md#17-ai-api-分离验收--信息泄漏的唯一证明)。

---

## 训练

```
warm start → selfplay → 收集样本 → 训练网络 → gating eval → 更新 best model → 循环
```

全流程 C++ 实现（selfplay、搜索、求解、eval），Python 只做训练循环调度和网络训练。配置驱动，不需要改代码。

### Value Head（多人支持）

Value head 输出 N 维向量（N = num_players），原生支持 2-4 人游戏。输出为 perspective-relative 排序（与 encoder 旋转对齐），`values[0]` = 当前玩家的价值。MCTS backup 时自动旋转回绝对玩家顺序。ONNX evaluator 兼容旧的标量 value head（1 维输出自动展开为零和 N 维）。`game_metadata(game_id)` 从 C++ 获取真实的 `num_players`、`action_space`、`feature_dim`。

### Warm Start

用 heuristic 或随机对局收集样本预训练网络，让初始网络不是纯随机。需要注册 `heuristic_picker`。

### Selfplay

每步独立掷骰判定两个维度：MCTS vs Heuristic 选动作、完整 vs 过滤动作空间。四种组合都生成训练样本。`legal_mask` 始终为完整合法动作集，被过滤动作在 policy target 中 visits 为 0。详见 [Guide §9](GAME_DEVELOPMENT_GUIDE.md#9-训练可选特性)。

### 训练增强

| 机制 | 说明 |
|------|------|
| **启发式引导** | `heuristic_picker` 给动作打分，混入 selfplay 加速早期学习 |
| **辅助训练信号** | `auxiliary_scorer` 提供 win/loss 外的额外学习目标（网络多一个 score head） |
| **动作过滤** | `training_action_filter` 裁剪明显垃圾动作，概率衰减到 0 |
| **温度 Schedule** | 分段线性衰减：开局高温探索，中后局低温利用 |
| **Dirichlet 噪声** | 根节点注入，可限制只在前 N 步 |
| **超时裁决** | `adjudicator` 在 `max_game_plies` 后判定胜负，selfplay 和 eval 均生效 |
| **Peek 模式** | 训练早期用全知状态搜索（忽略随机性和隐藏信息），先学稳基本策略。仅训练使用，实战 AI 不使用 |

### 评估

每 `--eval-every` 步触发，包含 benchmark eval + gating。Benchmark 支持 `heuristic_constrained`、`heuristic_free` 和指定 ONNX 模型。Gating 固定执行 latest vs best 对打，胜率 ≥ 阈值时更新 `model_best.onnx`。N 人游戏中 candidate 轮流坐每个座位以消除座位偏差。详见 [Guide §7.3](GAME_DEVELOPMENT_GUIDE.md#73-training-字段)。

---

## Web 前端

前端通过 HTTP API 与引擎交互，读取 `state_serializer` 渲染画面，读取 `action_descriptor` 理解动作语义。

**交互设计原则**（详见 [WEB_DESIGN_PRINCIPLES.md](WEB_DESIGN_PRINCIPLES.md)）：

- **自然交互**：不要给每个动作一个按钮。按物理游戏的交互方式设计 UI（点击棋子→点击目标、拖放等）。
- **空间锚定**：固定区域（棋盘、牌库、玩家面板）必须有固定的屏幕位置和尺寸，不随内容增减而移动。
- **动作动画**：每个动作（人类或 AI）都应有动画过渡，通过 `describeTransition` 向框架描述动画步骤。
- **视觉可辨性**：元素颜色要有足够对比度，可操作元素有明确反馈，关键信息一眼可见。

**动作空间约束**：人类永远看完整合法动作集；AI 是否用 filter 由 `use_filter` 参数控制。

### 通用布局

general 层提供统一页面布局，游戏前端只需填充内容：

```
┌─────────────────────────────────────────────────────┐
│                    上方区域                           │
│  ┌──────────────────────┐  ┌──────────────────────┐  │
│  │                      │  │  信息栏               │  │
│  │                      │  │  回合指示 / 胜率 /    │  │
│  │   公共游戏区          │  │  AI 提示              │  │
│  │   （棋盘 / 牌桌）     │  ├──────────────────────┤  │
│  │                      │  │  录像窗口              │  │
│  │                      │  │  （对局中隐藏，        │  │
│  │                      │  │   结束后显示）         │  │
│  └──────────────────────┘  └──────────────────────┘  │
├─────────────────────────────────────────────────────┤
│                    玩家区域                           │
│  2 人：左右分列                                       │
│  ┌────────────────────┐  ┌────────────────────┐      │
│  │     玩家 0          │  │     玩家 1          │      │
│  └────────────────────┘  └────────────────────┘      │
│                                                      │
│  3-4 人：网格排列                                     │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐│
│  │  玩家 0  │ │  玩家 1  │ │  玩家 2  │ │  玩家 3  ││
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘│
└─────────────────────────────────────────────────────┘
```

### 多人模式

`game.json` 的 `players.max > 2` 的游戏（如 Splendor、Azul）在侧边栏显示人数选择和座位选择。人类占一个座位，其余 N-1 个座位由 AI 控制。AI 连续落子直到轮到人类。

### 高级操作

general 层统一实现，游戏前端不需要额外代码：
- **悔棋**：回退玩家最后一步 + AI 回应
- **替对手落子**：玩家替 AI 选动作，用于调试
- **智能提示**：AI 推荐最佳动作和胜率，不落子

### Web AI 配置（web.json）

每个游戏可有独立的 `config/web.json`（可选），配置 Web 对局中的 AI 参数，与训练配置分离。

| 字段 | 说明 |
|------|------|
| `ai_use_action_filter` | AI 是否遵循 `training_action_filter` |
| `analysis_simulations` | 录像分析 MCTS 模拟次数 |
| `difficulty_overrides` | 按难度覆盖 simulations 和 temperature |
| `tail_solve` | 残局求解配置（enabled / depth_limit / node_budget） |

训练增强组件在 Web AI 中的可用性：

| 组件 | Web AI 支持 | 配置方式 |
|------|------------|---------|
| **动作过滤** | 支持 | `web.json` 的 `ai_use_action_filter` |
| **残局求解** | 支持 | `web.json` 的 `tail_solve` |
| **启发式辅助** | 支持 | heuristic 难度直接用 `get_heuristic_action()` |
| **温度调节** | 支持 | `difficulty_overrides` 按难度设温度 |
| **Peek 模式** | 不支持 | 仅训练使用 |

详见 [Guide §7.2](GAME_DEVELOPMENT_GUIDE.md#72-webjson--web-平台配置)。

### AI Pipeline 与动作分析

每个 human-to-play 局面只跑一次 MCTS（precompute，在 AI 落子后立即启动），结果**一份两用**：① 人类点"智能提示"时返回 ② 人类落子后读缓存的 `action_values[chosen_action]` 算掉分。AI 自己的决策是另一次独立 MCTS。分析基于 per-player Q 值，一次搜索覆盖所有合法动作，支持任意人数。详见 [Guide §14.3](GAME_DEVELOPMENT_GUIDE.md#143-ai-pipeline-与动作分析)。

### 录像回放

仅专家难度可用。对局结束后进入回放模式，每帧附带掉分分析（≥5% 失误、≥10% 严重失误）。在线对局和测试录像使用统一 JSON 格式，支持 N 人回放。`platform/tools/eval_model.py` 可独立评估模型并生成录像。详见 [Guide §14.4-14.6](GAME_DEVELOPMENT_GUIDE.md#144-录像回放)。

---

## 可选组件速查

| 组件 | 何时需要 | 一句话说明 |
|------|----------|-----------|
| `tail_solver` | 想在残局精确计算 | 内置 AlphaBetaTailSolver |
| `tail_solve_trigger` | 想智能触发求解 | 基于局势而非固定轮数 |
| `heuristic_picker` | 想增加训练多样性 | 快速弱策略混入 selfplay |
| `auxiliary_scorer` | 想加速早期训练 | 提供额外学习信号 |
| `training_action_filter` | 动作空间有明显垃圾 | 训练时裁剪动作空间 |
| `adjudicator` | 游戏可能死循环 | 超时判定胜负 |
| `episode_stats_extractor` | 想追踪自定义指标 | 每局结束后提取统计 |
| `belief_tracker` | 有隐藏信息或物理随机 | ISMCTS root 采样的来源 |
| `public_event_extractor` / `applier` | 有隐藏信息 | 状态差 → 事件流（用于 AI API 重放） |
| `initial_observation_extractor` / `applier` | 有隐藏信息 | 游戏开局的 observer 可见信息 |

---

## 配置速查

### game.json — 训练配置

| 字段 | 说明 |
|------|------|
| `game_id` / `display_name` | 唯一标识和显示名 |
| `players: {min, max}` | 支持的玩家数范围 |
| `action_space` / `feature_dim` | 必须和 encoder 一致 |
| `training.simulations` | MCTS 模拟次数（支持 schedule） |
| `training.temperature*` | 温度及衰减 schedule |
| `training.tail_solve_*` | 残局求解开关和预算 |
| `training.heuristic_guidance_*` | 启发式引导步数和概率 |
| `training.gating_accept_win_rate` | gating 晋升阈值 |
| `network.hidden_layers` | MLP 隐层大小 |

### web.json — Web 平台配置（可选）

| 字段 | 说明 |
|------|------|
| `ai_use_action_filter` | Web AI 是否遵循动作过滤 |
| `analysis_simulations` | 录像分析 MCTS 模拟次数（默认 5000） |
| `difficulty_overrides.<难度>.simulations` | 覆盖该难度的 MCTS 模拟次数 |
| `difficulty_overrides.<难度>.temperature` | 覆盖该难度的选择温度 |
| `tail_solve.enabled` | Web AI 是否启用残局求解 |
| `tail_solve.depth_limit` / `node_budget` | 残局求解搜索深度和节点预算 |

完整字段说明和配置示例详见 [Guide §7](GAME_DEVELOPMENT_GUIDE.md#7-配置文件)。

---

## 新游戏开发步骤

1. 定义状态结构（继承 `CloneableState<T>`）
2. 实现规则（`legal_actions`、`do_action_fast`、`undo_action`、`do_action_deterministic`）
3. 实现特征编码器
4. 写 `register.cpp` 组装 GameBundle
5. 写 `game.json` 配置
6. 加入 CMake 构建
7. 写 Web 前端
8. 运行测试
9. 跑训练、看日志、调参

参考实现组合了不同类型的游戏，每个都是某一类接入模式的"范例"：

| 游戏 | 定位 | 展示什么 |
|------|------|----------|
| **TicTacToe** | 最基础的桌游（MVP） | IGameState / IGameRules / IFeatureEncoder 的最小闭环；不需要任何 optional 组件 |
| **Quoridor** | MCTS 最擅长的完全信息确定游戏 | heuristic_picker、tail_solver + tail_solve_trigger、adjudicator、auxiliary_scorer、training_action_filter 全配齐 |
| **Splendor** | 对称随机 + 非对称隐藏（暗牌盲预订） | belief_tracker 维护 `seen_cards` 增量、public_event_extractor 发 `deck_flip` / `self_reserve_deck`、`do_action_deterministic` 用占位符支持 tail solver |
| **Azul** | 纯对称物理随机（袋中抽瓷砖），无非对称隐藏 | belief_tracker 只驱动 `randomize_unseen`（袋子重洗），不需要 `hash_private_fields` |
| **Love Letter** | 非对称隐藏信息 + tracker 追踪精确知识的范例 | belief_tracker 维护 `known_hand_[]`（Priest 偷看 / Baron 比较 / King 交换后知道的牌），encoder 读 `tracker->known_hand(p)` 编码已知对手手牌 |
| **Coup** | **自定义 randomize_unseen 启发式**的范例 | claim / challenge / reveal 历史驱动 per-opp role signals，加权联合采样（带硬约束的 `remaining[R]`）保证全局守恒，避免 uniform 采样在诈唬游戏里退化成"永不质疑永不诈唬"均衡 |

详见 [Guide §16 完整 Checklist](GAME_DEVELOPMENT_GUIDE.md#16-完整-checklist)。

---

## 测试

框架内置大量参数化测试，自动覆盖所有已注册游戏。新游戏只需将 game_id 加入 `tests/conftest.py` 的 `CANONICAL_GAMES` 列表。

详见 [Guide §15 测试](GAME_DEVELOPMENT_GUIDE.md#15-测试)。

---

## 框架局限性

本框架基于 MCTS + 神经网络的 AlphaZero 范式，以下类型的游戏不适合或需要其他方法：

### 1. 需要混合策略均衡的游戏

AlphaZero 训练的是确定性策略（给定状态输出固定概率分布），对需要精确混合策略均衡的游戏效果有限。这类游戏推荐使用 **CFR（Counterfactual Regret Minimization）** 或 **Neural Fictitious Self-Play / DeepCFR** 等专门求解均衡的算法。虽然本框架通过温度和 Dirichlet 噪声引入随机性，但这不等价于学到理论最优的混合策略。

**例：德州扑克**。最优策略要求在特定频率下 bluff（如河牌圈用垃圾牌以精确比例下注），selfplay 训练容易收敛到确定性策略（总是 bluff 或总是弃牌），而不是理论上的混合均衡频率。Libratus/Pluribus 使用 CFR 变体正是因为它能收敛到近似 Nash 均衡。

### 2. 决策高度依赖历史观察的游戏

本框架的特征编码是基于当前状态的快照，不直接编码完整历史序列。对于需要从历史动作序列中推理对手意图的游戏，能力受限于编码器能捕获多少历史信息：
- **可行的处理**：Belief tracker 维护确定性历史推断（如"某张牌已被打出"），编码器将关键历史信息编码为状态特征（如 Love Letter 的弃牌区）
- **困难的情况**：需要记住长序列行为模式来推断对手策略类型的游戏，目前无法编码。序列建模（Transformer/RNN 架构）可能是更合适的方案

**例：花火（Hanabi）**。玩家看不到自己的手牌，只能通过队友给出的提示序列推断手牌内容。关键信息不在当前状态中，而是分散在"第 3 轮队友指了我左边那张说是红色，第 7 轮又指了同一张说是 3"这样的历史提示链中。每条提示的含义取决于给出时的上下文（队友当时还能看到什么），需要完整回溯历史才能准确推理。

### 3. 动作空间无法有限编码的游戏

框架要求 `action_space` 为固定大小的整数，每个动作对应一个唯一 ID。对于动作空间为组合爆炸的游戏，固定编码方案要么维度太大导致网络训练困难，要么需要复杂的分层动作解码。这类游戏更适合使用分层动作空间（先选动作类型再选具体参数）或自回归策略网络。

**例：斗地主**。出牌组合包括单张、对子、三带一、顺子、连对、飞机带翅膀等，且长度可变（顺子可以是 5 张到 12 张）。合法组合总数超过 27,000 种，直接用固定 action_space 编码既稀疏又低效——绝大多数 ID 在任何给定手牌下都不合法。DouZero 等成功方案采用分层解码（先选牌型再选具体牌）来避免这个问题。

### 4. 卡牌构筑类游戏

框架只解决"给定牌组怎么打"（局内决策），不涉及"怎么组牌"（deck construction）。卡牌构筑是一个独立的元游戏（meta-game）问题：从几千张卡池中选 30-60 张组成有协同效应的牌组，本质上是组合优化而非博弈搜索，且构筑质量需要大量对局才能评估，反馈循环远长于单局决策。

**例：万智牌 / 炉石传说**。构筑涉及卡牌间的协同效应（combo）、对当前环境（meta）的针对性选择等，这些无法通过 MCTS 搜索解决。一个可行的分工是：用其他方法（如 LLM 辅助或进化算法）负责构筑，本框架负责局内对战评估。

### 5. 稀疏奖励 + 长对局

框架当前使用终局胜负作为唯一训练信号（`z_values`），中间步骤没有奖励。MCTS 的 value backup 在一定程度上缓解了这个问题——搜索深度足够时 value head 能学到对终局结果的估计，等效于中间密集信号。但当游戏流程很长（几百步以上）且搜索深度远不够覆盖时，value head 的 bootstrap 质量会很差，训练信号变得极其稀疏，收敛极慢甚至不收敛。

虽然 `AuxiliaryScorer` 可以注入 game-specific 的辅助奖励，但这本质是 reward shaping，容易引入偏差。更系统的方向是 TD(lambda) 式 value bootstrapping（用 value head 自身的中间预测做 target 而非只用终局 z_values），但这需要 value head 本身有一定准确度才能形成正反馈。当前框架尚未实现。

### 6. 仅 CPU 自博弈

当前自博弈管线全部跑在 CPU 上（ONNX CPUExecutionProvider），训练侧 PyTorch 也默认 CPU。GPU batch inference 需要架构改造：从当前"每个 worker 独立推理"改为"集中式 inference server + 多局异步攒 batch"，引入 IPC 通信和同步等待。

**当前网络规模下不值得**。MCTS 每步 simulation 是顺序的（前一步 backup 完才知道下一步走哪个节点），所以高 simulation 数不创造 batch 机会——batch 只能来自多个 worker 的并发请求。而 GPU batch inference 的完整链路（IPC 发送 → 攒 batch → CPU→GPU 拷贝 → GPU 计算 → GPU→CPU 拷贝 → IPC 返回）有 ~100-200μs 的固定开销。当前最大网络 [512×4]（~1M 参数）的 CPU 单次推理约 200μs，与 GPU 固定开销在同一量级，单个 worker 看到的延迟持平甚至更差。

**GPU 明确值得的条件**：单次 CPU 推理到毫秒级（GPU 固定开销相对可忽略），对应网络 > ~5M 参数（小型 ResNet 或 Transformer 架构）。如果未来引入 Transformer-based belief network 做序列建模，GPU 加速将成为必要。训练侧加 GPU 则很直接，改动很小。

### 关于裸 PPO

对于本框架定位的回合制桌游来说，很少有裸 PPO 优于 Net-MCTS 的情况。回合制桌游的决策频率低（每步可以花几百毫秒搜索），且分支因子通常有限（几十到几百），这正是 MCTS 发挥优势的场景——搜索树的宽度和深度都在可探索范围内，前瞻搜索提供的信息增益远超单次网络推理。裸 PPO 更适合实时游戏（如星际争霸、Dota 2）或动作空间极大的场景，这些不在本框架的目标范围内。

---

## Future Work：概率化 Belief Tracking

当前大多数 belief tracker 只追踪确定性信息，`randomize_unseen` 在已知约束下均匀采样未知部分，没有利用历史动作序列中蕴含的概率信号。

**Coup 的 `randomize_unseen` 已经是这件事的雏形**：它在采样里加入了 claim/challenge 历史驱动的加权（per-opp role signal count × 剩余牌池硬约束），证明"非均匀 belief 采样 + ISMCTS"的路走得通、搜索能收敛（详见 [Guide §11.4](GAME_DEVELOPMENT_GUIDE.md#114-实现示例) 和 `games/coup/coup_net_adapter.cpp`）。

下一步只是把手写启发式换成可训练的序列模型（Transformer / RNN 输入观察历史 → 输出 opp 隐藏状态分布），在 `randomize_unseen` 里替换 sampler 本身即可——MCTS、DAG、UCT2 都不需要改。

---

## Future Work：对手池维护（Opponent Pool）

当前训练管线是单一 latest 模型自博弈。对最优解是混合策略的游戏（Coup / Love Letter 这类含诈唬博弈），单 latest 自博弈会陷入**剪刀石头布循环**——策略 A 被 B 克制，B 被 C 克制，C 又被 A 克制，Elo 停滞不前。

解法是维护对手池：保留历史快照，selfplay 时从池里抽对手而不是只对最新的自己打。成熟算法很多，直接选型即可：Fictitious Self-Play、PFSP（AlphaStar）、League Training、Population-Based Training。
