# DinoBoard-v2 重构计划书

> **状态：已归档。** 本文档是项目初期的设计蓝图，实际实现有诸多演化。当前系统的准确描述请参考：
> - [docs/GAME_FEATURES_OVERVIEW.md](docs/GAME_FEATURES_OVERVIEW.md) — 功能概览
> - [docs/GAME_DEVELOPMENT_GUIDE.md](docs/GAME_DEVELOPMENT_GUIDE.md) — 开发者详细指南

## 项目目标

构建一个**通用棋盘游戏 AI 平台**，包含：
1. **训练框架**：实现新游戏时只需写规则引擎 + 配置文件，即可自动训练出强力 AI
2. **Web 对战平台**：统一入口，用户选择游戏后对战 AI。同时也是开发者验证规则引擎、测试启发式算法和 heuristic 的调试工具

## 核心原则

- 游戏开发者只写：C++ 规则引擎 + 特征编码 + config JSON + Web 渲染前端（`web/` 目录下的 HTML/CSS/JS）
- 所有训练逻辑、搜索算法、Web 平台代码在 general 层，游戏侧零样板
- 不完美信息（随机节点）、heuristic 等均为可选配置项，不需要额外代码

---

## 一、目标目录结构

```
DinoBoard-v2/
├── CMakeLists.txt                    # 统一构建（C++ 扩展 + pybind11）
├── engine/                           # C++ 通用引擎
│   ├── core/                         # 接口定义（IGameState, IFeatureEncoder, IStateValueModel）
│   │   ├── game_interfaces.h
│   │   ├── types.h
│   │   └── action_constraint.h/.cpp
│   ├── search/                       # MCTS 搜索（通用，不含游戏逻辑）
│   │   ├── net_mcts.h/.cpp
│   │   ├── root_noise.h
│   │   ├── temperature_schedule.h
│   │   └── search_options_common.h
│   ├── infer/                        # ONNX 推理
│   │   ├── onnx_policy_value_evaluator.h/.cpp
│   │   └── feature_encoder.h
│   └── runtime/                      # 通用 self-play / arena 循环（从各游戏的 engine_module 上提）
│       ├── selfplay_runner.h/.cpp    # 通用 self-play episode 逻辑
│       ├── arena_runner.h/.cpp       # 通用 arena match 逻辑
│       └── nopeek_support.h/.cpp     # with_hidden_randomized + TraversalLimiter 通用框架
│
├── bindings/                         # pybind11 绑定（薄层，只做类型转换）
│   ├── common_module.cpp             # 通用绑定模板
│   └── game_binding_template.h       # 游戏绑定的宏/模板
│
├── games/                            # 各游戏实现（只写规则 + 特征编码 + 渲染前端）
│   └── <game_name>/                  # 每个游戏一个目录，结构相同
│       ├── <game>_state.h/.cpp       # 游戏状态定义
│       ├── <game>_rules.h/.cpp       # 规则引擎（合法动作、状态转移、胜负判定）
│       ├── <game>_net_adapter.h/.cpp # 特征编码 + belief tracker（可选）
│       ├── <game>_register.cpp       # 注册到 GameRegistry（状态序列化、动作描述、heuristic）
│       ├── config/
│       │   └── game.json             # 游戏元数据 + 训练配置
│       ├── web/                      # 前端渲染（HTML/CSS/JS），规则逻辑走后端 API
│       └── model/                    # 训练产出的 ONNX 模型
│
├── training/                         # Python 训练框架（通用）
│   ├── pipeline.py                   # 主训练流程（合并当前 5 个 pipeline_*.py）
│   ├── selfplay.py                   # self-play 管理
│   ├── evaluator.py                  # 周期性 eval + gating
│   ├── trainer.py                    # PyTorch 训练器（合并 sparse + simple）
│   ├── warm_start.py                 # warm start
│   ├── config.py                     # 训练配置解析
│   ├── model.py                      # PvNet 定义 + ONNX 导出
│   ├── extensions.py                 # 通用 hooks（score margin 等）
│   ├── cli.py                        # 统一命令行入口
│   └── logging.py                    # 可配置的训练日志（游戏可自定义显示项）
│
├── platform/                         # Web 对战平台（从 mosaic-azul-web 迁移）
│   ├── app.py                        # FastAPI 主服务（拆分为多个模块）
│   ├── game_service/                 # 对局管理
│   │   ├── routes.py                 # 开局/走子/状态查询 API
│   │   ├── ai_worker.py             # AI 思考异步执行
│   │   └── analysis.py              # 录像分析
│   ├── tier/                         # 档位系统（体验/专家，无需登录）
│   │   ├── models.py                 # 档位定义
│   │   └── policy.py                 # 按档位限制 AI 强度
│   ├── db/                           # 数据库
│   │   ├── schema.py                 # SQLite 建表
│   │   └── migrations.py            # 数据迁移工具
│   └── static/                       # 通用前端资源
│       ├── index.html                # 统一入口页
│       ├── game_selector.js          # 游戏选择界面
│       └── common.css
│
├── scripts/                          # 运维脚本
│   ├── build.sh                      # 编译 C++ 扩展
│   ├── train.sh                      # 启动训练（通用，指定游戏名）
│   ├── eval_history.sh               # 查看训练 eval 历史
│   ├── tail_log.sh                   # 查看训练日志
│   └── deploy.sh                     # 部署 Web 平台
│
└── tests/                            # 测试
    ├── engine/                       # C++ 规则单元测试
    ├── training/                     # 训练 pipeline smoke test
    └── platform/                     # Web API 测试
```

