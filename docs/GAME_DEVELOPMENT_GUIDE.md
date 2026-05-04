# 游戏开发指南

> 本文档事无巨细地描述了如何在 DinoBoard-v2 平台上添加一个新游戏。
> 阅读后你将了解所有必须实现的接口、所有可选特性、配置文件格式、构建集成方式，以及常见踩坑。

---

## 目录

1. [总览](#1-总览)
2. [IGameState — 游戏状态](#2-igamestate--游戏状态)
3. [IGameRules — 游戏规则](#3-igamerules--游戏规则)
4. [IFeatureEncoder — 特征编码](#4-ifeatureencoder--特征编码)
5. [GameBundle — 组件注册](#5-gamebundle--组件注册)
6. [GameRegistrar — 注册模式](#6-gameregistrar--注册模式)
7. [配置文件](#7-配置文件)（game.json 训练配置、web.json Web 平台配置）
8. [构建集成](#8-构建集成)
9. [训练可选特性](#9-训练可选特性)（Heuristic、TailSolver、Filter、AuxScorer、Adjudicator、Stats、Peek）
10. [物理随机性](#10-物理随机性)（StochasticDetector、Chance Node、Truncate）
11. [隐藏信息与 Belief Tracker](#11-隐藏信息与-belief-tracker)（ISMCTS 根采样、Encoder 信息屏障）
12. [特征编码最佳实践](#12-特征编码最佳实践)
13. [Web 前端开发](#13-web-前端开发)（Serializer、Descriptor、目录结构、交互设计、createApp）
14. [Web 平台功能](#14-web-平台功能)（通用布局、高级操作、Pipeline 与动作分析、录像、API）
15. [测试](#15-测试)（自动化测试套件、运行方式、接入新游戏）
16. [完整 Checklist](#16-完整-checklist)

---

## 1. 总览

添加一个新游戏需要创建以下文件：

```
games/<your_game>/
├── <game>_state.h          # 游戏状态定义
├── <game>_state.cpp        # 游戏状态实现
├── <game>_rules.h          # 规则引擎头文件
├── <game>_rules.cpp        # 规则引擎实现
├── <game>_net_adapter.h    # 特征编码头文件
├── <game>_net_adapter.cpp  # 特征编码实现
├── <game>_register.cpp     # 注册到全局 GameRegistry
├── config/
│   └── game.json           # 训练和网络超参数
├── CMakeLists.txt          # CMake 构建文件
└── web/                    # Web 前端（玩家游玩 + 验收训练结果）
    ├── index.html
    ├── styles.css
    └── <game>.js
```

需要实现 **3 个核心类**（State、Rules、FeatureEncoder），外加 **1 个注册文件** 和 **1 个配置文件**。

所有 C++ 代码位于 `board_ai::<game_name>` 命名空间下。

---

## 2. IGameState — 游戏状态

**文件**：`engine/core/game_interfaces.h`

### 2.1 推荐继承方式

使用 CRTP 模板 `CloneableState<T>` 自动实现 `clone_state()` 和 `copy_from()`：

```cpp
#include "../../engine/core/game_interfaces.h"

namespace board_ai::mygame {

class MyGameState final : public CloneableState<MyGameState> {
 public:
  // === 必须实现的方法 ===
  StateHash64 state_hash(bool include_hidden_rng) const override;
  int current_player() const override;
  bool is_terminal() const override;
  int num_players() const override;
  int winner() const override;
  void reset_with_seed(std::uint64_t seed) override;

  // === 可选覆盖 ===
  // int first_player() const override;       // 默认返回 0
  // std::uint64_t rng_nonce() const override; // 默认返回 0

  // === 游戏数据 ===
  int current_player_ = 0;
  int winner_ = -1;
  bool terminal = false;
  int move_count = 0;
  std::array<int, 2> scores{};
  // ... 你的棋盘数据 ...

  // === Undo 支持 ===
  struct UndoRecord { /* 存储 do_action 前的快照 */ };
  std::vector<UndoRecord> undo_stack;
};

}  // namespace board_ai::mygame
```

### 2.2 各方法详解

#### `state_hash(bool include_hidden_rng) -> StateHash64`

返回当前状态的 64 位哈希。用于转置表和状态去重。

**要求**：
- 相同状态必须返回相同哈希
- 不同状态应尽量返回不同哈希（使用 `hash_combine`）
- `include_hidden_rng=true` 时，包含 RNG 种子等隐藏信息（用于 MCTS 树去重）
- `include_hidden_rng=false` 时，只哈希可观察的棋盘状态（用于转置表）

**示例**：
```cpp
StateHash64 state_hash(bool include_hidden_rng) const override {
  StateHash64 h = 0;
  hash_combine(h, current_player_);
  hash_combine(h, move_count);
  for (auto cell : board) hash_combine(h, cell);
  if (include_hidden_rng) hash_combine(h, rng_salt);
  return h;
}
```

#### `current_player() -> int`

返回当前行动玩家的索引（0-based）。

#### `is_terminal() -> bool`

游戏是否结束。true 时 `legal_actions()` 必须返回空。

#### `is_turn_start() -> bool`（可选，默认 true）

当前位置是否是一个玩家动作序列的起点。对于有多步动作的游戏（如 Splendor 的拿币→退币→选贵族），在子动作（退币、选贵族）阶段返回 false。可用于 UI 提示玩家正在进行子操作。简单游戏（每步恰好一个动作）不需要重写。

> **注意**：悔棋和替对手落子不依赖此标志，而是通过 `last_actor`（录像帧中的行动者）判断连续回合——无论是子动作还是跨轮连续行动，统一处理。

#### `num_players() -> int`

玩家总数。通常为 2，最多支持 4。

#### `winner() -> int`

- 游戏未结束或平局：返回 -1
- 有赢家：返回赢家的索引（0-based）

#### `reset_with_seed(uint64_t seed)`

重置为初始状态。使用 `sanitize_seed(seed)` 确保种子非零。

```cpp
void reset_with_seed(std::uint64_t seed) override {
  rng_salt = sanitize_seed(seed);
  current_player_ = 0;
  winner_ = -1;
  terminal = false;
  move_count = 0;
  scores = {0, 0};
  board.fill(kEmpty);
  undo_stack.clear();
}
```

#### `rng_nonce() -> uint64_t`（可选）

返回驱动 PRNG 的 state 字段值（如 `draw_nonce`），仅用于 debug 和 tail solver 的哈希去重。**ISMCTS-v2 不依赖此字段**——root 采样后 descent 是 deterministic，框架通过 `step_count_` 保证 DAG acyclic，不需要检测随机转移。确定性游戏（如 Quoridor、TicTacToe）不需要覆盖。

### 2.3 UndoRecord 设计模式

每次 `do_action_fast` 执行前，将所有会被修改的字段快照保存到 `UndoRecord`，压入 `undo_stack`。`undo_action` 弹出最后一条记录并恢复状态。

```cpp
struct UndoRecord {
  int prev_player;
  int prev_winner;
  bool prev_terminal;
  int prev_move_count;
  std::array<int, 2> prev_scores;
  // ... 动作相关的还原信息 ...
};
```

**关键**：`undo_stack` 必须用 `std::vector`（不是 `std::stack`），因为 `CloneableState<T>` 依赖拷贝构造函数进行克隆。

### 2.4 CloneableState 的工作原理

`CloneableState<T>` 通过 CRTP 自动生成：
- `clone_state()` → `make_unique<T>(*static_cast<const T*>(this))`（拷贝构造）
- `copy_from(other)` → `*static_cast<T*>(this) = checked_cast<T>(other)`（拷贝赋值）

前提：你的状态类必须是**可拷贝构造和可拷贝赋值**的。如果使用了 `unique_ptr` 等不可拷贝的字段，需要自己写拷贝构造函数。

---

## 3. IGameRules — 游戏规则

**文件**：`engine/core/game_interfaces.h`

### 3.1 必须实现的方法

```cpp
class MyGameRules final : public IGameRules {
 public:
  bool validate_action(const IGameState& state, ActionId action) const override;
  std::vector<ActionId> legal_actions(const IGameState& state) const override;
  UndoToken do_action_fast(IGameState& state, ActionId action) const override;
  void undo_action(IGameState& state, const UndoToken& token) const override;
};
```

#### `validate_action(state, action) -> bool`

检查 action 在当前状态下是否合法。用于 Python bindings 的输入验证。

#### `legal_actions(state) -> vector<ActionId>`

返回当前玩家所有合法动作。**terminal 状态必须返回空 vector**。

ActionId 是 `int32_t`，你需要设计一套编码方案将游戏动作映射为连续整数。例如：

```
TicTacToe：action = cell_index (0-8)
Quoridor：action ∈ [0, 209)
  - [0, 81)   → 移动棋子到 (row, col)
  - [81, 145)  → 放置水平墙 (row, col)
  - [145, 209) → 放置垂直墙 (row, col)
```

建议提供 `encode_xxx_action()` 和 `decode_xxx_action()` 辅助函数。

#### `do_action_fast(state, action) -> UndoToken`

**核心热路径方法**。在状态上原地执行动作，返回 UndoToken。MCTS 每次模拟调用上千次。

**实现模板**：
```cpp
UndoToken do_action_fast(IGameState& state, ActionId action) const override {
  auto* s = &checked_cast<MyGameState>(state);
  UndoToken token{};
  token.undo_depth = static_cast<std::uint32_t>(s->undo_stack.size());

  // 1. 保存快照
  UndoRecord rec{};
  rec.prev_player = s->current_player_;
  rec.prev_winner = s->winner_;
  // ... 保存所有会被修改的字段 ...
  s->undo_stack.push_back(rec);

  // 2. 执行动作
  // ... 修改棋盘状态 ...

  // 3. 更新游戏元数据
  s->move_count += 1;
  // ... 检查胜负 ...
  s->current_player_ = 1 - s->current_player_;  // 切换玩家

  return token;
}
```

#### `undo_action(state, token)`

弹出 undo_stack 顶部记录，恢复状态。

```cpp
void undo_action(IGameState& state, const UndoToken& token) const override {
  auto* s = &checked_cast<MyGameState>(state);
  if (s->undo_stack.empty()) return;
  const auto rec = s->undo_stack.back();
  s->undo_stack.pop_back();

  // 还原所有修改过的字段
  s->current_player_ = rec.prev_player;
  s->winner_ = rec.prev_winner;
  // ...

  (void)token;  // token.undo_depth 可用于一致性检查
}
```

### 3.2 可选实现：do_action_deterministic（残局求解需要）

#### `do_action_deterministic(state, action) -> UndoToken`

执行动作的**确定化版本**——随机结果 / 依赖隐藏信息的结果用**占位符**替代。仅当启用残局求解（Tail Solver）时才需要实现。基类默认实现直接委托给 `do_action_fast`。

**什么游戏要实现**：
- **纯确定游戏**（TicTacToe、Quoridor）：不用，默认实现就对
- **有物理随机的游戏**（Splendor 翻牌、Azul 袋中抽瓷砖）：要用 tail solver 就必须实现，已实现参考 `splendor_rules.cpp` / `azul_rules.cpp`
- **有隐藏信息的游戏**（Love Letter 看对手手牌、Coup 盲牌）：**理论上也可以实现**——只要开发者能给对手手牌 / 盲牌设计一个"不会破坏规则语义"的占位符，并在 `do_action_fast`（被调用 for 确定化路径时）正确处理占位符。做不出来就不要注册 `tail_solver`，框架不会强制

**实现原则**：
- 随机抽牌 / 翻牌：用占位符牌替代。规则引擎要识别占位符并标记不可用（不能打出、不能作为资源、不触发能力）
- 隐藏信息动作：比如 Guard 猜牌这种"动作效果依赖对手手牌"的，占位符手牌要让动作结果变为"明确可判的确定分支"（比如固定一个猜错分支，或者让此动作在 deterministic 路径里变成无效动作）
- 占位符不能破坏 `legal_actions` / `is_terminal` / `winner` 的正确性。Tail solver 会在这个确定化状态上完整展开搜索树

**选择不实现**：游戏注册时不设 `tail_solver` 即可，ISMCTS-v2 的主搜索路径（selfplay / arena / GameSession）完全不依赖 `do_action_deterministic`。

### 3.3 物理随机游戏的主搜索流程

主搜索（selfplay / arena / API / GameSession）**不使用** `do_action_deterministic`。流程：

1. `do_action_fast` 直接消费真实 state 里的 RNG（抽牌从 deck top 弹、翻牌翻 tier deck 等），正常推进
2. MCTS 每次 sim 开头先 `belief_tracker.randomize_unseen(sim_state, rng)` 把未知字段采样成具体值，之后 descent 完全 deterministic
3. 不同 sim 采不同的世界，observer 能分辨的后继（如 Splendor 翻出的公开牌）通过 `hash_public_fields` 差异自然分叉，observer 不能分辨的后继（如 opp 抽的私牌）在观察者决策节点通过 hash 合并汇聚

开发者只需实现 `do_action_fast` / `undo_action` 时正确更新 `step_count_` 和 RNG 状态。**如果你的游戏需要 tail solver，额外实现 §3.2 的 `do_action_deterministic`**。详见 [MCTS_ALGORITHM.md §7](MCTS_ALGORITHM.md#7-物理随机不是-chance-node而是-sampled-world)。

---

## 4. IFeatureEncoder — 特征编码

**文件**：`engine/core/feature_encoder.h`

### 4.1 必须实现的方法

```cpp
class MyGameFeatureEncoder final : public IFeatureEncoder {
 public:
  int action_space() const override;     // 动作空间大小
  int feature_dim() const override;      // 特征向量维度
  bool encode(const IGameState& state,
              int perspective_player,
              const std::vector<ActionId>& legal_actions,
              std::vector<float>* features,
              std::vector<float>* legal_mask) const override;
};
```

#### `action_space() -> int`

返回动作空间总大小（策略头的输出维度）。**必须和 game.json 中的 `action_space` 一致**。

#### `feature_dim() -> int`

返回特征向量维度（网络输入大小）。**必须和 game.json 中的 `feature_dim` 一致**。

#### `encode(state, perspective_player, legal_actions, *features, *legal_mask) -> bool`

核心编码函数。将游戏状态编码为浮点特征向量，并生成合法动作掩码。

**参数**：
- `perspective_player`：从哪个玩家的视角编码（"我" vs "对手"）
- `legal_actions`：当前合法动作列表
- `features`：输出，大小必须等于 `feature_dim()`
- `legal_mask`：输出，大小必须等于 `action_space()`，合法动作位置为 1.0f，其余为 0.0f

**示例**（TicTacToe，feature_dim=28）：
```cpp
bool encode(const IGameState& state, int perspective_player,
            const std::vector<ActionId>& legal_actions,
            std::vector<float>* features,
            std::vector<float>* legal_mask) const override {
  const auto& s = checked_cast<TicTacToeState>(state);
  features->clear();
  features->reserve(28);

  // 9 个格子 × 3 通道（是我的、是对手的、是空的）
  for (int i = 0; i < 9; ++i) {
    features->push_back(s.board[i] == perspective_player ? 1.0f : 0.0f);
    features->push_back(s.board[i] == (1 - perspective_player) ? 1.0f : 0.0f);
    features->push_back(s.board[i] == kEmpty ? 1.0f : 0.0f);
  }
  // 1 个标量：是否是先手
  features->push_back(perspective_player == s.first_player() ? 1.0f : 0.0f);

  // 合法动作掩码
  fill_legal_mask(action_space(), legal_actions, legal_mask);
  return true;
}
```

### 4.2 视角处理

**不要旋转棋盘**。只做「我 / 对手」的特征交换，加一个 scalar 标识「我是先手 / 后手」（或等价的方向标记）。

原因详见 `docs/KNOWN_ISSUES.md` 第 4 条：棋盘旋转容易把和格子绑定的结构（例如 Quoridor 里墙的「挡哪两条边」语义）旋转错，而且训练看起来能跑、但有一方的策略永远学不好，这种 bug 非常难定位。网络自己可以学 P0/P1 的不对称，不需要我们帮它「归一化」视角。

```cpp
// 正确示范：把 "我" 和 "对手" 的棋子都按 perspective_player 来选，棋盘坐标保持不变
const int me = perspective_player;
const int opp = 1 - perspective_player;
for (int i = 0; i < kCells; ++i) {
  features->push_back(s.board[i] == me ? 1.0f : 0.0f);
}
for (int i = 0; i < kCells; ++i) {
  features->push_back(s.board[i] == opp ? 1.0f : 0.0f);
}
// 告诉网络 "我" 是谁
features->push_back(perspective_player == 0 ? 1.0f : 0.0f);
```

动作输出层面 `policy_action_ids` 直接用原始 `ActionId`，不做任何旋转映射。

---

## 5. GameBundle — 组件注册

**文件**：`engine/core/game_registry.h`

GameBundle 是一个聚合所有游戏组件的结构体。工厂函数返回一个 GameBundle 实例。

### 5.1 全部字段

| # | 字段 | 类型 | 必须 | 说明 |
|---|------|------|------|------|
| 1 | `game_id` | `string` | 是 | 唯一标识符，如 `"quoridor"` |
| 2 | `state` | `unique_ptr<IGameState>` | 是 | 初始状态（已调用 `reset_with_seed`） |
| 3 | `rules` | `unique_ptr<IGameRules>` | 是 | 规则引擎 |
| 4 | `value_model` | `unique_ptr<IStateValueModel>` | 是 | 通常用 `DefaultStateValueModel`。`terminal_values()` 必须零和 |
| 5 | `encoder` | `unique_ptr<IFeatureEncoder>` | 是 | 特征编码器 |
| 6 | `belief_tracker` | `unique_ptr<IBeliefTracker>` | 否 | 有隐藏信息或物理随机的游戏必须注册 |
| 7 | `public_event_extractor` | `PublicEventExtractor` | 否 | (before, action, after, perspective) → events；tracker 通过这个更新 belief |
| 8 | `public_event_applier` | `PublicEventApplier` | 否 | 把事件应用到 state（AI API 侧重放用） |
| 9 | `initial_observation_extractor` | `InitialObservationExtractor` | 否 | 提取 perspective 的开局可见信息 |
| 10 | `initial_observation_applier` | `InitialObservationApplier` | 否 | 把 initial observation 填入 state（AI API 侧用） |
| 11 | `state_serializer` | `StateSerializer` | 否 | 状态序列化为 JSON（Web 前端需要） |
| 12 | `action_descriptor` | `ActionDescriptor` | 否 | 动作语义描述（Web 前端需要） |
| 13 | `heuristic_picker` | `HeuristicPicker` | 否 | 启发式策略（warm start + eval benchmark） |
| 14 | `tail_solver` | `unique_ptr<ITailSolver>` | 否 | 残局求解器（通常用 `AlphaBetaTailSolver`） |
| 15 | `tail_solve_trigger` | `TailSolveTrigger` | 否 | 残局求解触发条件（未注册则 fallback 到 ply 阈值） |
| 16 | `episode_stats_extractor` | `EpisodeStatsExtractor` | 否 | 每局自定义统计 |
| 17 | `adjudicator` | `GameAdjudicator` | 否 | 超时判定胜负 |
| 18 | `auxiliary_scorer` | `AuxiliaryScorer` | 否 | 辅助训练信号 |
| 19 | `training_action_filter` | `TrainingActionFilter` | 否 | 训练时约束动作空间 |

### 5.2 各类型签名

```cpp
// 状态序列化：state → key-value map
using StateSerializer = std::function<AnyMap(const IGameState& state)>;

// 动作描述：action → key-value map
using ActionDescriptor = std::function<AnyMap(ActionId action)>;

// 启发式策略：返回带分数的动作列表，框架根据 scores 选择动作
using HeuristicPicker = std::function<
    HeuristicResult(IGameState& state, const IGameRules& rules, std::uint64_t rng_seed)>;

struct HeuristicResult {
  std::vector<ActionId> actions;
  std::vector<double> scores;
};

// 每局统计
using EpisodeStatsExtractor = std::function<
    std::map<std::string, double>(
        const IGameState& final_state,
        const std::vector<SelfplaySampleView>& samples)>;

// 超时判定
using GameAdjudicator = std::function<int(const IGameState& state)>;

// 辅助分数
using AuxiliaryScorer = std::function<float(const IGameState& state, int player)>;

// 训练动作过滤
using TrainingActionFilter = std::function<std::vector<ActionId>(
    IGameState& state, const IGameRules& rules, const std::vector<ActionId>& legal)>;
```

---

## 6. GameRegistrar — 注册模式

**文件**：`<game>_register.cpp`

使用文件作用域的静态对象，在程序启动时自动注册游戏。

### 6.1 最小示例（TicTacToe）

```cpp
#include "../../engine/core/game_registry.h"
#include "mygame_state.h"
#include "mygame_rules.h"
#include "mygame_net_adapter.h"

namespace {

board_ai::GameRegistrar reg("mygame", [](std::uint64_t seed) {
  board_ai::GameBundle b;
  b.game_id = "mygame";

  auto s = std::make_unique<board_ai::mygame::MyGameState>();
  s->reset_with_seed(seed);
  b.state = std::move(s);

  b.rules = std::make_unique<board_ai::mygame::MyGameRules>();
  b.value_model = std::make_unique<board_ai::DefaultStateValueModel>();
  b.encoder = std::make_unique<board_ai::mygame::MyGameFeatureEncoder>();

  return b;
});

}  // namespace
```

### 6.2 完整示例（带所有可选特性）

参考 `games/quoridor/quoridor_register.cpp`，它注册了以下所有可选组件：

```cpp
board_ai::GameRegistrar reg("quoridor", [](std::uint64_t seed) {
  board_ai::GameBundle b;
  b.game_id = "quoridor";

  // 必须组件
  auto s = std::make_unique<QuoridorState>();
  s->reset_with_seed(seed);
  b.state = std::move(s);
  b.rules = std::make_unique<QuoridorRules>();
  b.value_model = std::make_unique<board_ai::DefaultStateValueModel>();
  b.encoder = std::make_unique<QuoridorFeatureEncoder>();

  // 状态序列化（Web 前端用）
  b.state_serializer = serialize_quoridor;
  b.action_descriptor = describe_quoridor;

  // 启发式策略（warm start + eval benchmark）
  b.heuristic_picker = heuristic_pick_quoridor;

  // 残局求解器
  b.tail_solver = std::make_unique<board_ai::search::AlphaBetaTailSolver>();

  // 每局统计
  b.episode_stats_extractor = [](const board_ai::IGameState&,
      const std::vector<board_ai::SelfplaySampleView>& samples)
      -> std::map<std::string, double> {
    return {{"turns", static_cast<double>(samples.size()) / 2.0}};
  };

  // 辅助训练信号
  b.auxiliary_scorer = [](const board_ai::IGameState& state, int player) -> float {
    const auto& qs = board_ai::checked_cast<QuoridorState>(state);
    int me = QuoridorRules::shortest_path_distance(qs, player);
    int opp = QuoridorRules::shortest_path_distance(qs, 1 - player);
    return std::tanh(static_cast<float>(opp - me) / 8.0f);
  };

  // 超时判定
  b.adjudicator = [](const board_ai::IGameState& state) -> int {
    const auto& qs = board_ai::checked_cast<QuoridorState>(state);
    int d0 = QuoridorRules::shortest_path_distance(qs, 0);
    int d1 = QuoridorRules::shortest_path_distance(qs, 1);
    if (d0 < d1) return 0;
    if (d1 < d0) return 1;
    return -1;
  };

  // 训练动作过滤
  b.training_action_filter = [](board_ai::IGameState& state, /* ... */) {
    // ... 过滤逻辑 ...
  };

  return b;
});
```

### 6.3 多变体注册

同一个游戏可以注册多个变体（如不同玩家数），使用不同的 game_id：

```cpp
// Splendor 注册了 4 个变体
board_ai::GameRegistrar reg_2p("splendor",    factory<2>);
board_ai::GameRegistrar reg_2p_("splendor_2p", factory<2>);
board_ai::GameRegistrar reg_3p("splendor_3p", factory<3>);
board_ai::GameRegistrar reg_4p("splendor_4p", factory<4>);
```

---

## 7. 配置文件

**位置**：`games/<game>/config/game.json`

训练 pipeline 自动发现此文件（通过 `training/cli.py` 中的 `find_game_config()`）。

### 7.1 顶层字段

| 字段 | 类型 | 必须 | 说明 |
|------|------|------|------|
| `game_id` | string | 是 | 必须和 GameRegistrar 注册的 id 一致 |
| `display_name` | string | 是 | 显示名称 |
| `players.min` | int | 是 | 最少玩家数 |
| `players.max` | int | 是 | 最多玩家数 |
| `action_space` | int | 是 | 动作空间大小（必须和 encoder 一致） |
| `feature_dim` | int | 是 | 特征维度（必须和 encoder 一致） |

### 7.2 web.json — Web 平台配置

**位置**：`games/<game>/config/web.json`（可选，不存在则全部用默认值）

Web 平台相关的 AI 参数独立于训练配置，放在 `web.json` 中管理。

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `ai_use_action_filter` | bool | false | Web 对局时 AI 搜索是否遵循 `training_action_filter`。人类永远不受约束；只控制 AI（pipeline、ai-hint、precompute）。只有注册了 `training_action_filter` 的游戏才需要设置 |
| `analysis_simulations` | int | 5000 | 录像分析和 precompute 的 MCTS 模拟次数。对局较短或决策空间较小的游戏可以调低，减少分析延迟 |
| `difficulty_overrides` | object | {} | 按难度覆盖 AI 参数。键为难度名（casual/expert），值可包含 `simulations` 和 `temperature` |
| `tail_solve` | object | {} | 残局求解配置。子字段见下 |
| `tail_solve.enabled` | bool | false | 是否在 Web 对局中启用残局求解。只有注册了 `tail_solver` 的游戏才有效 |
| `tail_solve.depth_limit` | int | 10 | 残局求解搜索深度上限 |
| `tail_solve.node_budget` | int | 200000 | 残局求解节点预算 |

**全局默认值**（未配置时）：casual=10 sim, expert=5000 sim，温度均为 0，分析 5000 sim。heuristic 难度不走 MCTS，直接调用 `get_heuristic_action()`，无需配置 simulations。

**示例**（Coup 的 `config/web.json`）：
```json
{
  "analysis_simulations": 2000,
  "difficulty_overrides": {
    "casual": {"simulations": 50, "temperature": 0.3},
    "expert": {"simulations": 2000, "temperature": 0.1}
  }
}
```

**示例**（Quoridor 的 `config/web.json`，启用动作过滤和残局求解）：
```json
{
  "ai_use_action_filter": true,
  "tail_solve": {
    "enabled": true,
    "depth_limit": 10,
    "node_budget": 200000
  }
}
```

**何时需要配置**：
- 含隐藏信息或虚张声势的游戏（Coup、Love Letter）：加非零温度，避免 AI 完全确定性
- 使用 training_action_filter 的游戏（Quoridor）：设置 `ai_use_action_filter: true`
- 注册了 tail_solver 的完全信息游戏（Quoridor）：设置 `tail_solve.enabled: true`，AI 在终局阶段会尝试精确求解

**向后兼容**：如果 `web.json` 不存在，平台会回退读取 `game.json` 中的 `web` 字段和 `ai_use_action_filter`。新游戏应使用 `web.json`。

### 7.3 training 字段

#### MCTS 搜索参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `simulations` | int | 200 | 最终 MCTS 模拟次数（eval 也用此值） |
| `c_puct` | float | 1.4 | PUCT 探索常数 |
| `temperature` | float | 1.0 | 基础温度（未启用 schedule 时使用） |
| `temperature_initial` | float | -1 | 温度衰减起始值（-1 表示使用 temperature） |
| `temperature_final` | float | -1 | 温度衰减终止值 |
| `temperature_decay_plies` | int | 0 | 温度从 initial 线性衰减到 final 的步数 |
| `dirichlet_alpha` | float | 0.3 | Dirichlet 噪声 alpha 参数 |
| `dirichlet_epsilon` | float | 0.25 | 根节点先验中噪声的比例 |
| `dirichlet_on_first_n_plies` | int | 30 | 只在前 N 步添加 Dirichlet 噪声 |
| `max_game_plies` | int | 500 | 最大步数（超出后调用 adjudicator 或判和） |

**温度调参建议**：
- `alpha` 的经验法则：`alpha ≈ 10 / action_space`
- 简单游戏（TicTacToe）：alpha=1.0, decay_plies=6
- 复杂游戏（Quoridor）：alpha=0.05, decay_plies=24

#### 训练循环参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `steps` | int | 1000 | 总训练步数 |
| `episodes_per_step` | int | 200 | 每步自我对弈局数 |
| `batch_size` | int | 512 | SGD mini-batch 大小 |
| `learning_rate` | float | 0.001 | AdamW 优化器学习率 |
| `weight_decay` | float | 1e-4 | AdamW weight decay |
| `train_batches_per_step` | int | 3 | 每步从 replay buffer 随机采样训练的 mini-batch 数 |
| `grad_clip_norm` | float | 1.0 | 梯度裁剪范数（0 表示不裁剪） |

#### MCTS Schedule 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `simulations` | int | 200 | 最终 MCTS 模拟次数（也用于 eval） |
| `simulations_start` | int | =simulations | 训练初始 MCTS 模拟次数，线性递增到 `simulations` |

如果 `simulations_start` 未设置或等于 `simulations`，则模拟次数全程固定（向后兼容）。

递增公式：`sims = start + min(1.0, step / (steps * 0.3)) * (simulations - start)`

即在前 30% 步数内线性爬坡到 `simulations`，之后固定。这样训练早期的自博弈更快，能更快形成有效的 replay buffer。

**注意**：eval 对弈始终使用 `simulations`（最终值），确保评估标准一致。

#### Eval 与 Gating 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `gating_accept_win_rate` | float | 0.55 | latest vs best 的晋升阈值 |
| `eval_temperature` | float | 0.0 | gating 和 ONNX benchmark 对局的动作选择温度。0=贪心，>0 引入随机性。确定性游戏建议 0.1 |

每 `--eval-every` 步触发一轮评估，包含两部分：

**Benchmark eval**（通过 CLI `--eval-benchmark` 配置，可同时指定多个）：

| Benchmark 值 | 说明 |
|-------------|------|
| `heuristic_constrained` | 模型 vs heuristic，模型受 action filter 约束 |
| `heuristic_free` | 模型 vs heuristic，模型不受 action filter 约束 |
| ONNX 文件路径 | 模型 vs 指定 ONNX 模型 |

示例：
```bash
# Quoridor: constrained + free + gating
python3 -m training.cli --game quoridor --output runs/quoridor_v12 \
  --eval-benchmark heuristic_constrained heuristic_free

# 不传 --eval-benchmark，只跑 gating
python3 -m training.cli --game quoridor --output runs/quoridor_v12
```

**Gating eval**（固定执行，不受 `--eval-benchmark` 影响）：latest vs best 对打 `--eval-games` 局，胜率 ≥ `gating_accept_win_rate` 时 `shutil.copy2` 更新 `model_best.onnx`。

训练过程中维护两个模型文件：
- **latest** (`model_latest.onnx`)：每步训练后都重新导出，selfplay 立刻使用新权重，**永不被替换或回退**
- **best** (`model_best.onnx`)：独立文件，只在 gating 通过时从 latest 复制过来

Selfplay 始终使用 latest 模型。Gating 只影响 best 模型的保存。定期存档 `model_step_NNNNN.onnx` 按 `--save-every` 间隔保存，用于事后实验，不参与训练流程。

#### Warm Start 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `warm_start_episodes` | int | episodes_per_step | 预训练收集的局数 |
| `warm_start_epochs` | int | 5 | 预训练 SGD epoch 数 |
| `warm_start_heuristic` | bool | false | true 使用 heuristic 对局，false 使用随机对局 |
| `warm_start_temperature` | float | 3.0 | heuristic warm start 时的温度（越高越探索） |

#### Tail Solve 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `tail_solve_enabled` | bool | false | 启用残局求解 |
| `tail_solve_start_ply` | int | 40 | 最早尝试求解的步数 |
| `tail_solve_depth_limit` | int | 5 | alpha-beta 最大深度 |
| `tail_solve_node_budget` | int | 10000000 | 最大搜索节点数 |
| `tail_solve_margin_weight` | float | 0.0 | 终局评估加入分差系数（需配合 auxiliary_scorer） |

> **是否启用残局求解？** alpha-beta 在每个搜索节点调用 `legal_actions()`，因此开销 = `node_budget × legal_actions 单次耗时`。启用前评估你的游戏：
> - 分支因子 < 20 且 `legal_actions` 廉价（如 TicTacToe）→ 推荐启用
> - 分支因子 > 50 或 `legal_actions` 含 BFS/连通性检查（如 Quoridor ~130 分支，~35μs/次）→ 不推荐，200k 预算下单次求解耗时可达 800+ms，远超 800 次 MCTS simulation（~28ms）
>
> 要实测你的游戏的 tail solve 开销，可用 `dinoboard_engine.tail_solve(game_id, seed, perspective_player, depth_limit, node_budget)` 在 Python 中直接调用，读取 `elapsed_ms` 和 `nodes_searched`。

> **隐藏信息游戏的 tail solver 硬约束**：如果游戏同时注册了 `belief_tracker`（有非对称隐藏信息）和 `tail_solver`，必须：
> 1. override `do_action_deterministic`，保证其 NEVER 从隐藏源（deck / 袋子 / 对手手牌）抽取数据。如果 `do_action_fast` 的回合推进会翻随机牌，deterministic 版本要用占位或冻结逻辑（参考 Splendor 的 `forced_draw_override`）
> 2. 在 GameBundle 中显式设置 `stochastic_tail_solve_safe = true`，作为你已审阅过 `do_action_deterministic` 的声明
>
> `GameRegistry::create_game()` 在检测到 belief_tracker + tail_solver 而没有 `stochastic_tail_solve_safe = true` 时会抛异常。这是防止 tail solver 偷看真实状态污染训练数据（违反 "AI 链路不读取隐藏字段" 的核心设计原则）。

#### Heuristic Guidance 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `heuristic_guidance_steps` | int | 0 | 启发式引导的训练步数（0 表示禁用） |
| `heuristic_guidance_initial_ratio` | float | 0.5 | 初始概率（使用 heuristic 而非 MCTS） |
| `heuristic_temperature` | float | 0.0 | 启发式动作选择温度（0=从最高分中均匀选，>0=softmax 采样） |

在 `heuristic_guidance_steps` 步内，比例从 `initial_ratio` 线性衰减到 0。

`heuristic_temperature` 控制 selfplay 中启发式步骤的动作选择随机性。温度为 0 时在得分最高的动作中均匀随机选择；温度 > 0 时对分数做 softmax（`exp(score/T)`）采样。policy target 对应实际使用的概率分布。

#### Training Filter 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `training_filter_steps` | int | 0 | 动作过滤的训练步数（0 表示禁用） |
| `training_filter_initial_ratio` | float | 0.5 | 初始概率（应用 filter） |

在 `training_filter_steps` 步内，比例从 `initial_ratio` 线性衰减到 0。之后在全动作空间训练。

#### Peek 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `peek_steps` | int | 0 | 前 N 个训练步用 peek 模式（跳过 root 采样，MCTS 看真相），之后切回 ISMCTS。0 表示始终 ISMCTS |

Peek 模式下 `nopeek_enabled=False`，selfplay 调 `run_selfplay_episode` 时 belief_tracker 传 `nullptr`——MCTS 不调 `randomize_unseen`，直接在 truth state 上搜索。适合训练早期让 value head 先学到基本策略结构，再切到 ISMCTS 学习在信息不完全下决策。仅影响 selfplay，arena/eval 始终使用 ISMCTS。

#### Auxiliary Score 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `auxiliary_score` | bool | false | 启用辅助训练头 |
| `auxiliary_score_weight` | float | 0.5 | 辅助损失的权重 |

### 7.4 network 字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `hidden_layers` | int[] | `[256, 256]` | 隐层大小列表，如 `[256, 256, 256]` |

> 激活函数当前硬编码为 ReLU，网络结构固定为 MLP。如需其他结构/激活，修改 `training/model.py`。

> **Value head 维度**：自动由 `num_players`（从 C++ `game_metadata()` 获取）决定。输出 N 维 perspective-relative value，与 encoder 旋转对齐。无需在 config 中指定。

### 7.5 完整示例

<details>
<summary>TicTacToe（最小配置）</summary>

```json
{
  "game_id": "tictactoe",
  "display_name": "Tic-Tac-Toe",
  "players": {"min": 2, "max": 2},
  "action_space": 9,
  "feature_dim": 28,
  "training": {
    "simulations": 100,
    "c_puct": 1.4,
    "temperature": 1.0,
    "temperature_initial": 1.0,
    "temperature_final": 0.1,
    "temperature_decay_plies": 6,
    "dirichlet_alpha": 1.0,
    "dirichlet_epsilon": 0.25,
    "dirichlet_on_first_n_plies": 9,
    "max_game_plies": 9,
    "episodes_per_step": 200,
    "batch_size": 256,
    "learning_rate": 0.001,
    "steps": 500
  },
  "network": {
    "hidden_layers": [64, 64]
  }
}
```
</details>

<details>
<summary>Quoridor（完整配置）</summary>

```json
{
  "game_id": "quoridor",
  "display_name": "Quoridor",
  "players": {"min": 2, "max": 2},
  "action_space": 209,
  "feature_dim": 295,
  "training": {
    "simulations": 200,
    "c_puct": 1.4,
    "temperature_initial": 1.0,
    "temperature_final": 0.15,
    "temperature_decay_plies": 24,
    "dirichlet_alpha": 0.05,
    "dirichlet_epsilon": 0.1,
    "dirichlet_on_first_n_plies": 8,
    "max_game_plies": 200,
    "episodes_per_step": 100,
    "batch_size": 2048,
    "learning_rate": 0.001,
    "steps": 1500,
    "warm_start_episodes": 800,
    "warm_start_epochs": 10,
    "warm_start_heuristic": true,
    "warm_start_temperature": 3.0,
    "tail_solve_enabled": true,
    "tail_solve_start_ply": 30,
    "tail_solve_depth_limit": 10,
    "tail_solve_node_budget": 200000,
    "tail_solve_margin_weight": 0.01,
    "heuristic_guidance_steps": 500,
    "auxiliary_score": true,
    "auxiliary_score_weight": 0.5
  },
  "network": {
    "hidden_layers": [256, 256, 256]
  }
}
```
</details>

---

## 8. 构建集成

### 8.1 CMakeLists.txt

在 `games/<game>/` 下创建：

```cmake
add_library(game_<name> STATIC
    <name>_state.cpp
    <name>_rules.cpp
    <name>_net_adapter.cpp
    <name>_register.cpp
)
target_include_directories(game_<name> PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_<name> PUBLIC dinoboard_core)
```

然后在**顶层 `CMakeLists.txt`** 添加：

```cmake
add_subdirectory(games/<name>)
```

### 8.2 setup.py

在 `setup.py` 的 `sources` 列表中添加 4 个文件：

```python
sources = [
    # ... 已有文件 ...
    "games/<name>/<name>_state.cpp",
    "games/<name>/<name>_rules.cpp",
    "games/<name>/<name>_net_adapter.cpp",
    "games/<name>/<name>_register.cpp",
]
```

### 8.3 构建验证

```bash
# 构建 C++ 扩展
pip install -e .

# 验证注册成功
python -c "import dinoboard_engine; print(dinoboard_engine.available_games())"
# 应该输出包含你的 game_id 的列表
```

---

## 9. 训练可选特性

### 9.1 HeuristicPicker — 启发式策略

**用途**：
1. Warm start 预训练：在 MCTS 训练前先用 heuristic 对局初始化网络权重
2. Heuristic guidance：训练前期以一定概率使用 heuristic 代替 MCTS 选动作，在网络太弱无法有效搜索时推动游戏前进。启发式步骤生成训练样本（policy target 为 uniform over max-score actions）
3. Eval benchmark：评估模型 vs heuristic 的胜率

**签名**：`(IGameState&, const IGameRules&, uint64_t rng_seed) -> HeuristicResult`

**开发者只需给动作打分**。框架负责根据分数选择动作：
- Selfplay guidance：由 `heuristic_temperature` 控制。温度=0 时走贪心 argmax；温度>0 时对分数做 softmax 采样（`exp(score/T)`）
- Warm start：由 `warm_start_temperature` 独立控制。对分数做 softmax 采样（高温→更多探索）
- Eval benchmark：**与 selfplay 使用同一套"分数 + 温度"选择规则**，温度同样取自 `heuristic_temperature`（通过 `run_constrained_eval_vs_heuristic` 的 `heuristic_temperature` 参数传入；训练 pipeline 会自动从 `game.json` 读取。温度=0 即贪心 argmax）

不需要在 heuristic 内部实现随机逻辑或 tiebreaking。启发式步骤也生成训练样本，policy target 对应实际使用的概率分布。

> **注意**：eval 与 selfplay 必须共用同一套启发式选择规则（共享 `sample_heuristic_index`）。如果 eval 里退化成"只取第一个最大值"，对打分存在并列的游戏（如 Quoridor 的走子 vs 有效放墙都是 +1）会让启发式行为和训练时完全不同，win rate 毫无参考意义。

```cpp
HeuristicResult heuristic_pick(IGameState& state, const IGameRules& rules,
                                std::uint64_t /*rng_seed*/) {
  auto legal = rules.legal_actions(state);
  HeuristicResult result;
  result.actions = legal;
  result.scores.resize(legal.size());

  for (size_t i = 0; i < legal.size(); ++i) {
    auto clone = state.clone_state();
    rules.do_action_fast(*clone, legal[i]);
    result.scores[i] = evaluate_position(*clone);
  }

  return result;
}
```

### 9.2 ITailSolver — 残局求解器

**用途**：在 MCTS 搜索之前，尝试用 alpha-beta 精确求解。如果证明了必赢（proven win），直接使用求解结果而非 MCTS。使用 paranoid 假设（所有对手联合针对当前玩家），支持任意玩家数。仅在 proven win 时替代 MCTS——proven loss 在多人场景下过于悲观（对手之间有利益冲突），不作为放弃搜索的依据。

**触发条件**：通过 `tail_solve_trigger` 回调决定是否尝试求解。未注册时 fallback 到 `ply >= tail_solve_start_ply`。

**签名**：`(const IGameState& state, int ply) -> bool`

同时接收游戏状态和轮数（因为有些游戏轮数没有编码进状态）。返回 `true` 表示应该尝试求解。

**注册方式**：
```cpp
b.tail_solver = std::make_unique<board_ai::search::AlphaBetaTailSolver>();
b.tail_solve_trigger = [](const board_ai::IGameState& state, int ply) -> bool {
  // 根据游戏局势判断是否值得尝试求解
  ...
};
```

**示例**（Splendor — 有人接近胜利时尝试）：
```cpp
b.tail_solve_trigger = [](const board_ai::IGameState& state, int ply) -> bool {
  if (ply < 40) return false;
  const auto& s = board_ai::checked_cast<SplendorState<NPlayers>>(state);
  const auto& d = s.persistent.data();
  for (int p = 0; p < NPlayers; ++p) {
    if (d.player_points[static_cast<size_t>(p)] >= 12) return true;
  }
  return false;
};
```

**示例**（Quoridor — 有人接近目标时尝试）：
```cpp
b.tail_solve_trigger = [](const board_ai::IGameState& state, int ply) -> bool {
  if (ply < 20) return false;
  const auto& qs = board_ai::checked_cast<QuoridorState>(state);
  const int d0 = QuoridorRules::shortest_path_distance(qs, 0);
  const int d1 = QuoridorRules::shortest_path_distance(qs, 1);
  return d0 <= 4 || d1 <= 4;
};
```

**设计建议**：
- 用 `ply` 下限避免在开局/中局浪费预算（搜索树太大必然超 budget）
- 用游戏局势判断"可能在几步内结束"再尝试求解
- 训练日志中的 tail solve 统计可以帮助调参：成功率过低说明触发太激进，成功率接近 100% 说明可以适当放宽条件

**训练日志格式**：`tail_solve=X%(N/M, Yms)` 和 `ts=proven/completed/attempts`
- `X%`：成功率（proven / attempts）
- `N/M`：成功数 / 尝试数
- `Yms`：平均求解耗时
- `ts=proven/completed/attempts`：每步的求解统计（proven 是证明必赢/必输的次数，completed 是在预算内完成搜索的次数，attempts 是总尝试次数）

**采用条件**：求解返回的 `|value| >= 1.0`。只有证明了必赢/必输才替换 MCTS 结果；平局或搜索不完整（budget 耗尽）不会采用。

需要在 game.json 中启用：
```json
"tail_solve_enabled": true,
"tail_solve_start_ply": 30,
"tail_solve_depth_limit": 10,
"tail_solve_node_budget": 200000,
"tail_solve_margin_weight": 0.01
```

`tail_solve_start_ply` 仅在未注册 `tail_solve_trigger` 时作为 fallback 使用。

**分差偏好**：设置 `tail_solve_margin_weight` 可让 tail solver 在多条必赢路线中选择"赢得最多"的。终局节点的评估公式变为：

```
value = terminal_value + margin_weight × auxiliary_scorer(state, perspective)
```

`margin_weight` 需保证 `margin_weight × max(|scorer_value|) < 1.0`，否则平局可能被误判为胜利（采用条件为 `|value| >= 1.0`）。`auxiliary_scorer` 接口无返回值限制，如果 scorer 无界需相应减小 weight。需要同时注册 `auxiliary_scorer`。

**注意**：对于随机游戏，确保实现了 `do_action_deterministic()`，否则 tail solver 会包含随机分支导致结果不准确。

### 9.3 TrainingActionFilter — 训练动作过滤

**用途**：在训练时缩小动作空间，去除明显不好的动作，加速早期学习。

**签名**：`(IGameState&, const IGameRules&, const vector<ActionId>&) -> vector<ActionId>`

**概率应用**：filter 不是永久生效的。通过 `training_filter_steps` 配置，filter 的应用概率从 `training_filter_initial_ratio`（默认 0.5）线性衰减到 0。每个 ply 独立掷骰决定是否使用 filter。这确保网络最终在全动作空间上训练，避免泛化问题。

```json
"training_filter_steps": 800,
"training_filter_initial_ratio": 1.0
```

**注意**：
- filter 接收**可变**的 `IGameState&`（可以 do/undo 来评估动作质量）
- 如果 filter 返回空 vector，框架会 **fallback 到完整合法动作集**
- filter 同时影响 selfplay MCTS 和 constrained eval
- 详见 [BUG-003](KNOWN_ISSUES.md#bug-003-训练-评估动作空间不一致) 关于评估一致性的讨论

**legal_mask 与 filter 的关系**：filter 只影响 MCTS 搜索和动作选择的范围，训练样本的 `legal_mask` 始终使用完整合法动作集。被过滤的动作在 policy target 中 visits 为 0，通过 cross entropy 梯度，模型学到这些动作概率应该为 0。如果 `legal_mask` 也被 filter 缩小，模型不会收到任何关于被过滤动作的梯度信号，导致这些动作的 logit 保持随机值——在 free 模式下模型会错误地选择它们（见 [BUG-016](KNOWN_ISSUES.md#bug-016-legal-mask-被-filter-缩小导致-free-模式失效)）。

### 9.4 AuxiliaryScorer — 辅助训练信号

**用途**：提供 win/loss 之外的额外训练目标。网络会多出一个 score head 来预测这个分数。

**签名**：`(const IGameState&, int player) -> float`

建议返回值在 [-1, 1] 范围内（网络使用 tanh 输出）。

**示例**（Quoridor 的位置优势分）：
```cpp
b.auxiliary_scorer = [](const IGameState& state, int player) -> float {
  const auto& qs = checked_cast<QuoridorState>(state);
  int me = QuoridorRules::shortest_path_distance(qs, player);
  int opp = QuoridorRules::shortest_path_distance(qs, 1 - player);
  return std::tanh(static_cast<float>(opp - me) / 8.0f);
};
```

### 9.5 GameAdjudicator — 超时判定

**用途**：当游戏达到 `max_game_plies` 但未终局时，判定胜负。

**签名**：`(const IGameState&) -> int`

- 返回赢家索引（0-based）
- 返回 -1 表示平局

### 9.6 EpisodeStatsExtractor — 每局统计

**用途**：从完成的对局中提取自定义指标，汇总后在训练日志中显示。

**签名**：`(const IGameState&, const vector<SelfplaySampleView>&) -> map<string, double>`

```cpp
b.episode_stats_extractor = [](const IGameState&,
    const std::vector<SelfplaySampleView>& samples) -> std::map<std::string, double> {
  return {{"turns", static_cast<double>(samples.size()) / 2.0}};
};
```

这会在训练日志中产生 `turns=7.4` 这样的条目。

### 9.7 Peek 模式 — 全知搜索训练增强

**用途**：训练早期用全知状态搜索（忽略随机性和隐藏信息），帮助网络先学稳基本策略，后续切换到正式搜索模式。类似启发式引导的训练辅助手段。

**仅在训练 selfplay 中使用，实战 AI 不使用。** 对局时 AI 必须通过正式 ISMCTS 搜索。

**适用场景**：随机性或隐藏信息较复杂的游戏（如 Splendor），训练初期网络太弱、正式搜索噪声过大时，用 Peek 模式收集一批高质量样本快速建立基本策略。

**配置**：通过 `game.json` 的 `training.peek_steps` 控制。前 `peek_steps` 个训练步使用 peek（selfplay 跳过 root 采样，MCTS 看 truth），之后自动切回 ISMCTS。默认 0（始终 ISMCTS）。Arena 和 eval 始终使用 ISMCTS，不受此参数影响。

```json
{
  "training": {
    "peek_steps": 500
  }
}
```

---

## 10. 物理随机性

物理随机性指翻牌、抽卡、掷骰等改变游戏状态的随机事件。确定性游戏（TicTacToe、Quoridor）不涉及此节。

**ISMCTS-v2 下的处理**：物理随机和信息不对称被**统一**——不再有 chance node 专门机制（NoPeek / traversal_limiter / stochastic_detector 全部删除）。关键点：

- Root 采样通过 `belief_tracker->randomize_unseen(sim_state, rng)` 一次性固定当前 sim 的"全部未来随机"（deck 顺序、未来翻牌结果等）
- Descent 里 `do_action_fast` 照常从状态中读取随机源（如 `d.deck.top()` 或 `splitmix64(d.draw_nonce)`），每次 sim 拿到的值由 root 采样决定
- 观察者可见的后果（Splendor 翻新卡到 tableau）自然通过 `hash_public_fields` 差异分化到不同 DAG 节点
- 观察者不可见的后果（opp 抽牌）在 observer 视角 hash 下被合并

**开发者要做的**（对有物理随机的游戏）：

1. 在 `IGameState` 中实现 `rng_nonce()`（默认实现 `return draw_nonce` 等）
2. `do_action_fast` 里用 `splitmix64(state.draw_nonce)` 或类似 PRNG 驱动随机抽取。保持 state 里的 deck 等随机源字段明确
3. 实现 `IBeliefTracker::randomize_unseen(state, rng)` —— 把 state 里的隐藏字段（deck 内容 + 对手 hidden）按 belief 一次性采样填入
4. **不要** 调用任何 `stochastic_detector` 相关 API——这些都已删除

对**对称物理随机但无非对称 hidden info** 的游戏（如 Azul）：仍然要注册 `belief_tracker`，但 `hash_private_fields` 可空。`randomize_unseen` 只洗袋子。

---

## 11. 隐藏信息与 Belief Tracker

> **算法深入**：DAG 节点共享、UCT2、完整 search_root 流程、debug 指标——独立文档 [`docs/MCTS_ALGORITHM.md`](MCTS_ALGORITHM.md)。本节讲开发者接口。

隐藏信息指玩家间的**非对称**信息——某个玩家知道、其他玩家不知道的游戏状态。如 Splendor 的盲压暗牌（执行者知道是什么牌，对手不知道）。

对称无知的随机（如 Azul 袋子未来抽取顺序）**也**走 belief_tracker 通道——`hash_private_fields` 可以为空，`randomize_unseen` 负责洗袋子。见 §10。

### 11.1 架构原理（ISMCTS-v2）

核心三层机制：

1. **Root 采样 determinization**：每次 MCTS simulation 开头调 `belief_tracker->randomize_unseen(sim_state, rng)`，一次性把所有 hidden 字段（opp 手牌、deck 顺序、未来随机结果）采样成具体值。之后 descent **完全 deterministic**——动作按规则读 state 的采样值，不存在"chance 点重新抽"
2. **Per-acting-player DAG keying**：每决策节点用 `state.state_hash_for_perspective(state.current_player())` 当 key。全局 `unordered_map<StateHash64, int>` 表让不同路径到达同一信息集共享节点
3. **UCT2 UCB**：DAG 下多父路径汇聚到同一节点时，`sqrt()` 分子底用刚经过的入边的 visit_count，避免 node global count 夸大探索预算

**无 chance node 机制**——物理随机被 root 采样吞掉；observer-visible 后果通过 hash 自然分叉，observer-invisible 后果通过 hash 自然合并。

**新游戏开发者要做**：
- 覆盖 `IGameState::hash_public_fields` 和 `hash_private_fields(int player)`（§11.1b）——声明字段公开/私密归类
- 实现 `IBeliefTracker`（§11.2）——`init` / `observe_public_event` / `randomize_unseen`
- 实现 public-event protocol（`public_event_extractor` / `applier` / `initial_observation_extractor` / `applier`）
- 框架自动接管 root 采样时机、DAG 节点复用、UCT2 UCB。**不需要**在 rules 里做任何防御性 nonce bump 或 hidden-info guard

### 11.1b 声明式 hash API：hash_public_fields / hash_private_fields

每个 `IGameState` 子类实现两个虚方法：

```cpp
virtual void hash_public_fields(Hasher& h) const = 0;
virtual void hash_private_fields(int player, Hasher& h) const = 0;
```

框架派生出：

```cpp
StateHash64 state_hash_for_perspective(int player) const {
  Hasher h;
  h.add(step_count_);                 // DAG 结构性防环
  hash_public_fields(h);
  hash_private_fields(player, h);
  return h.finalize();
}
```

**字段归类**：
- **public**：所有玩家都能看见的字段。全部 hash 进去。弃牌堆、公开棋盘、分数、当前玩家、回合计数等
- **private for player p**：只有 p 能看见的字段。**只在 `hash_private_fields(p)` 里 hash**，不在 `hash_public_fields` 里重复 hash
- **完全隐藏（谁都看不见）**：deck 内容、set_aside、rng 种子——**任何地方都不 hash**。这些字段在不同 sim 的采样世界里会不同，但 DAG 节点不关心

**常见错误**：
- ❌ 把 `hand[p] for all p` 都 hash 进 public → opp hand 进了 public，不同采样世界分叉到不同节点，DAG 共享失效
- ❌ public 字段在 private 里又 hash 一遍 → hash 依赖 perspective，info set 边界混乱
- ✓ `hand[observer]` 只在 `hash_private_fields(observer)` 里 hash；对手的 hand 只在他们自己的 private hash 里

Love Letter 示例（`games/loveletter/loveletter_state.cpp` 实际代码）：

```cpp
void LoveLetterState<N>::hash_public_fields(Hasher& h) const {
  const auto& d = data;
  h.add(d.current_player);
  h.add(d.ply);
  h.add(d.winner + 11);
  h.add(d.terminal ? 1 : 0);
  for (int p = 0; p < N; ++p) {
    h.add(d.alive[p]);
    h.add(d.protected_flags[p]);
    h.add(d.hand_exposed[p]);
    for (auto c : d.discard_piles[p]) h.add(c);   // 弃牌堆公开
  }
  h.add(d.deck.size());                             // 大小公开，内容不是
  for (auto c : d.face_up_removed) h.add(c);       // 2p 规则的 3 张公开移除
}

void LoveLetterState<N>::hash_private_fields(int player, Hasher& h) const {
  const auto& d = data;
  if (player >= 0 && player < N) {
    h.add(d.hand[player]);                          // 只 hash 自己的手
    if (d.current_player == player && d.drawn_card) {
      h.add(d.drawn_card);                          // 自己回合的 drawn 也算
    }
  }
  // opp hand / set_aside / deck 内容 / draw_nonce：不 hash（任何地方都不 hash）
}
```

**Step counter**：`IGameState::step_count_` 由框架管理：
- `do_action_fast` 里调 `s->begin_step()`（`s` 在 template 类里用 `this->begin_step()`）
- `undo_action` 里调 `s->end_step()`
- `reset_with_seed` 里重置为 0

`state_hash_for_perspective` 自动把 step_count 计入 hash，保证 DAG 结构性 acyclic。**游戏开发者不要在 `hash_public_fields` 里重复 hash step_count**。

**测试建议**：构造两个 state，public + 同 perspective 的 private 完全相同，但 opp private 不同；assert `state_hash_for_perspective(perspective)` 相等。这是 hash 归类正确性的基本检查，`tests/test_encoder_respects_hash_scope.py` 提供模板。

### 11.2 IBeliefTracker

**用途**：维护当前玩家的信息认知，为 ISMCTS 根采样提供 prior。

3 个必须实现的方法（2026-05-04 起的观察-only 接口）：

```cpp
virtual void init(int perspective_player, const AnyMap& initial_observation) = 0;
virtual void observe_public_event(
    int actor, ActionId action,
    const std::vector<PublicEvent>& pre_events,
    const std::vector<PublicEvent>& post_events) = 0;
virtual void randomize_unseen(IGameState& state, std::mt19937& rng) const = 0;
```

**结构性约束（编译器层强制）**：`init` 和 `observe_public_event` 方法签名里没有 `IGameState*` —— tracker 在这两个方法里物理上拿不到 state 指针，**无法**偷看真实游戏状态。所有输入都来自游戏注册的两个 extractor：

- `init` 收 `initial_observation` → 来自 `bundle.initial_observation_extractor(state, perspective)`
- `observe_public_event` 收 `pre_events` / `post_events` → 来自 `bundle.public_event_extractor(before, action, after, perspective)`

两个 extractor 是小函数（~20-40 行），只读观察者可见字段，易于审计。

`randomize_unseen(state, rng)` 是 MCTS 采样的**写入口**——可以读 state 的公开字段 + 观察者自己的字段（discard_piles、自己的 hand 等），但禁止读 opp 的 hidden 字段（对手手牌、deck 内容）。tracker 从自己累积的 belief 构建 unseen pool 和 known-hand 分配。

**Belief tracker 不仅追踪公开信息，也可以追踪通过游戏技能合法获得的私有知识**。例如 Love Letter 中 Priest 偷看对手手牌、King 交换后知道对方原来的牌——这些通过 `hand_override` 事件传递到 tracker。`randomize_unseen` 时优先使用 tracker 中的确定知识（直接固定），没有确定知识的才从 unseen pool 中随机采样。这使得 ISMCTS 的采样质量更高——已知的不浪费预算重新猜。

**迁移说明**：`observe_action(before, action, after)` 旧接口已移除。原先从 state 读取的信息必须改为从事件流或 tracker 内部维护的状态读取。Love Letter tracker 内部维护 `own_hand_` / `own_drawn_card_` / `alive_tracked_[]` 等字段，通过事件增量更新。

参考 `games/splendor/splendor_net_adapter.h` 中的 `SplendorBeliefTracker` 实现。

### 11.3 Belief Tracker 生命周期

框架在所有代码路径（selfplay、arena、web GameSession、AI API）中自动管理以下生命周期，游戏开发者只需实现 `IBeliefTracker` 的三个方法：

```
游戏开始：
  initial_obs = bundle.initial_observation_extractor(state, perspective)
  belief_tracker->init(perspective, initial_obs)       // 观察-only init

每一步 ply:
  1. （可选）belief_tracker->init(cp, initial_obs_for_cp)  // actor 切换时幂等
  2. MCTS 搜索：每 sim 开头调 randomize_unseen          // ISMCTS 根采样
                  descent 纯 deterministic
  3. do_action_fast(state, chosen)                       // 执行动作
  4. evt = bundle.public_event_extractor(before, action, after, perspective)
     belief_tracker->observe_public_event(actor, chosen, evt.pre_events, evt.post_events)
```

`tracker_init` / `tracker_observe` 这类 helper 封装了 extractor 调用（见 `bindings/py_engine.cpp`）。游戏开发者只需实现 IBeliefTracker 的 3 个方法 + `initial_observation_extractor` + `public_event_extractor`。

### 11.4 实现示例

Belief tracker 有两种不同定位，由游戏的信息结构决定：

**定位一：维护随机来源**（Splendor）——状态无法推导出完整的"见过什么"历史，tracker 增量追踪 seen 信息，为 `randomize_unseen` 提供准确的 unseen pool。

**定位二：维护游戏中获取的精确知识**（Love Letter）——随机来源可从状态推导（弃牌堆是完整打出记录），不需要追踪；但游戏技能产生了精确信息（Priest 偷看、Baron 比较、King 交换），tracker 追踪"我确切知道对手拿什么"，`randomize_unseen` 直接固定已知手牌，encoder 向 tracker 查询后编码已知对手手牌（而非输出占位符）。

两种定位可以并存。Love Letter 是本框架中对"不完美信息 + 短对局 + 频繁信息交换"类游戏的探索——这类游戏的难点不在随机来源维护，而在通过 belief tracker + encoder 协作将游戏技能产生的精确知识正确传递给网络。

**Love Letter（游戏知识追踪——seen 从状态推导，known_hand 增量维护）**：
- `init(perspective, initial_obs)`：从 `initial_obs["my_hand"]` / `"my_drawn_card"` 读自己的起手牌，清空 `known_hand_`
- `observe_public_event(actor, action, pre_events, post_events)`：追踪 Priest（偷看 → 记录对手手牌，来自 `hand_override` 事件）、Baron 平局（互看 → 记录双方手牌）、King（交换 → 更新/转移知识）、Prince（重摸 → 清除知识）、淘汰（清除）、对手打出已知牌（清除）
- `randomize_unseen()`：seen = 自己手牌 + drawn_card + 所有弃牌堆 + face_up_removed + known_hand（从 unseen pool 扣除）；已知对手手牌直接固定，未知的随机采样
- encoder 持有 tracker 指针，编码对手手牌时查询 `tracker->known_hand(p)`：有值则编码真实牌，无值则输出全零占位符
- 随机来源与 Azul 同属"状态可推导"模式：弃牌堆是完整历史，不需要 `seen_cards` 集合

**Splendor（随机来源追踪——seen 需增量维护）**：
- 维护 `seen_cards` 集合，**绝不读取 `data.decks`**
- `init(perspective, initial_obs)`：从 `initial_obs["tableau"]` 初始化 seen（所有公开 tableau 卡）
- `observe_public_event`：处理 `deck_flip` 事件（新翻出来的卡加进 seen）和 `self_reserve_deck`（perspective 自己盲抽知道的牌）
- `randomize_unseen()`：用 `全卡池 - seen_cards` 构建 unseen pool，按 tier shuffle 后回填 deck 和对手暗牌
- 需要增量追踪的原因：牌被买走后离开 tableau，仅从当前状态无法知道历史上哪些牌曾经可见

**Coup（自定义概率化 sampling 的范例）**：

Coup 是**诈唬核心**游戏——公开声明（claim）和真实持牌可以不一致。如果用 uniform 采样，对手被建模成"随机机器人"，MCTS 会疯狂质疑，训练收敛到"永不质疑永不诈唬"的退化均衡。解决方案是在 `randomize_unseen` 里**手写启发式加权采样**，从 claim / challenge 历史推断对手可能持有的角色。

- `init(perspective, initial_obs)`：清空 `signals_[player][role]` 矩阵（5 个角色 × N 玩家）
- `observe_public_event`：增量维护 `signals_[p][R]`
  - **claim R 无人质疑**：`signals_[claimer][R] += 1`（弱正信号）
  - **claim R 被质疑 + 为真**（证实后洗回 deck）：`signals_[claimer][R] = 0`（那张牌已经不在手上）
  - **claim R 被质疑 + 为假**（bluff 戳穿）：`signals_[claimer][R] = 0`（原始 claim 是假的）
  - **玩家 q 质疑某人 claim R**：`signals_[q][R] += 1`（q 敢质疑说明他自己可能有 R）
  - **任何玩家暴露一张 X**：`signals_[revealer][X] = 0`（X 已 revealed，剩余 slot 对 X 的先验回到 baseline）
  - **Ambassador 换牌**：`signals_[exchanger][*] = 0` 全清零
- `randomize_unseen()`：**带硬约束的加权联合采样**
  1. 收集所有 unseen slot：opp 未揭露 hand + opp exchange_drawn + court deck
  2. 随机 shuffle slot 顺序（避免 opp₀ 总是优先拿稀有角色）
  3. 对每个 slot，`weight[R] = remaining[R] × (1 + 0.5 × signals_[owner][R])`；`remaining[R]` 是**硬约束**——池里没了权重直接 0，保证全局每角色总数 ≤ 3
  4. 按 weight 采样一张，从 `remaining` 扣除
  5. 所有 weight 为 0 时 fallback 到 uniform 采样（保证永不陷入不可行）

**为什么 feasible 是关键**：N-玩家 Coup 的 unseen 字段包括 N 个 opp 各 1~2 张手牌 + deck 剩余（可达 ≥8 张），全都要**联合采样**满足总牌数守恒。独立 per-player 采样会出现"两个 opp 都被采样成 Duke×2，但 pool 里总共只有 3 张 Duke"的不可行情况。用"剩余池 × 启发式权重"的顺序采样自然 enforce 约束。

参考实现：`games/coup/coup_net_adapter.cpp` 的 `CoupBeliefTracker::randomize_unseen`。这套模式适用于任何**手牌可能不匹配公开声明**的游戏——把 claim/challenge 历史转成 per-opp role prior，在池约束下联合采样。比完整的概率化 belief network（未来 work）实现成本低一个数量级。

### 11.5 Encoder 信息屏障与 Tracker 协作

Feature encoder 必须在视角层强制信息隐藏。核心规则：**每个玩家只能看到自己通过合法途径获得的信息**。即使 `randomize_unseen` 已经把采样值写入 state，encoder 对非当前视角玩家的隐藏字段仍须输出占位符——除非 tracker 确认当前玩家合法知道该信息。

Encoder 的信息源有两个，地位等价：
- **state**：公开局面信息（棋盘、弃牌堆、存活状态等）
- **belief tracker**：通过 `observe_public_event` 积累的私有知识（Priest 偷看的手牌、Baron 比较的结果等）

Encoder 可以持有 tracker 指针，编码时查询已知信息。Love Letter 的实现中，encoder 向 tracker 查询 `known_hand(p)`，已知则编码真实牌，未知则输出占位符。这让网络直接获得游戏技能产生的信息，无需从弃牌历史自行推导。

以 Splendor 双人局为例，假设双方各有一张暗牌：

| 视角玩家 | 自己的暗牌 | 对手的暗牌 |
|---------|-----------|-----------|
| 我的节点 | 编码真实牌 | **占位符** |
| 对手节点 | 编码采样牌（来自 ISMCTS 根采样） | **占位符**（我的暗牌对手看不到） |

两个方向互为镜像——我看不到对手的牌，对手也看不到我的牌。ISMCTS 采样的值只在"自己看自己"时可见，不会泄露到对方的 encoding 里。

Splendor 实现（`splendor_net_adapter.cpp`）：

```cpp
const bool is_self = (pid == perspective_player);
if (!is_self) {
    const bool visible = d.reserved_visible[pid][slot] != 0;
    if (!visible) {
        encode_hidden_reserved_placeholder(features);  // 占位符
        continue;
    }
}
// is_self=true: 编码真实（或采样的）卡牌
```

这保证了 encoder 在训练和搜索中看到的信息结构完全一致——训练时每个玩家的 sample 也是从该玩家视角编码的，看不到对手隐藏信息。

### 11.6 框架限制

ISMCTS 根采样 + encoder 信息屏障在**双人游戏**中完全自洽。多人游戏（3+）存在精度局限：搜索中模拟第三方玩家时，无法建模该玩家对其他人的私有知识（如"B 用 Priest 看了 C 的牌"）。这是 determinization 方法的固有限制，不影响双人游戏。

### 11.7 开发者 Checklist

1. 确认游戏是否有非对称隐藏信息（玩家间知道的不一样）。对称无知（如 Azul 的 bag）不需要 `hash_private_fields` 但仍然需要 `belief_tracker` 来驱动 `randomize_unseen`
2. 实现 `IBeliefTracker` 的三个方法（`init` / `observe_public_event` / `randomize_unseen`），遵守"绝不读取隐藏字段"约束（§11.2）
3. Encoder 中对非自身玩家的隐藏信息输出占位符（§11.5），严格只读 `public + current player's private` 范围的字段
4. **实现 `hash_public_fields(Hasher&)` 和 `hash_private_fields(int player, Hasher&)`**（§11.1b）——声明式分离公开 / 玩家私有信息。框架用 `state_hash_for_perspective(p) = step_count + public + private(p)` 作 DAG 节点键，让信息集跨路径共享
5. **在 `do_action_fast` 里调 `state.begin_step()`，在 `undo_action` 里调 `state.end_step()`**，`reset_with_seed` 里重置 `this->step_count_ = 0`。`step_count_` 单调递增保证 DAG 结构性 acyclic
6. **实现 public-event protocol**（`extract_events` / `apply_event` / `extract_initial_observation` / `apply_initial_observation`）——框架用它在 `GameSessionWrapper` 里维护每个 perspective 的 ai_view，同时驱动外部 AI API 的观察流。详见 §17 事件协议章节（或直接参考 `games/loveletter/loveletter_register.cpp`）
7. 在 `make_<game>` 里注册 `belief_tracker` + `public_event_extractor` + `public_event_applier` + `initial_observation_extractor` + `initial_observation_applier`
8. **验证测试**：
   - `tests/test_ai_api_separation.py::test_full_game_via_api[<game>]` 必须过（API 契约）
   - `tests/test_api_belief_matches_selfplay.py::*[<game>]` 必须过（belief 等价）
   - `tests/test_encoder_respects_hash_scope.py` 必须过（encoder 不越界读 opp private）
   - 建议自己写黑盒统计测试（类似 `TestLoveLetterGuardAccuracy`）：在某个会读隐藏信息的决策点上，验证 AI 的选择分布和无先验情况下的基线一致
   - 建议加 hash 单元测试：两个相同 info set 的 state（公开字段 + 视角玩家 private 字段全同，其他玩家 private 可不同）的 `state_hash_for_perspective(p)` 必须相等

### 11.7a 为什么这么多 hook

ISMCTS-v2 让开发者**不需要在游戏规则里做任何防御性代码**（没有 `++nonce`、没有 hidden-info guard、没有 MCTS 特殊路径）。所有隐藏信息处理都在框架层，代价是开发者要把"观察者视角下能看到什么"精确表达出来——这是 hook 列表看起来长的原因。每个 hook 的职责都有清晰语义：

| Hook | 说什么 |
|------|-------|
| `hash_public_fields` | 哪些字段所有玩家都能看到（信息集的公共部分） |
| `hash_private_fields(p)` | 玩家 p 的私有字段（进入 p 作 acting player 的节点 key） |
| `initial_observation_extractor/applier` | 游戏开始时观察者看到什么 |
| `public_event_extractor/applier` | 一次动作后观察者的知识增量 |
| `belief_tracker` | 观察者基于历次观察累积的精确知识 + `randomize_unseen` |
| encoder 的 `is_self` 逻辑 | 观察者不应看到的字段如何 mask |

这六个加起来是"观察者视角"的完整规格。ISMCTS-v2 的行为完全从这个规格推导——相同的 framework code 处理所有游戏，不需要 per-game 的 MCTS 特判。

---

## 12. 特征编码最佳实践

### 12.1 视角编码

**始终从 `perspective_player` 的视角编码**。将 "我的" 和 "对手的" 分开，而不是 "玩家0的" 和 "玩家1的"。这让网络学到的策略对两个玩家通用。

```cpp
// 好：视角无关
features->push_back(cell_owner == perspective_player ? 1.0f : 0.0f);  // 是我的
features->push_back(cell_owner == opponent ? 1.0f : 0.0f);            // 是对手的

// 差：玩家绑定
features->push_back(cell_owner == 0 ? 1.0f : 0.0f);  // 是玩家0的
features->push_back(cell_owner == 1 ? 1.0f : 0.0f);  // 是玩家1的
```

### 12.2 不要旋转棋盘

棋盘对称游戏（如 Quoridor 两个玩家面对面）**不要**物理旋转棋盘。只做「我 / 对手」特征交换 + 一个方向 scalar，详见 4.2 节。旋转棋盘引入过的 bug 会让某一方的策略永远学不出来，不值得这个省事。

### 12.3 标量特征归一化

将标量特征归一化到 [0, 1] 或 [-1, 1]：
```cpp
// 好
features->push_back(static_cast<float>(walls_remaining) / kMaxWalls);
features->push_back(static_cast<float>(move_count) / kMaxPlies);

// 差
features->push_back(static_cast<float>(walls_remaining));  // 原始值 0-10
```

### 12.4 合法掩码

使用 `fill_legal_mask()` 辅助函数：

```cpp
fill_legal_mask(action_space(), legal_actions, legal_mask);
```

---

## 13. Web 前端开发

本章描述游戏开发者需要实现的部分。框架提供的通用功能见 [§14 Web 平台功能](#14-web-平台功能)。

### 13.1 StateSerializer — 状态序列化

**用途**：Web 前端通过 `/api/games/{session_id}` 获取游戏状态 JSON。

**签名**：`(const IGameState&) -> AnyMap`

`AnyMap` 是 `std::map<std::string, std::any>`。Python bindings 自动将 `std::any` 转为 Python 对象，支持的类型：

| C++ 类型 | Python 类型 |
|----------|-------------|
| `int` | `int` |
| `double` / `float` | `float` |
| `bool` | `bool` |
| `std::string` | `str` |
| `std::vector<int>` | `list[int]` |
| `std::vector<AnyMap>` | `list[dict]` |
| `AnyMap` | `dict` |

**示例**：
```cpp
AnyMap serialize_quoridor(const IGameState& state) {
  const auto& s = checked_cast<QuoridorState>(state);
  AnyMap m;
  m["current_player"] = std::any(s.current_player());
  m["board_size"] = std::any(static_cast<int>(kBoardSize));

  std::vector<AnyMap> pawns;
  for (int i = 0; i < kPlayers; ++i) {
    pawns.push_back({
        {"player", std::any(i)},
        {"row", std::any(static_cast<int>(s.pawn_row[i]))},
        {"col", std::any(static_cast<int>(s.pawn_col[i]))},
    });
  }
  m["pawns"] = std::any(pawns);
  return m;
}
```

### 13.2 ActionDescriptor — 动作描述

**用途**：Web 前端将 ActionId 翻译为人类可读的描述。

**签名**：`(ActionId) -> AnyMap`

```cpp
AnyMap describe_quoridor(ActionId action) {
  AnyMap m;
  m["action_id"] = std::any(static_cast<int>(action));
  if (is_move_action(action)) {
    m["type"] = std::any(std::string("move"));
    m["row"] = std::any(decode_move_row(action));
    m["col"] = std::any(decode_move_col(action));
  }
  // ...
  return m;
}
```

### 13.3 目录结构

```
games/<game>/web/
├── index.html    # 主页面
├── styles.css    # 样式
└── <game>.js     # 游戏逻辑
```

`platform/app.py` 会自动扫描 `games/*/web/` 并挂载到 `/games/<game>/`。

### 13.4 交互设计原则

> 完整的视觉与交互指引见 [WEB_DESIGN_PRINCIPLES.md](WEB_DESIGN_PRINCIPLES.md)。本节只列实现相关要点。

**自然交互——不要给每个动作一个按钮**。动作空间可能有几百个，逐一列出既不美观也不可操作。按照物理游戏的交互方式设计 UI：

| 游戏动作类型 | 推荐交互方式 | 说明 |
|-------------|-------------|------|
| 移动棋子 | 点击棋子 → 高亮可到达位置 → 点击目标 | 两步点击 |
| 放置墙/棋子 | 悬停预览 → 点击确认 | 鼠标跟随 |
| 选择资源 | 点击资源池 → 点击目标位置 | 拖放或两步点击 |
| 组合动作 | 分步骤引导，每步缩小选择范围 | 层级选择 |

游戏 JS 负责将手势翻译为 `ActionId`，通过 `ctx.submitAction(actionId)` 提交。玩家不需要看到或理解 ActionId 编码。

**空间锚定——固定区域不动，只有内容变化**。游戏中固定存在的容器（棋盘格、工厂盘、中心区、玩家面板、牌库位置）必须有固定的屏幕位置和尺寸，不随内容数量变化而移动、缩放或重排。只有容器内部的元素（棋子、牌、token）可以出现、消失、移动。

玩家靠空间记忆快速定位信息——"左下角是我的图案线，右上角是公共牌库"。如果容器位置随内容增减而漂移（比如 Azul 中心区的 token 被拿走后区域收缩，导致旁边的工厂盘位移），玩家每步都要重新扫描整个画面，严重影响可玩性。物理桌游天然满足这个约束（棋盘不会自己挪位置），前端实现时要显式保持这一点。

实践要点：
- 用固定尺寸的容器（`width`/`height` 写死或 `min-width`/`min-height`），不用 `fit-content`
- 元素减少时容器留白，不收缩；元素增加时内部滚动或缩放，容器不撑大
- 避免对容器级元素使用 `flexbox` 的 `gap` + 自动换行——内容变化会改变行数，推动后续容器位移

### 13.5 createApp(config) — 通用框架 API

游戏前端的入口是调用 `createApp(config)`（从 `general/app.js` 导入）。框架处理所有通用逻辑（开局、AI 对弈、悔棋、提示、录像），游戏只需提供渲染和格式化函数。

#### config 对象字段

| 字段 | 类型 | 必须 | 说明 |
|------|------|------|------|
| `gameId` | string | 是 | 游戏 ID，匹配 game_registry 注册的 id |
| `numPlayers` | int | 否 | 玩家数（默认 2） |
| `renderBoard` | `(container, gameState, ctx)` | 是 | 渲染公共游戏区域（棋盘/牌桌） |
| `renderPlayerArea` | `(container, gameState, ctx)` | 是 | 渲染玩家私有区域 |
| `formatOpponentMove` | `(actionInfo, actionId) -> string` | 否 | 格式化对手上一步的文字描述 |
| `formatSuggestedMove` | `(actionInfo, actionId) -> string` | 否 | 格式化 AI 提示推荐动作的文字描述 |
| `getPlayerSymbol` | `(aiPlayer) -> string` | 否 | 返回玩家身份描述（默认"先手"/"后手"） |
| `extensions` | `(gameState) -> [{label, value}]` | 否 | 信息栏扩展内容（如"牌堆剩余"） |
| `onGameStart` | `() -> void` | 否 | 开局回调（可用于清理 UI 状态） |
| `onActionSubmitted` | `() -> void` | 否 | 玩家提交动作后回调 |
| `onUndo` | `() -> void` | 否 | 悔棋后回调 |

#### ctx 对象（传给 renderBoard/renderPlayerArea）

| 方法/属性 | 说明 |
|-----------|------|
| `ctx.canPlay` | 当前是否允许人类操作（综合判断：非终局、非 busy、轮到人类或替对手模式） |
| `ctx.state` | 当前 app 状态（含 `aiPlayer`、`busy`、`forceMode` 等） |
| `ctx.submitAction(actionId)` | 提交玩家动作。游戏将点击/拖拽手势翻译为 actionId 后调用此方法 |
| `ctx.rerender()` | 强制重新渲染（用于游戏内部状态变化后触发更新） |

#### gameState 对象（来自后端 state_serializer）

后端 `session_response()` 返回的对象，包含：
- `current_player`：当前玩家
- `is_terminal`：是否终局
- `winner`：胜者（-1 为平局或未终局）
- `legal_actions`：合法动作 ID 列表
- `last_action_id`：上一步动作 ID
- `last_action_info`：上一步 `action_descriptor` 返回的信息
- `difficulty`：当前难度
- 游戏自定义字段（由 `state_serializer` 返回的所有 key-value）

#### 典型游戏 JS 结构

```javascript
import { createApp } from '/static/general/app.js';

createApp({
  gameId: 'mygame',
  numPlayers: 2,

  renderBoard(container, gameState, ctx) {
    if (!gameState) { window.DinoBoard.showNotStarted(); return; }
    window.DinoBoard.hideNotStarted();
    // 根据 gameState 渲染棋盘到 container
    // 用户交互后调用 ctx.submitAction(actionId)
  },

  renderPlayerArea(container, gameState, ctx) {
    // 渲染玩家手牌/个人区域
  },

  formatOpponentMove(actionInfo, actionId) {
    return `对手${actionInfo.type === 'move' ? '移动到' : '放置墙于'} (${actionInfo.row},${actionInfo.col})`;
  },

  formatSuggestedMove(actionInfo, actionId) {
    return `建议${actionInfo.type === 'move' ? '移动到' : '放墙于'} (${actionInfo.row},${actionInfo.col})`;
  },
});
```

### 13.6 参考实现

- **简单参考**：`games/tictactoe/web/`（9 格棋盘，最简交互）
- **复杂参考**：`games/quoridor/web/`（9×9 棋盘 + 墙放置 + 棋子跳跃）

---

## 14. Web 平台功能

本章描述框架提供的通用功能，游戏开发者了解即可，无需额外代码。

### 14.1 通用布局

general 层提供统一的页面布局，游戏前端只需填充内容区域：

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

- **上方左侧**：`renderBoard()` 渲染的公共区域（棋盘、牌桌、公共资源等）
- **上方右侧**：general 层自动管理的信息栏（上半）和录像窗口（下半）。信息栏包含回合指示、对手上一步描述（`formatOpponentMove`）、AI 胜率/提示。录像窗口在对局中隐藏，结束后显示
- **下方**：`renderPlayerArea()` 渲染的玩家私有区域。2 人游戏左右分列，3-4 人游戏自动切换为网格排列

### 14.2 高级操作

general 层统一实现以下操作，游戏前端**不需要额外代码**：

| 操作 | API | 说明 |
|------|-----|------|
| **悔棋** | `POST /step-back` | 回到人类上一串连续行动的起点。循环调用 step-back，跳过 AI 回应和人类的连续回合（通过 `last_actor` 判断）。Splendor 拿币→退币退到拿币处；Azul 跨轮连续行动退到该串的第一步。本质上不区分子动作和连续回合，统一按 `last_actor` 处理 |
| **替对手落子** | `ctx.state.forceMode` | 回退到目标 AI 玩家的一串连续行动起点，由人类替其选动作。同样通过 `last_actor` 跳过连续回合。多人游戏侧边栏有每个 AI 的独立按钮，可指定替哪个 AI 落子 |
| **智能提示** | `POST /ai-hint` | AI 推荐最佳动作和胜率，不落子。`formatSuggestedMove()` 格式化显示 |

### 14.3 AI Pipeline 与动作分析

Pipeline 协调对局中的 AI 决策和分析。核心设计：**每个 human-to-play 局面只跑一次 MCTS（precompute），结果同时用于"智能提示"和"人类走棋后的掉分分析"**。

#### 每个 human→AI 循环的 MCTS 运行

| 运行 | 触发时机 | 用途 |
|------|---------|------|
| **Precompute** | AI 落子完成后（或游戏开局 / 悔棋后）立刻启动，用 `analysis_simulations` 次模拟在**当前 human-to-play 局面**上跑 MCTS | 一份结果两用：① 人类点"智能提示"时直接返回 ② 人类落子后读取缓存的 `action_values[chosen_action]` 算掉分 |
| **AI decision** | 人类落子后进入 AI 回合，用 `simulations` 次模拟在 AI-to-play 局面上跑 MCTS | AI 决定下一步 |

**不存在"分析用户上一步"和"下一轮提示"两次独立搜索**——同一个 precompute 结果在两个时刻被消费。

#### Pipeline 阶段机

```
[AI 落子完成] → [done] → 立即启动 precompute（后台）
  │
  ▼ 人类走棋
  │
[analyzing] —— 纯缓存读取：等待当前 precompute 完成（如果还没好），从其
               action_values 中取出人类所选动作的 Q 值，计算掉分
  │
  ▼
[ai_thinking] —— AI 的 MCTS 决策
  │
  ▼
[done] —— 触发下一轮 precompute，循环继续
```

**关键**：`[analyzing]` 阶段只是等 precompute 的结果 + 读缓存，**不触发新的 MCTS**。只有当 precompute 异常（被取消、8 秒超时）才有兜底的 inline MCTS。

非专家难度跳过 analyzing 阶段和 precompute，只做 AI 思考落子。

#### 智能提示（ai-hint）

人类回合点击"智能提示"按钮时：

1. 前端立即在信息栏显示"局面分析中..."
2. `POST /ai-hint` 后端：
   - 如果 precompute 结果已就绪 → 立即返回
   - 如果仍在跑 → 阻塞等待至多 8 秒后返回
3. 前端更新信息栏为提示结果

用户看起来是无缝的："分析没好时显示分析中，好了就显示结果"。后端用的仍然是同一份 precompute，不产生额外 MCTS。

前端通过轮询 `GET /pipeline` 获取当前阶段（`analyzing` / `ai_thinking` / `done` / `error`）。pipeline 在后台线程执行 MCTS，GIL 已通过 `py::gil_scoped_release` 释放，不阻塞前端请求。

#### 动作分析（掉分检测）

分析基于 precompute 的 MCTS 结果。搜索树的每个根边记录 N 维 Q 值（`action_values: {action_id: [p0_q, p1_q, ...]}`），是该边所有叶子的 per-player value 均值。

**计算公式**：
- `best_wr = (root_values[human_player] + 1) / 2`：最优动作对应的人类胜率
- `actual_wr = (action_values[chosen_action][human_player] + 1) / 2`：用户实际选择的动作对应的人类胜率
- `drop = (best_wr - actual_wr) * 100`：掉分百分比

一次搜索同时得到所有合法动作的 Q 值，直接读取 `human_player` 维度即可，支持任意人数。

**掉分阈值**：≥5% 标记为失误（warn），≥10% 标记为严重失误（blunder）。

#### 高级操作与 pipeline 的交互

| 操作 | 与 pipeline 的关系 |
|------|-------------------|
| **悔棋** (`POST /step-back`) | 先 `cancel_pipeline` 终止正在进行的搜索，回退动作历史，重建 GameSession，然后重新触发 precompute |
| **智能提示** (`POST /ai-hint`) | 优先复用 precompute 已有的搜索结果（等待最多 8s）；超时或无结果则临时跑一次独立 MCTS。不触发 pipeline，不落子 |
| **替对手落子** (`POST /action` with forceMode) | 和正常用户走棋相同流程，触发 pipeline |

**实现文件**：`platform/game_service/pipeline.py`（调度和分析）、`platform/game_service/routes.py`（API 端点）。

#### 交互规范

所有用户操作遵循"先渲染、后计算"——用户点击后立即看到画面更新，后台任务异步执行。

**用户走棋**：
1. cancel 旧 pipeline，立即 apply 并渲染新画面
2. 若 precompute 分析已完成：显示掉分分析，然后触发 AI 思考
3. 若 precompute 分析未完成：等分析完成后显示掉分，再触发 AI 思考
4. AI 思考完成后渲染 AI 落子，触发下一轮 precompute

**悔棋**：
1. cancel 当前 pipeline
2. 循环 step-back：跳过 AI 回应和人类的连续行动（`last_actor` 匹配），直到回到人类一串行动的真正起点
3. 立即渲染正确画面
4. 立即触发 precompute 分析新局面

**替对手落子**：
1. cancel 当前 pipeline
2. 循环 step-back：跳过连续行动（`last_actor` 匹配），直到到达目标 AI 玩家一串行动的起点
3. 立即渲染，等用户替对手落子后，按正常"用户走棋"流程处理

**智能提示**：
- 专家难度且 precompute 已完成：直接显示分析结果
- 专家难度但 precompute 未完成：显示"局面分析中"，完成后自动更新
- 非专家难度：显示"局面分析中"，临时跑分析，完成后显示
- 提示过程中用户仍可正常落子（提示不阻塞交互）

### 14.4 录像回放

仅**专家难度**可用。对局结束后自动进入回放模式。

**功能**：
- 每帧附带掉分分析（见 §14.3）
- general 层提供回放控制（前进/后退/跳到下一个失误）
- 游戏前端只需确保 `renderBoard()` 能渲染任意帧的状态（通过 `GET /replay` 获取帧列表）

**开发者无需额外代码**：只要 `state_serializer` 和 `renderBoard()` 正确实现，录像功能自动可用。

### 14.5 统一录像格式

在线对局和测试脚本使用统一的 JSON 格式：

```json
{
  "game_id": "quoridor",
  "seed": 42,
  "players": {
    "player_0": {"name": "latest_step500", "type": "model"},
    "player_1": {"name": "heuristic_t0.2", "type": "heuristic"}
  },
  "result": {"winner": 0, "draw": false, "total_plies": 75},
  "config": { ... },
  "action_history": [42, 130, 5, ...],
  "frames": [...]
}
```

- **actor** 统一为 `"player_0"` / `"player_1"` / ... / `"start"`，通过 `players` 字典标注名字和类型（human/model/heuristic/ai）
- **frames** 可选：在线对局包含完整帧（带 analysis），测试录像仅含 `action_history`，前端加载时通过 `POST /api/replay/build` 自动回放生成帧
- 录像核心模块：`platform/game_service/replay.py`（`make_replay_frame`、`build_frames_from_actions`、`build_replay_dict`）

### 14.6 模型评估工具

`platform/tools/eval_model.py` — 独立评估训练成果的脚本，支持并行对局和胜率统计：

```bash
# 快速评估：40 局 4 worker 并行，只看胜率（不保存录像）
python3 platform/tools/eval_model.py \
  --game quoridor \
  --model-a runs/quoridor_v14/models/model_best.onnx \
  --sims 400 --games 40 --workers 4 --no-save -o /tmp/eval

# model vs heuristic，保存录像供前端回放
python3 platform/tools/eval_model.py \
  --game quoridor \
  --model-a runs/quoridor_v14/models/model_latest.onnx --name-a latest \
  --heuristic-temp 0.2 --sims 800 --games 20 \
  -o games/quoridor/replay/latest_vs_heuristic

# model vs model
python3 platform/tools/eval_model.py \
  --game quoridor \
  --model-a models/step500.onnx --name-a step500 \
  --model-b models/warm.onnx --name-b warm \
  --temp 0.1 --games 20 --workers 4 \
  -o games/quoridor/replay/step500_vs_warm

# constrained 模式（动作过滤）
python3 platform/tools/eval_model.py \
  --game quoridor --constrained \
  --model-a models/latest.onnx --name-a latest \
  --games 10 -o games/quoridor/replay/constrained_test
```

参数说明：`--workers N` 并行跑 N 局，`--no-save` 只输出统计不写文件。自动交替先后手，输出按先后手分别统计胜率。录像为轻量 JSON（仅含 action_history），前端加载时自动回放生成帧。

### 14.7 通用功能（common.js）

`common.js` 提供以下自动注入的通用功能，所有游戏前端自动获得：

| 功能 | 说明 |
|------|------|
| 缩放控件 | 左上角 +/- 按钮，缩放棋盘区域，状态保存到 localStorage |
| 侧边栏收起 | 侧边栏边缘"收起/展开"按钮，状态保存到 localStorage |
| 尚未开局提示 | 棋盘区域显示"尚未开局"占位文字 |

**尚未开局提示**：`common.js` 在 `#board-stage` 中自动注入 `#not-started-placeholder`。游戏 JS 通过以下 API 控制显隐：

```javascript
// 游戏开始后隐藏
window.DinoBoard.hideNotStarted();

// 需要重新显示时（如重置）
window.DinoBoard.showNotStarted();
```

游戏的 `renderBoard()` 函数应在 `gameState` 为空时调用 `showNotStarted()`，在有状态时调用 `hideNotStarted()`。

### 14.8 核心 API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/games/available` | GET | 列出已注册游戏（ID、显示名、玩家数、是否有 web） |
| `/api/games` | POST | 创建新游戏，参数：`game_id`, `seed`, `human_player`, `difficulty` |
| `/api/games/{id}` | GET | 获取当前状态（调用 `state_serializer`） |
| `/api/games/{id}/action` | POST | 执行玩家动作，参数：`action_id` |
| `/api/games/{id}/ai-action` | POST | 触发 AI 落子（AI 先手时用） |
| `/api/games/{id}/pipeline` | GET | 轮询 AI 响应状态 |
| `/api/games/{id}/ai-hint` | POST | 获取 AI 推荐动作（不落子） |
| `/api/games/{id}/step-back` | POST | 悔棋 |
| `/api/games/{id}/replay` | GET | 获取完整录像帧（含 players 信息） |
| `/api/replay/build` | POST | 从 action_history 回放生成帧（参数：game_id, seed, action_history） |
| `/api/replay/file` | GET | 读取项目内的录像 JSON 文件（参数：path） |

### 14.9 交互流程

1. POST `/api/games` 创建对局 → 得到 `session_id`
2. GET `/api/games/{id}` 获取状态 → 渲染棋盘
3. 用户点击 → 将点击映射为 `action_id`
4. POST `/api/games/{id}/action` 提交动作
5. 轮询 GET `/api/games/{id}/pipeline` 直到 `phase == "done"`
6. 重新获取状态 → 渲染新棋盘

---

## 15. 测试

DinoBoard-v2 有一套完整的参数化测试，自动覆盖所有已注册游戏。新游戏只需加入一个列表，即可获得完整的回归测试保护。

### 15.1 运行测试

```bash
# 运行全部测试（约 40 秒）
python -m pytest tests/ -x -q

# 只跑某个游戏的测试
python -m pytest tests/ -k quoridor -q

# 只跑某类测试
python -m pytest tests/test_do_undo_consistency.py -q

# 详细输出（调试失败时用）
python -m pytest tests/ -k "your_game" -v --tb=short
```

### 15.2 测试覆盖范围

| 测试文件 | 覆盖内容 | 测试数 |
|---|---|---|
| `test_registry_smoke.py` | 注册、available_games、config 加载 | ~14 |
| `test_game_state_consistency.py` | 初始状态、合法动作、状态变化、终局 | ~25 |
| `test_game_session.py` | GameSession 创建、走棋、AI 决策 | ~27 |
| `test_do_undo_consistency.py` | do/undo 完美逆操作、种子确定性 | ~24 |
| `test_encode_state.py` | encode_state 维度、config 一致性 | ~45 |
| `test_feature_encoding.py` | 特征逐步变化、BUG-007 回归 | ~35 |
| `test_selfplay_sample_integrity.py` | 样本结构、visits=sims、z 与 winner 一致 | ~48 |
| `test_sample_collection.py` | 样本收集流程、policy 格式 | ~61 |
| `test_mcts_core.py` | MCTS 搜索行为、温度、噪声 | ~22 |
| `test_onnx_roundtrip.py` | PyTorch → ONNX → C++ selfplay | ~17 |
| `test_arena.py` | 模型对战、胜率统计 | ~30 |
| `test_training_pipeline.py` | 训练 tensor、前向/反向传播 | ~17 |
| `test_pipeline_integration.py` | 端到端 pipeline 流程 | ~30 |
| `test_config_passthrough.py` | game.json 参数透传到 C++ | ~15 |
| `test_heuristic_and_eval.py` | 启发式对局、eval 模式 | ~8 |
| `test_tail_solver.py` | 残局求解 API、统计不变量 | ~4 |
| `test_score_head.py` | 辅助训练信号 head | ~9 |
| `test_hidden_info.py` | ISMCTS root 采样、belief tracking、不偷看验证 | ~27 |
| `test_multiplayer.py` | 多人变体注册、selfplay、z_values | ~57 |
| `test_game_specific.py` | 各游戏特有组件（heuristic、filter 等） | ~35 |

### 15.3 自动参数化机制

所有通用测试都通过 `conftest.py` 中的 `CANONICAL_GAMES` 列表自动参数化：

```python
# tests/conftest.py
CANONICAL_GAMES = ["tictactoe", "quoridor", "splendor", "azul"]

@pytest.fixture(params=CANONICAL_GAMES)
def game_id(request):
    return request.param
```

测试函数只需声明 `game_id` fixture，pytest 就会对每个游戏各跑一次：

```python
def test_initial_not_terminal(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    assert not gs.is_terminal
```

可选组件的测试通过独立列表控制，避免在没有该组件的游戏上报错：

```python
GAMES_WITH_HEURISTIC = ["quoridor"]
GAMES_WITH_TAIL_SOLVER = ["quoridor", "splendor"]
GAMES_WITH_TRAINING_FILTER = ["quoridor"]
```

### 15.4 接入新游戏

新游戏通过所有手动验证后（见 [NEW_GAME_TEST_GUIDE.md](NEW_GAME_TEST_GUIDE.md)），将其加入自动化测试：

1. **加入 `CANONICAL_GAMES`**：

```python
CANONICAL_GAMES = ["tictactoe", "quoridor", "splendor", "azul", "your_game"]
```

2. **加入可选组件列表**（根据你注册的组件）：

```python
GAMES_WITH_HEURISTIC = ["quoridor", "your_game"]
GAMES_WITH_TAIL_SOLVER = ["quoridor", "splendor", "your_game"]
```

3. **运行全量测试确认通过**：

```bash
python -m pytest tests/ -x -q
```

之后每次代码变更，所有参数化测试都会自动覆盖你的游戏。

### 15.5 测试辅助工具

`conftest.py` 提供常用 fixture 和 helper：

| 名称 | 类型 | 说明 |
|---|---|---|
| `game_id` | fixture | 当前游戏 ID（自动参数化） |
| `game_config` | fixture | 当前游戏的 game.json dict |
| `model_path` | fixture | 随机 ONNX 模型路径（自动缓存） |
| `get_test_model(game_id)` | 函数 | 创建/缓存一个随机初始化的 ONNX 模型 |
| `run_short_selfplay(game_id)` | 函数 | 快速跑一局 selfplay（10 sims, 50 plies） |
| `run_short_heuristic(game_id)` | 函数 | 快速跑一局 heuristic 对局 |

---

## 16. 完整 Checklist

### 必须完成

- [ ] 创建 `games/<name>/` 目录
- [ ] 实现 `<name>_state.h/.cpp`（继承 `CloneableState<T>`，实现 6 个必须方法）
- [ ] 实现 `<name>_rules.h/.cpp`（继承 `IGameRules`，实现 4 个必须方法）
- [ ] 实现 `<name>_net_adapter.h/.cpp`（继承 `IFeatureEncoder`，实现 3 个必须方法）
- [ ] 创建 `<name>_register.cpp`（GameRegistrar + 工厂函数）
- [ ] 创建 `config/game.json`（game_id、action_space、feature_dim 必须精确）
- [ ] 创建 `CMakeLists.txt`
- [ ] 在顶层 `CMakeLists.txt` 添加 `add_subdirectory(games/<name>)`
- [ ] 在 `setup.py` 的 sources 列表中添加 4 个 .cpp 文件
- [ ] 构建通过：`pip install -e .`
- [ ] 验证注册：`python -c "import dinoboard_engine; print(dinoboard_engine.available_games())"`

### 必须完成（续）

- [ ] 实现 `state_serializer`（Web 前端需要）
- [ ] 实现 `action_descriptor`（Web 前端需要）
- [ ] 创建 `web/` 前端（玩家游玩 + 验收训练结果的主要界面）
- [ ] 通过手动验证：按 [NEW_GAME_TEST_GUIDE.md](NEW_GAME_TEST_GUIDE.md) 逐步验证
- [ ] 加入自动化测试：将 game_id 加入 `tests/conftest.py` 的 `CANONICAL_GAMES`
- [ ] 全量测试通过：`python -m pytest tests/ -x -q`
- [ ] **通过 AI API 分离验收**（强制——证明 AI 不从 ground truth 偷看信息的唯一机制；不过这一步即使其他测试全过也不算合格）：
  - 所有游戏：`tests/test_ai_api_separation.py::test_full_game_via_api[<name>]` 必须通过
  - 随机游戏额外：`tests/test_api_belief_matches_selfplay.py` 的三个断言（belief / public state / legal actions 等价）
  - 详见 §17
- [ ] 运行首次训练：`python -m training.cli --game <name> --output runs/<name>_001`
- [ ] 部署模型：将 `runs/<name>_001/models/model_best.onnx` 复制到 `games/<name>/model/<name>_<N>p.onnx`，其中 `<N>p` 是变体标识（2p / 3p / 4p）。同一游戏的所有变体模型共用 `games/<base>/model/`，不再为每个变体建单独目录。例如 Azul 的三个模型并排放在 `games/azul/model/{azul_2p,azul_3p,azul_4p}.onnx`。
- [ ] 创建 `config/web.json`（可选，配置 Web AI 参数：难度覆盖、温度、动作过滤、残局求解）
- [ ] **写 `docs/games/<name>_api.md`**——面向第三方接入者的 AI API 说明。至少包含：动作空间编码表、公开事件格式与发放时机、一个完整 curl/python 示例。模板见 `docs/games/tictactoe_api.md`（最简）和 `docs/games/splendor_api.md`（带事件复杂例）。**这份文档是游戏的"外部接口规范"**——没它第三方无法把你的 AI 接入他们的游戏 / 网站

### 推荐完成

- [ ] 实现 `heuristic_picker`（加速训练收敛）

### 进阶可选

- [ ] 实现 `adjudicator`（长局游戏需要）
- [ ] 实现 `auxiliary_scorer`（提供额外训练信号）
- [ ] 实现 `training_action_filter`（动作空间过大时加速学习）
- [ ] 实现 `episode_stats_extractor`（自定义训练监控）
- [ ] 启用 `tail_solver`（残局求解，需要 `do_action_deterministic`）
- [ ] 实现 `IBeliefTracker`（隐藏信息游戏）

---

## 17. AI API 分离验收 —— 信息泄漏的唯一证明

> **这一节的硬性要求**：一个新游戏的 AI 实现不通过本节所有测试就不能算验收合格，哪怕 selfplay 能跑、ONNX 能导出、Web 对局能完成。
>
> **为什么**：CLAUDE.md 的核心设计原则要求 AI 链路只依赖观察历史、不读真实隐藏 state。但这个要求靠 code review 根本无法保证——一个 `checked_cast` 加一行隐藏字段读取就能悄悄泄漏，审代码时很容易漏掉。**唯一能确定性证明"AI 没有偷看"的方法**就是：把 AI 放到一个独立的 session 里，用和 ground truth **不同的 seed** 初始化（两边的内部隐藏 state 完全不同），只通过动作序列 + 公开事件同步，最后检查 AI 维护的 belief 是否与 ground truth 的自博弈 belief 逐步等价。
>
> 如果 AI 代码里有任何对真实 state 的隐藏读取，这个测试会把它暴露为 belief 发散、公开 state 发散、或 AI 给出非法动作。反之，如果三个断言全过，就在信息论层面证明了 AI 决策只依赖观察序列——这正是设计原则要求的"分离"。

框架提供一个观察驱动的 AI 推理 API（`platform/ai_service/`），把 AI 决策暴露为 HTTP 端点。外部调用者只通过动作 ID 与 AI 交互，任何 state 字段都不会跨越边界。这同时也是未来对接第三方数字化桌游团队的接口。

### 17.1 两道门槛

两层测试都通过才算合格：

**第一层**：`tests/test_ai_api_separation.py::test_full_game_via_api[<game_id>]`（所有游戏）
- API 契约干净（没有 state 进，没有 state 出）——由响应字段白名单扫描保证
- 给定初始设置 + 动作序列，AI 能端到端完成对局
- AI 返回的动作永远在 ground truth 的合法动作集里

**第二层**：`tests/test_api_belief_matches_selfplay.py`（随机游戏）——这是真正的分离证明
- AI 用独立 seed 从零启动
- 只通过公开事件同步
- 每步的 belief snapshot 必须与自博弈维护的 belief 完全相等
- 终局公开 state 必须完全相等
- perspective 回合的 legal actions 必须完全相等

### 17.2 新游戏需要做什么

**确定性游戏**（无 `belief_tracker`）：
1. 把 `game_id` 加入 `tests/conftest.py` 的 `CANONICAL_GAMES`
2. 在 `tests/test_ai_api_separation.py` 的 `_PLY_BUDGET` 里加一条
3. 同时把 `game_id` 加入 `_DETERMINISTIC_GAMES`（启用独立 seed 测试）
4. 跑 `pytest tests/test_ai_api_separation.py -v` 全通过

**随机或信息不对称游戏**（有 `belief_tracker`）：
1. 上面确定性游戏的 4 步
2. 实现 `IBeliefTracker::serialize()` — 输出 canonical 可对比字典（sorted set → vector）
3. 实现 public-event 协议（§17.4），在 GameBundle 注册：
   - `public_event_extractor` — selfplay 侧：state_before + action + state_after → 事件列表
   - `public_event_applier` — API 侧：把事件 apply 到 AI 的 state 上
   - `initial_observation_extractor` / `initial_observation_applier` — 初始设置同步
4. 把 `game_id` 加入 `tests/test_api_belief_matches_selfplay.py` 的 `GAMES_WITH_EVENT_PROTOCOL`
5. 在同文件 `_PUBLIC_KEYS` 字典里加游戏的公开字段列表
6. 跑 `pytest tests/test_api_belief_matches_selfplay.py -v -k <game_id>` 三个断言全通过

### 17.3 Public-Event 协议设计

事件负责在 AI 侧同步 ground truth 的公开事实（翻的新卡、抽到的公共牌、挑战揭露的身份等）。AI 内部 state 在 do_action_fast 跑出来的随机结果会被 event 覆盖，belief tracker 通过 `observe_public_event(actor, action, pre_events, post_events)` 读到正确的事件流。

**事件 shape**：`{"kind": str, "payload": dict}`，kind 和 payload 结构由每个游戏定义。

**事件分两种时序**：
- **post-action**：`do_action_fast` 结束后 apply。覆盖随机翻牌/抽牌结果。Splendor `deck_flip`、Azul `factory_refill` 都属于这类。
- **pre-action**：`do_action_fast` 之前 apply。当动作的效果**依赖隐藏 state** 时必须用——比如 Love Letter Baron 对决要比较双方手牌，AI 内部对手手牌是随机的，不先改对动作就错了。

**`apply_observation(action, pre_events, post_events)`**：API 侧统一入口，顺序：
1. 克隆 state_before
2. apply 所有 pre_events
3. `do_action_fast(action)`
4. apply 所有 post_events
5. `belief_tracker.observe_public_event(actor, action, pre_events, post_events)` — tracker 从事件流增量更新（不再读 state_before/state_after）

**可见性过滤**：ground truth 端实现 `public_event_extractor` 时决定给 perspective 看什么。比如对手盲抽一张卡，事件只传 `{"player": 1}`（不带牌面），AI 知道"发生过抽牌"但不知道具体卡。框架不做 firewall——ground truth 愿意多传也可以（对接友好）。

**初始观察**：`initial_observation_extractor` 输出 perspective 视角能看到的开局信息（比如 Love Letter 的 `my_hand`、Splendor 的 `tableau` + `nobles`）。`initial_observation_applier` 在 API session 启动时 apply。

### 17.4 参考实现

| 游戏 | 事件类型 | 关键特点 | 位置 |
|------|---------|---------|------|
| Azul | `factory_refill` (post) | stateless tracker，只同步 factories | `games/azul/azul_register.cpp` |
| Splendor | `deck_flip` (post), `self_reserve_deck` (post) | tracker 维护 seen_cards，盲预订时 AI 需要知道自己抽了什么 | `games/splendor/splendor_register.cpp` |
| Love Letter | `hand_override` (pre/post), `drawn_override` (post) | pre-action 场景最多——Baron/Guard/Priest/Prince/King 都读对手手牌 | `games/loveletter/loveletter_register.cpp` |

**编写事件协议时的 checklist**：
- [ ] `do_action_fast` 每处读 `state.hand[other]` / `state.deck` / 对手隐藏字段的地方，都要有对应的 pre-event
- [ ] `do_action_fast` 每处写 random 结果到 state 的地方（翻牌、抽牌），都要有对应的 post-event
- [ ] `advance_turn` 之类的子流程也要审一遍
- [ ] 事件只暴露 perspective 能公开看到的信息；不可见的事件仍要发（让 AI 知道"发生过"），但 payload 不含牌面
- [ ] `apply_event` 不仅要改可见字段，还要维护 deck 一致性（被揭露的卡从 deck 移除，被换出的卡加回 deck 等）
