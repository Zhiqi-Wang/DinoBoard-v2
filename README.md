# DinoBoard-v2

通用棋盘游戏 AI 平台。AlphaZero 风格的 MCTS + 神经网络自我对弈训练，支持完美信息和不完美信息游戏。

**核心理念**：游戏开发者只需编写 C++ 规则引擎 + 特征编码 + JSON 配置，即可自动训练出强力 AI 并在 Web 上对战。

---

## 架构概览

```
┌─────────────────────────────────────────────────────┐
│                    Python 层                         │
│  training/pipeline.py  ←→  bindings/py_engine.cpp   │
│  training/cli.py            (pybind11)              │
│  platform/app.py       ←→  GameSession              │
├─────────────────────────────────────────────────────┤
│                    C++ 引擎                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐  │
│  │ runtime/  │  │ search/  │  │ infer/           │  │
│  │ selfplay  │→ │ NetMCTS  │→ │ ONNX Evaluator   │  │
│  │ arena     │  │ TailSolv │  │ (可选 ONNX RT)    │  │
│  │ heuristic │  │ NoPeek   │  └──────────────────┘  │
│  └──────────┘  └──────────┘                         │
│  ┌──────────────────────────────────────────────┐   │
│  │ core/ — 接口定义                               │   │
│  │ IGameState · IGameRules · IFeatureEncoder     │   │
│  │ IBeliefTracker · GameRegistry · GameBundle    │   │
│  └──────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────┤
│                    游戏实现                          │
│  games/tictactoe/  games/quoridor/                  │
│  games/splendor/   games/azul/                      │
│  games/loveletter/ games/coup/                      │
└─────────────────────────────────────────────────────┘
```

---

## 目录结构

```
DinoBoard-v2/
├── engine/                         # C++ 通用引擎
│   ├── core/                       # 接口定义
│   │   ├── game_interfaces.h       #   IGameState, IGameRules, IStateValueModel
│   │   ├── feature_encoder.h       #   IFeatureEncoder
│   │   ├── belief_tracker.h        #   IBeliefTracker（隐藏信息）
│   │   ├── game_registry.h         #   GameBundle, GameRegistrar, 所有 typedef
│   │   ├── types.h                 #   ActionId, StateHash64, UndoToken
│   │   └── action_constraint.h     #   IActionConstraint（动作约束管线）
│   ├── search/                     # 搜索算法
│   │   ├── net_mcts.h/.cpp         #   PUCT-MCTS + 神经网络评估
│   │   ├── tail_solver.h/.cpp      #   Alpha-Beta 残局求解器
│   │   ├── root_noise.h            #   Dirichlet 噪声
│   │   └── temperature_schedule.h  #   温度衰减调度
│   ├── infer/                      # 推理
│   │   └── onnx_policy_value_evaluator.h/.cpp  # ONNX Runtime 推理
│   └── runtime/                    # 运行时
│       ├── selfplay_runner.h/.cpp  #   自我对弈循环 + FilteredRulesWrapper
│       ├── arena_runner.h/.cpp     #   模型对战
│       ├── heuristic_runner.h/.cpp #   启发式对局生成
│       └── nopeek_support.h/.cpp   #   不完美信息 MCTS 支持
│
├── games/                          # 游戏实现
│   ├── tictactoe/                  #   井字棋（参考实现）
│   ├── quoridor/                   #   步步为营（9×9，含墙）
│   ├── splendor/                   #   璀璨宝石（2-4人，隐藏信息）
│   ├── azul/                       #   花砖物语（2-4人，隐藏信息）
│   ├── loveletter/                 #   情书（2-4人，隐藏信息，玩家淘汰）
│   └── coup/                       #   政变（2-4人，虚张声势，质疑与反制）
│
├── bindings/py_engine.cpp          # pybind11 Python 绑定
├── training/                       # Python 训练框架
│   ├── pipeline.py                 #   自我对弈 + 训练 + 评估循环
│   ├── model.py                    #   PyTorch 网络定义
│   └── cli.py                      #   命令行入口
│
├── platform/                       # FastAPI Web 对战平台
│   ├── app.py                      #   主服务器
│   ├── game_service/               #   游戏会话、异步管线、录像分析
│   └── static/                     #   通用前端资源
│
├── setup.py                        # Python 构建脚本
├── requirements.txt                # Python 依赖
└── docs/                           # 文档
    ├── GAME_FEATURES_OVERVIEW.md   #   功能概览（搜索、训练、随机性、Web 前端）
    ├── GAME_DEVELOPMENT_GUIDE.md   #   游戏开发指南（接口、配置、构建、测试）
    ├── NEW_GAME_TEST_GUIDE.md      #   新游戏验收测试流程（9 步）
    ├── KNOWN_ISSUES.md             #   已知问题与踩坑汇总
    └── devlog/                     #   开发日志
```

---

## 快速上手

### 1. 构建

```bash
# 安装依赖
pip install pybind11 torch

# 构建 C++ 扩展（不含 ONNX Runtime）
pip install -e .

# （可选）启用 ONNX Runtime 加速推理
BOARD_AI_WITH_ONNX=1 BOARD_AI_ONNXRUNTIME_ROOT=/path/to/onnxruntime pip install -e .

# 验证
python -c "import dinoboard_engine; print(dinoboard_engine.available_games())"
# ['azul', 'azul_2p', ..., 'quoridor', 'splendor', ..., 'tictactoe']
```