---

## 二、关键设计决策

### 2.1 游戏注册机制

每个游戏通过 `config/game.json` 声明自身元数据，框架自动发现：

```json
{
  "game_type": "splendor",
  "display_name": "璀璨宝石",
  "players": 2,
  "feature_dim": 294,
  "action_space": 70,
  "has_hidden_info": true,
  "nopeek": {
    "enable_draw_chance": true,
    "stop_on_draw_transition": true,
    "chance_expand_cap": 10
  },
  "heuristic": {
    "available": true,
    "peek_advantage": true
  }
}
```

框架通过扫描 `games/*/config/game.json` 自动注册所有可用游戏，无需手动维护列表。

### 2.2 随机节点处理（三种模式）

通过 train config 的 `hidden_info_mode` 字段选择，游戏开发者不需要为此写额外代码：

| 模式 | 配置值 | 行为 | 适用场景 |
|------|--------|------|---------|
| 偷看 | `peek` | 搜索时直接看到实际结果，不做随机化 | 完美信息游戏，或复杂游戏训练前期加速收敛 |
| 截断 | `truncate` | 遇到随机节点时 determinize 一次，然后截断搜索（用 value network 评估） | 随机分支多但影响可控的游戏 |
| 不偷看 | `nopeek` | determinize 后继续展开搜索，chance node 分叉 | 需要精确评估随机影响的游戏 |

无隐藏信息的游戏（如 Quoridor、TicTacToe）不需要配置此字段，默认走标准 MCTS。

### 2.3 残局求解器（Tail Solve，可选接入训练）

参考现有 Splendor 的实现：alpha-beta + 转置表，在 MCTS 前短路。
重构后泛化为框架能力，游戏可选接入。

**核心设计：**

- 残局求解不依赖任何随机元素。遇到随机分支（如抽牌）时假设该分支不可用或以最差情况处理
- 求解器作为 MCTS 的前置短路：证明了必胜/必败则直接返回，否则回退到 MCTS
- 接入训练时的 self-play episode 中也可启用，加速后期局面的 value 精度

**时间控制（关键约束）：**

残局求解不能拖慢训练。控制原则：单局 self-play 中残局求解总耗时不超过该局总时间的 50%。
通过以下参数联合控制：

| 参数 | 说明 | 参考值 |
|------|------|--------|
| `tail_solve_start_ply` | 多少步之后才尝试求解 | 40 |
| `tail_solve_depth_limit` | 搜索深度限制 | 5 |
| `tail_solve_node_budget` | 单次求解的节点数上限 | 10,000,000 |
| `tail_solve_time_ms` | 单次求解的时间上限（0=不限） | 由框架根据当局已用时间动态计算 |

框架在 self-play 时追踪每局的 MCTS 耗时和 tail-solve 耗时，如果 tail-solve 累计占比超过阈值，
后续步骤自动降低 node_budget 或跳过求解，确保不拖后腿。

**实现策略：框架提供通用默认实现 + 游戏可选覆盖**

框架基于 IGameState 已有接口（legal_actions, do_action, undo_action, is_terminal, terminal_value）
提供通用的 alpha-beta + 转置表求解器。

随机后果的处理——**占位符模式**：

残局求解不跳过有随机后果的动作，而是正常执行动作，但用**占位符**代替随机结果。
占位符的唯一规则：`legal_actions` 永远不会生成"对占位符操作"的动作。
alpha-beta 搜索自然不会走到占位符相关分支，无需额外处理。

以 Splendor 为例：

| 动作 | 正常版 | 残局求解版 |
|------|--------|-----------|
| 买 tableau 上的牌 | 买牌 + 从牌堆补一张新牌 | 买牌 + 补位放占位符（不可购买） |
| 从 tableau 预留 | 拿牌 + 牌堆补新牌 | 拿牌 + 补位放占位符 |
| 从牌堆预留 | 拿金币 + 抽一张牌到手 | 拿金币 + 预留一张占位符（不可购买） |
| 拿宝石/还宝石/选贵族 | 无随机性 | 不变 |

游戏侧实现 `do_action_deterministic()` 时只需处理会触发随机的动作，
其他动作直接调 `do_action()`。无随机后果的游戏（棋类等）不需要实现。

这个占位符模式是通用的：任何有"从牌堆/袋子抽取"机制的游戏都可以用同样的方式处理。

```cpp
// IGameState 可选覆盖
virtual void do_action_deterministic(GameState& s, ActionId a) {
  do_action(s, a);  // 默认 fallback：无随机后果的游戏直接用这个
}
```

如果游戏需要更高性能（如 Splendor 残局复杂度高），可选实现 ITailSolver 覆盖默认行为：

```cpp
class ITailSolver {
  // 紧凑状态 + 高效哈希 + 动作排序启发
  // 比通用实现更快，但不是必须的
};
```

大多数游戏用默认实现即可，只有残局搜索空间特别大的游戏才需要自定义优化。

**架构：AI Agent = 信息维护模块 + 搜索模块**

规则引擎（GameState）保持纯粹：上帝视角，只管游戏机制。
AI 端分为两个解耦的模块：

```
AI Agent
├── 信息维护模块（BeliefTracker）
│   - 从开局起逐步记录该玩家见过的牌
│   - 维护 unseen_pool（全部牌 - 见过的牌）
│   - 提供 randomize_unseen()：把 unseen_pool 随机分配到牌堆 + 对手暗牌槽
│   - 当前实现：写死均匀分布（简单可靠）
│   - 未来可替换为 IS-MCTS 或贝叶斯推断（接口不变，搜索模块无需改动）
│
└── 搜索模块（Net-MCTS）
    - 标准 PUCT + 神经网络评估
    - 遇到随机节点时，调 BeliefTracker.randomize_unseen() 做 determinize
    - 不关心信息怎么来的，只管搜索
```

两个模块通过接口隔离：搜索模块只调 `randomize_unseen()`，不知道也不关心背后的推断策略。

**为什么不能从当前状态快照推算**：
当前状态里历史信息已丢失（如被买走的牌只留下 bonus 计数），
无法反推 AI 见过哪些牌。所以 BeliefTracker 必须从开局起逐步跟踪。

**游戏侧需实现的接口**（仅有隐藏信息的游戏需要）：

```cpp
class IBeliefTracker {
  // 游戏开始时初始化（记录初始可见信息）
  void init(const GameState& state, int perspective_player);

  // 每次动作后更新（记录新暴露的信息）
  void observe_action(const GameState& before,
                      int action,
                      const GameState& after);

  // MCTS determinize 时调用：基于维护的信息随机化未见部分
  void randomize_unseen(GameState& state, RNG& rng);
};
```

这解决了当前版本的 bug：`with_hidden_randomized()` 只洗牌堆，不处理对手暗藏的预留牌。
正确做法是基于信息集做 determinization——所有未见过的牌作为统一池子随机分配。

### 2.3 训练日志（可配置）

框架提供默认日志格式，游戏可通过 game.json 中 `log_fields` 自定义显示项：