### 2. 训练

```bash
# 训练 TicTacToe（约 5 分钟）
python -m training.cli --game tictactoe --output runs/tictactoe_001

# 训练 Quoridor（约数小时，含 warm start 和 heuristic guidance）
python -m training.cli --game quoridor --output runs/quoridor_001 \
    --workers 4 --eval-every 25 --eval-games 40 --eval-benchmark heuristic

# 所有 CLI 参数
python -m training.cli --help
```

训练配置由 `games/<game>/config/game.json` 驱动，CLI 参数可覆盖。Web 对局的 AI 参数（难度覆盖、残局求解、动作过滤等）由 `config/web.json` 配置。

**输出**：
- `runs/<name>/models/model_best.onnx` — 最佳模型
- `runs/<name>/checkpoint.pt` — PyTorch checkpoint
- `runs/<name>/train.log` — 训练日志

### 3. Web 对战

```bash
# 安装 Web 依赖
pip install -r requirements.txt  # fastapi, uvicorn

# 启动服务器
cd platform && python -m uvicorn app:app --host 0.0.0.0 --port 8000

# 打开浏览器
open http://localhost:8000
```

**功能**：
- 游戏选择首页
- 三种难度：Heuristic / Casual / Expert
- 多人模式：支持 2-4 人游戏，可选座位，AI 占剩余座位
- 悔棋、AI 提示
- 录像回放 + 掉分分析
- 加载测试录像（`platform/tools/eval_model.py` 生成）

---

## 各游戏状态

| 游戏 | C++ 引擎 | 训练 | Web 前端 | 模型 | 特殊功能 |
|------|---------|------|---------|------|---------|
| TicTacToe | 完成 | 完成 | 完成 | 有 | 基础参考实现 |
| Quoridor | 完成 | 进行中 | 完成 | 训练中 | Heuristic、Tail Solver、Adjudicator、Auxiliary Score、Training Filter |
| Splendor | 完成 | 未开始 | 未开始 | 无 | BeliefTracker、NoPeek、2-4 人 |
| Azul | 完成 | 未开始 | 未开始 | 无 | BeliefTracker、NoPeek、2-4 人 |
| Love Letter | 完成 | 训练中 | 完成 | 训练中 | BeliefTracker、NoPeek、2-4 人、玩家淘汰 |
| Coup | 完成 | 未开始 | 完成 | 无 | BeliefTracker、NoPeek、2-4 人、虚张声势、11 阶段状态机 |

---

## 核心设计概念

### GameBundle 注册模式

每个游戏通过 `GameRegistrar` 在程序启动时自动注册。工厂函数返回一个 `GameBundle`，包含状态、规则、编码器等所有组件。详见 [游戏开发指南](docs/GAME_DEVELOPMENT_GUIDE.md)。

### 17 个 GameBundle 字段

5 个必须（state, rules, value_model, encoder, game_id）+ 12 个可选（belief_tracker, stochastic_detector, enable_chance_sampling, state_serializer, action_descriptor, heuristic_picker, tail_solver, tail_solve_trigger, episode_stats_extractor, adjudicator, auxiliary_scorer, training_action_filter）。

### NoPeek 模式

针对不完美信息游戏的 MCTS 搜索方案。通过 BeliefTracker 追踪已知信息，在搜索遇到随机节点时停止并重新采样隐藏信息，避免「偷看」对手的牌。

### 训练管线

自我对弈 → Replay Buffer → SGD 训练 → ONNX 导出 → 评估 → 最佳模型门控（≥60% 胜率更新）。支持 Warm Start、Heuristic Guidance、Auxiliary Score、Training Action Filter、MCTS Schedule。

### AI API 与分离验收（强制门槛）

框架提供观察驱动的 AI 推理 API（`platform/ai_service/`）——外部调用者只传动作 ID 和公共事件，API 只返回动作 ID。这既是对接第三方数字化桌游的接口，也是**新游戏的硬性验收门槛**：

- 所有游戏必须通过 `tests/test_ai_api_separation.py`——API 契约不泄漏 state
- 随机游戏额外必须通过 `tests/test_api_belief_matches_selfplay.py`——AI 用独立 seed 从零启动，只靠公共事件与自博弈同步，belief tracker 逐步对齐

这两层测试是证明 AI 决策链路不读真实状态的**唯一机制**——code review 容易漏掉隐藏的状态读取，但独立 seed 的 belief 等价测试会把任何窃取行为暴露为 belief 发散。详见 [GAME_DEVELOPMENT_GUIDE §17](docs/GAME_DEVELOPMENT_GUIDE.md#17-ai-api-分离验收)。

---

## 文档

- **[功能概览](docs/GAME_FEATURES_OVERVIEW.md)** — 框架能力速查：搜索、训练、随机性处理、Web 前端、可选组件
- **[游戏开发指南](docs/GAME_DEVELOPMENT_GUIDE.md)** — 添加新游戏的完整指南，事无巨细
- **[新游戏验收测试](docs/NEW_GAME_TEST_GUIDE.md)** — 9 步验收流程，覆盖所有已知踩坑
- **[已知问题与踩坑汇总](docs/KNOWN_ISSUES.md)** — BUG-001 ~ BUG-020 + 通用踩坑与设计取舍