```json
{
  "log_fields": {
    "selfplay_step": ["samples", "normal_samples", "avg_plies", "wins"],
    "eval": ["benchmark_win_rate", "history_best_win_rate"]
  }
}
```

不指定则使用默认日志格式（samples, avg_plies, wins）。像 Splendor 的 normal_samples 是游戏特定的，其他游戏不需要写。

### 2.4 Heuristic（可选）

- 游戏提供 heuristic → warm start 和 eval benchmark 使用它
- 游戏不提供 heuristic → warm start 使用随机引擎，eval benchmark 用随机或跳过
- 通过 game.json 的 `heuristic.available` 字段声明

### 2.5 Web 平台用户模型与档位

**无用户系统，全部游客**。不做注册/登录/用户认证。所有访客以匿名游客身份直接开局。
对局通过 session ID（服务端生成、cookie 持有）关联，不绑定用户账号。

两级免费体系，用户在开局时自行选择：

| 档位 | AI 强度 | 说明 |
|------|---------|------|
| 体验 | MCTS 10 次模拟 | 适合休闲玩家，服务器负载极低 |
| 专家 | MCTS 5000 次模拟 | 适合高手，每局计算量大 |

具体模拟次数按游戏可配置，写在 game.json 里。

### 2.6 SQLite 并发与任务分片（关键约束）

SQLite 写操作持有全局锁。MCTS 搜索和残局求解是重计算（可能几秒到几十秒），
**绝对不能在持有数据库连接/事务的情况下执行 AI 计算**。违反此原则会导致一个用户的 AI 思考阻塞所有其他用户的读写请求。

**正确的任务分片模式：**

```
1. 从 DB 读取游戏状态 → 立即关闭连接 / 释放锁
2. 在内存中执行 MCTS / tail solve（纯计算，不碰 DB）
3. 计算完成后再开新连接，写入结果
```

所有涉及 AI 计算的路径都必须遵守此模式，包括：
- `pipeline.py` 中的 AI 走棋 worker
- `replay.py` 中的录像分析（MCTS 评估、掉分计算）
- 预计算 worker（用户思考时后台跑的分析）
- 未来的残局求解器（tail solve）

**实现要求：**
- 使用短连接模式：每次 DB 操作开连接、完成后立即关闭，不要维持长连接
- AI 计算函数签名中不传入 DB connection，从接口层面杜绝误用
- ThreadPoolExecutor 中的 worker 只接收纯数据（游戏状态的 dict/对象），不接收 DB 句柄

### 2.7 录像分析系统（从 mosaic-azul-web 迁移并泛化）

这是 azul 项目最有价值的功能之一，必须完整复刻到通用平台。

**核心架构：**

```
用户走棋 → HTTP 立即返回
              ↓
        后台 Pipeline（ThreadPoolExecutor）
        ├── 1. 分析用户走的棋（复用预计算结果）
        ├── 2. AI 思考并走棋
        └── 3. 预计算下一个用户局面的分析
              ↓
        前端 300ms 轮询 → 拿到 AI 走棋 + 分析结果
```

**每步分析内容：**

| 字段 | 说明 |
|------|------|
| `best_move` | AI 推荐的最佳动作 |
| `best_win_rate` | 最佳动作的胜率估计 |
| `chosen_move` | 实际走的动作 |
| `chosen_win_rate` | 实际动作的胜率估计 |
| `drop_score` | 掉分 = (best_win_rate - chosen_win_rate) × 100 |
| `severity` | `normal`（<5%）/ `warn`（≥5%，失误）/ `blunder`（≥10%，严重失误）|
| `chosen_rank` | 实际动作在所有合法动作中的排名 |
| `tail_solved` | 是否使用残局精确求解 |

**录像回放：**

- 每步存完整棋盘快照（replay_frame），不只是动作列表
- 前后跳转 = 直接加载对应快照，不需要从头重放
- 支持一键跳转到失误/严重失误步
- 支持自动播放（逐步推进 + 动画）

**预计算机制：**

在用户思考时，预先对当前局面跑 MCTS 分析。用户走完棋后：
- 如果预计算的局面和实际一致 → 秒出分析结果
- 如果不一致（用户悔棋等） → fallback 到实时计算

**残局阶段：**

后期局面自动切换到 tail solve 精确求解，分析结果从"估计胜率"变为"精确胜率"。

**悔棋与替对手落子：**

桌游不一定严格一人一步，可能出现 AABB、AABBAA 等连续动作序列。
但把同一玩家的连续动作视为一个"回合"，整体仍是回合制。

- **悔棋**：回退到自己最后一组连续动作的开头。
  例：历史 `AABBAA`，A 悔棋 → 回到 `AABB` 之后（撤销最后的 `AA`）
- **替对手落子**：回退到对手最后一组连续动作的开头，替对手走一步。
  例：历史 `AABB`，A 替对手落子 → 回到 `AA` 之后（撤销 `BB`），然后 A 帮 B 走一步

两个操作都只能在轮到自己行动时使用。
实现上基于 moves 表的 actor 字段向前扫描，找到同一 actor 的连续组边界即可。

**SQLite 并发约束：** 见 2.6 节。所有 worker 必须遵守"读→释放锁→计算→再写"的分片模式。

**泛化要点：**

azul 项目里这些全写在一个 5393 行的 app.py 里。重构时拆分为：
- `platform/game_service/analysis.py` — 分析计算（MCTS 评估、掉分计算、severity 分类）
- `platform/game_service/pipeline.py` — 异步 pipeline（预计算、AI 思考调度）
- `platform/game_service/replay.py` — 录像快照生成与存储
- 游戏侧只需提供：`render_move_text(state, action) → str`（把 action ID 转为人类可读文字）

### 2.8 调试/对战前端开发规范

现有游戏的前端直接迁移即可。但未来新增游戏用 vibe coding 写前端时，
AI 容易走捷径——把每个 legal action 做成一个按钮。必须在框架层面防止这个问题。

**方案一：CLAUDE.md 前端规范（防止 AI 走歪）**

项目根目录的 CLAUDE.md 中写明前端交互硬性要求，vibe coding 时 AI 会自动读到：

```
【前端交互硬性要求】
禁止：把每个 legal action 做成按钮或下拉列表让用户逐条选。
必须：渲染完整游戏画面（棋盘/卡牌/宝石/棋子），用户通过点击游戏元素操作。
交互模式：
  1. 用户点击游戏元素（如一张牌、一颗宝石）
  2. 前端高亮所有可选的后续目标
  3. 用户点击目标完成操作
  4. 前端把点击序列转为 action ID 发送给后端
禁止在画面之外显示动作列表。唯一例外：当合法动作无法自然映射到画面元素时
（如"跳过回合"），允许一个单独按钮。
```

**方案二：action ID ↔ 语义双向映射（让前端代码自然按游戏逻辑组织）**

游戏引擎导出动作语义解码函数，前端基于语义渲染而非 action ID：

```cpp
// 游戏实现此接口（可选但强烈推荐）
struct ActionSemantics {
  string type;           // "buy_card", "take_gems", "reserve", "pass"...
  map<string, any> params;  // {tier: 1, slot: 2} 或 {colors: ["red","blue"]}
};
ActionSemantics decode_action(const GameState& s, ActionId a);
```

这样 AI 写前端时看到的是：
```json
{"type": "buy_card", "tier": 1, "slot": 2, "card_name": "蓝宝石矿"}
```
而不是 `action_id: 42`。前端代码自然会按"点击卡牌→购买"的逻辑组织。

两个方案组合使用：规范文档确保 AI 不走歪路，语义映射让正确的实现更容易写出来。

### 2.9 多人游戏支持

框架不限定 2 人，支持 2-N 人游戏（如 4 人 Splendor）。

**game.json 声明：**
```json
{
  "players": {"min": 2, "max": 4, "default": 2}
}
```

**各层影响与方案：**

| 层 | 2人现状 | 多人方案 |
|---|---|---|
| 规则引擎 | `kPlayers = 2` 硬编码 | 改为运行时参数，由 game.json 配置 |
| MCTS | negamax backup（零和二人） | max^n backup：每个节点存 N 维 value vector，各玩家最大化自己的分量 |
| 神经网络 value head | 单标量（我的胜率） | 单标量保持不变（"我赢的概率"），简单够用 |
| 特征编码 | me + opponent 双视角 | me + opponent_1 + opponent_2 + ... 按座位顺序排列 |
| Web 平台 | 1v1（人 vs AI） | 多人房间：人 + AI 混合，支持 2-N 人 |
| 录像分析 | 双方胜率对比 | 每步显示所有玩家胜率变化 |
| 悔棋/替对手落子 | 回退自己/对手的最后回合 | 悔棋不变；替对手落子改为选择替哪个对手（4人局多两个按钮） |
| 开局界面 | 无人数选择 | 加人数选项（默认范围 2-4 人，由 game.json 的 min/max 决定） |

**特征编码约定：**
- 第 0 个视角固定为 "me"（当前决策玩家）
- 后续按座位顺序从 me 的下家开始排列
- feature_dim 在 game.json 中按最大玩家数声明，不足的玩家位填零

**训练：**
- 多人自对弈时所有玩家用同一个模型（不同视角）
- value target z：赢 = +1，输 = -1，可选分差 shaping
- 评估：候选模型占一个座位，其余座位放 benchmark（heuristic 或历史最佳）

### 2.10 技术选型

| 项目 | 选择 | 理由 |
|------|------|------|
| 仓库 | 新建 `DinoBoard-v2` | 旧仓库保留训练服务器运行 |
| 前端 | 原生 JS | 每个游戏独立页面，不需要框架；vibe coding 更直接 |
| 数据库 | SQLite | 个人项目单机部署，零成本；遵守不持锁做重计算即可 |
| 后端 | FastAPI | 沿用 azul 项目，异步支持好 |
| AI 推理 | ONNX Runtime | 沿用，跨平台 |
| C++ 构建 | CMake + pybind11 | 统一构建所有游戏 |

### 2.10 构建系统

统一 CMake：
- 顶层 CMakeLists.txt 扫描 `games/*/src/` 自动构建
- 每个游戏编译为一个 Python 扩展模块（如 `cpp_splendor_engine`）
- ONNX Runtime 作为可选依赖，通过 CMake option 控制
- 支持 Linux（训练服务器）和 macOS（本地开发）

---

## 三、执行阶段

### 阶段 1：骨架搭建
- 新建仓库，创建目录结构
- 设置 CMakeLists.txt 基础框架
- 搬运 C++ 核心代码（engine/core, engine/search, engine/infer）
- 确保基础能编译通过

### 阶段 2：通用运行时
- 从各游戏的 `cpp_*_engine_module.cpp` 中提取通用 self-play / arena 逻辑到 `engine/runtime/`
- 实现通用 pybind11 绑定模板
- 搬运一个游戏（tictactoe，最简单）验证架构

### 阶段 3：训练框架
- 合并 Python 训练 pipeline（5 个文件 → 3 个）
- 实现 game.json 自动发现 + 配置驱动
- 实现可配置训练日志
- 用 tictactoe 跑通完整训练流程

### 阶段 4：迁移所有游戏
- 依次迁移 Azul、Quoridor、Splendor
- Splendor 的 nopeek 机制泛化到 engine/runtime/nopeek_support
- 每迁移一个游戏，跑 smoke test 验证

### 阶段 5：Web 对战平台
- 全游客模式，无用户注册/登录，session ID 关联对局
- 拆分 5393 行 app.py 为模块化结构
- 实现统一游戏入口页
- SQLite 持久化（遵守 2.6 节任务分片原则）
- 适配多游戏的 AI worker
- 完整复刻录像分析系统（详见 2.7 节）

### 阶段 6：调试前端整理
- 各游戏 Web 前端迁移到 `games/*/web/`
- 调试服务复用 platform 的 FastAPI 实例，不再单独启动

### 阶段 7：测试与部署
- C++ 规则单元测试
- 训练 pipeline smoke test
- Web API 测试
- 部署脚本

---

## 四、验收标准

1. 新增一个简单游戏（如四子棋）时，只需要：
   - 写 C++ 规则引擎（rules + state + net_adapter）
   - 写一个 game.json
   - 写前端交互（web/）
   - 无需触碰 training/、engine/runtime/、platform/ 中的任何代码

2. 训练命令统一为：
   ```
   python -m training.cli --game splendor --config games/splendor/config/train.json --output runs/splendor_001
   ```

3. Web 平台统一入口，用户选择游戏即可对战

4. Splendor nopeek 训练效果不退化（对 benchmark 胜率能持续上升）
