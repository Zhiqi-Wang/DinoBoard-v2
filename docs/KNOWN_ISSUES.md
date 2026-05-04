# 已知问题与踩坑汇总

> 记录 DinoBoard-v2 开发过程中发现并修复的 bug，以及需要注意的设计取舍。
> 目的：避免未来开发者重复踩坑。

根据受众不同分为两大类：

- **框架层 Issues**：影响 `engine/` / `engine/runtime/` / `engine/search/` / `training/` / `bindings/` / `platform/` 的问题。修改框架代码前建议通读——这些是架构层面的坑，跨所有游戏生效。
- **游戏层 Issues**：只影响 `games/<name>/` 下具体游戏的 rules / encoder / tracker / config。开发新游戏时**应着重参考本节**——里面的模式（Splendor 不偷看、Love Letter 揭牌事件）是游戏开发者常见的踩坑点。

---

## 目录

### 框架层 Issues（搜索 / 训练 / 运行时 / 平台）

- [BUG-001] Tail Solver 转置表标志位反转
- [BUG-002] 平局 z 值赋值错误
- [BUG-003] 训练-评估动作空间不一致
- [BUG-004] Heuristic Runner 缺少 Adjudicator 支持
- [BUG-005] FilteredRulesWrapper 的 const_cast
- [BUG-006] Replay Buffer 样本利用率
- [BUG-007] pipeline.py 用初始局面特征训练所有样本
- [BUG-008] pipeline.py 读取嵌套 `temperature_schedule` 时静默失败
- [BUG-009] pipeline.py 重写丢失三项训练改进
- [BUG-010] pipeline.py 重写丢失 Replay Buffer
- [BUG-011] ONNX 未编译导致 MCTS 使用均匀策略
- [BUG-012] model_init.onnx 导出顺序错误
- [BUG-013] ONNX 不是每步导出，selfplay 用旧模型
- [BUG-014] best model 路径指向 latest 文件被覆盖
- [BUG-015] pipeline.py 用 z_values 取值但 C++ 不总是填充
- [BUG-016] legal mask 被 filter 缩小导致 free 模式失效
- [BUG-018] Adjudicator z_values 不零和 + 标量 value head 的 3p+ 展开 bug
- [BUG-019] 多人模式全链路 2p 硬编码
- [BUG-020] Pipeline 分析使用错误的 stats key 导致 expert AI 卡死
- [BUG-021] fly 动画继承源容器尺寸，导致 Azul 砖巨大化 + 顺序播放卡顿
- [BUG-022] cancel_pipeline 误清 precompute，导致 expert 第一手 AI 响应多 8 秒
- [BUG-024] GameSession MCTS 搜索在真实状态上跑，应隔离为 AI view
- [BUG-025] pipeline.py `nopeek_enabled` off-by-one：peek_steps=0 被错误解读为"第 0 步 peek"
- [BUG-026] ISMCTS-v2 DAG hash collision → MCTS 选中非法 action 崩溃

### 游戏层 Issues（具体游戏的规则 / 编码器 / tracker）

- [BUG-017] SplendorBeliefTracker 偷看牌堆内容（Splendor）
- [BUG-023] Love Letter AI 永远猜对 Guard — terminal-by-elimination 漏过 NoPeek 检测（Love Letter，**已被 ISMCTS-v2 重构整体解决**）

---

# 框架层 Issues

以下所有 issue 都位于 `engine/` / `engine/runtime/` / `engine/search/` / `training/` / `bindings/` / `platform/` ——修改框架代码前应通读，跨所有游戏生效。

## [BUG-001] Tail Solver 转置表标志位反转

**状态**：已修复
**文件**：`engine/search/tail_solver.cpp`
**严重程度**：高 — 导致残局求解结果错误

### 问题描述

Alpha-Beta 搜索的 minimizing 分支中，转置表（TT）的初始标志位和截断标志位互换了：

```
修复前（错误）：
  maximizing 分支：initial = kUpperBound, cutoff = kLowerBound  ✓ 正确
  minimizing 分支：initial = kUpperBound, cutoff = kLowerBound  ✗ 错误（和 max 一样了）

修复后（正确）：
  maximizing 分支：initial = kUpperBound, cutoff = kLowerBound  ✓
  minimizing 分支：initial = kLowerBound, cutoff = kUpperBound  ✓
```

### 根因分析

Alpha-Beta 中，TT 标志位的含义：
- **kExact**：存储的值是精确值
- **kLowerBound**：存储的值是真实值的下界（发生了 beta 截断）
- **kUpperBound**：存储的值是真实值的上界（没有提升 alpha）

对于 **maximizing** 分支：
- 初始 best_value = -∞，尚未搜索任何子节点 → 这是一个上界（kUpperBound）
- 搜索过程中提升了 alpha → 变为精确值（kExact）
- 发生 alpha >= beta 截断 → 实际值至少这么大，是下界（kLowerBound）

对于 **minimizing** 分支（关键区别）：
- 初始 best_value = +∞，尚未搜索任何子节点 → 这是一个下界（kLowerBound）
- 搜索过程中降低了 beta → 变为精确值（kExact）
- 发生 alpha >= beta 截断 → 实际值至多这么小，是上界（kUpperBound）

修复前 minimizing 分支的初始和截断标志完全反了，导致转置表在后续查询中做出错误的剪枝决策。

### 影响

Tail solver 可能错误地判断胜负，导致在残局阶段选择劣势走法。由于 tail solver 的结果会覆盖 MCTS 的选择（temperature=0 确定性选择），一旦结果错误，影响直接且严重。

### 修复代码

```cpp
// minimizing 分支 — 修复后的版本
best_value = 2.0f;
flag = TTFlag::kLowerBound;  // 修复：初始为下界（非 kUpperBound）
for (const ActionId action : ordered) {
    // ... alpha-beta 搜索 ...
    if (best_value < beta) {
        beta = best_value;
        flag = TTFlag::kExact;
    }
    if (alpha >= beta) {
        flag = TTFlag::kUpperBound;  // 修复：截断为上界（非 kLowerBound）
        break;
    }
}
```

---

## [BUG-002] 平局 z 值赋值错误

**状态**：已修复
**文件**：`engine/runtime/selfplay_runner.cpp`
**严重程度**：中 — 偏置训练数据

### 问题描述

selfplay_runner 在游戏平局时将所有样本的 z 值设为 -0.5，而非 0.0。

```
修复前：s.z = -0.5f;   // 平局被当作半输
修复后：s.z = 0.0f;    // 平局是中性结果
```

### 根因分析

z 值是训练 value head 的目标标签：+1 表示赢、-1 表示输、0 表示平局。使用 -0.5 意味着平局被当作「比输好一点但仍然是负面结果」。

### 影响

- Value head 会学到「平局不好」，导致模型在应该接受平局的位置过度冒险
- 对于平局很少的游戏（如 Quoridor）影响较小，但对于平局常见的游戏（如 TicTacToe）影响显著
- 同样的 bug 存在于 terminal 和 adjudicator 两个分支中，都已修复

---

## [BUG-003] 训练-评估动作空间不一致

**状态**：已修复
**文件**：`bindings/py_engine.cpp`, `training/pipeline.py`
**严重程度**：高 — 导致评估结果完全不可信

### 问题描述

当使用 `TrainingActionFilter`（如 Quoridor 的最短路径差约束）进行自我对弈训练时，模型只在约束后的动作空间内学习策略。但评估（eval vs heuristic）使用的是无约束的完整动作空间。

结果：模型在约束空间内已经能和 heuristic 五五开，但评估显示 0% 胜率。

### 根因分析

- 自我对弈：`run_selfplay_episode` 接收 `training_action_filter` 参数，通过 `FilteredRulesWrapper` 将约束注入 MCTS 搜索
- 评估：`run_arena_match` 不使用 `training_action_filter`，模型被迫在完整动作空间中选择，而它从未训练过这些动作
- 这本质上是一个 **distribution shift** 问题：训练和测试的动作分布不一致

### 修复方案

1. 在 `py_engine.cpp` 中添加 `run_constrained_eval_vs_heuristic` 函数：
   - 模型侧通过 `FilteredRulesWrapper` 约束动作空间
   - Heuristic 侧也通过 `FilteredRulesWrapper` 约束（公平对比）
2. 在 `pipeline.py` 中同时运行两种评估：
   - `eval vs heuristic (constrained)` — 约束对局，反映模型在训练分布内的实力
   - `eval vs heuristic (free)` — 自由对局，反映模型在完整游戏中的实力

### 教训

**当训练使用动作约束时，必须同时提供约束和非约束两种评估**。只看非约束评估会误判模型完全没有学到东西。约束评估是衡量学习进度的真实指标，非约束评估是衡量泛化能力的指标。

---

## [BUG-004] Heuristic Runner 缺少 Adjudicator 支持

**状态**：已修复
**文件**：`engine/runtime/heuristic_runner.h`, `engine/runtime/heuristic_runner.cpp`
**严重程度**：中 — warm start 训练数据标签错误

### 问题描述

`run_heuristic_episode()` 不接受 `adjudicator` 参数。当 heuristic 对局达到 `max_game_plies` 而游戏未终局时，所有样本的 z 值默认为 0.0（未知结果），而不是通过 adjudicator 判定胜负。

### 影响

- Warm start 阶段使用 heuristic 对局生成训练数据
- 对于像 Quoridor 这样经常不能在 ply 限制内结束的游戏，大量样本的 z 值为 0（既不是赢也不是输）
- 这些错误标签会误导 value head，降低 warm start 的效果

### 修复

在 `run_heuristic_episode` 的参数列表中添加 `GameAdjudicator adjudicator`。当游戏超时未终局时，调用 adjudicator 判定胜负并正确分配 z 值。

---

## [BUG-005] FilteredRulesWrapper 的 const_cast

**状态**：已知问题（安全的 workaround）
**文件**：`engine/runtime/selfplay_runner.h`
**严重程度**：低 — 代码不优雅但运行正确

### 问题描述

`IGameRules::legal_actions` 接口签名接收 `const IGameState&`，但 `TrainingActionFilter` 需要可变的 `IGameState&`（因为 filter 内部要 do_action + undo_action 来评估墙的质量）。

`FilteredRulesWrapper` 通过 `const_cast` 解决：

```cpp
std::vector<ActionId> legal_actions(const IGameState& state) const override {
    auto legal = inner_.legal_actions(state);
    if (filter_) {
        auto filtered = filter_(const_cast<IGameState&>(state), inner_, legal);
        if (!filtered.empty()) return filtered;
    }
    return legal;
}
```

### 为什么安全

Filter 内部执行的 `do_action_fast` + `undo_action` 是 **net-no-op**：状态在 filter 执行前后完全一致。逻辑上的 const 性被保持了，只是 C++ 类型系统无法表达这一点。

### 理想修复

将 `IGameRules::legal_actions` 改为接收非 const 的 `IGameState&`。但这会影响所有游戏实现和调用点，工作量大且风险高，暂不修改。

---

## [BUG-006] Replay Buffer 样本利用率

**状态**：已知行为（可接受）
**文件**：`training/pipeline.py`
**严重程度**：低 — 这是在线 RL 的正常行为

### 观察

Replay buffer 大小为 `episodes_per_step * 50 * 20`（约 100,000 个样本）。每步训练生成约 5,000 个新样本（100 episode * ~50 ply）。`train_epochs=3` 表示每步训练遍历整个 buffer 3 次。

结果：每个样本在被新数据淘汰前，平均只被训练 1-2 次。

### 是否是问题

**通常不是**。在线 RL（如 AlphaZero）中，使用每个样本少量次数可以防止对过时数据的过拟合。AlphaZero 原论文也使用类似的滑动窗口策略。

### 调参建议

- 如果训练 loss 不稳定：增大 buffer（提高 buffer multiplier）或减少 `episodes_per_step`
- 如果收敛太慢：减小 buffer 使数据更新鲜，或增加 `train_epochs`
- 如果 value head 质量差：增大 buffer 以增加训练样本多样性

---

## [BUG-007] pipeline.py 用初始局面特征训练所有样本

**状态**：已修复
**文件**：`training/pipeline.py`
**严重程度**：致命 — 模型在噪声上训练

### 问题描述

`pipeline.py` 在收集训练数据时调用 `encode_state(game_id, seed + sample["ply"])`。`encode_state` 始终创建一个新游戏（初始局面），用不同的 seed 参数——它不会重建第 N 步的棋局。结果：每个训练样本的 features 都是某个初始局面的编码，而 policy/value 标签来自实际对局中的中间局面。

### 根因分析

C++ selfplay runner 已经在每个采样点正确编码了 features（`SelfplaySample::features`），并通过 pybind11 返回给 Python。但 `pipeline.py` 忽略了这些 features，试图自己重新编码——然而 `encode_state` 的设计只是"给定 seed 创建初始局面并编码"，无法重建中间状态。

### 影响

- 所有训练样本的 features 与 policy/value 标签完全不匹配
- 模型学到的是随机噪声，无法泛化
- 这解释了 v6/v7/v8 训练中 free eval 始终为 0% 的根本原因
- constrained eval 能达到 ~42% 只是因为 training filter 大幅缩小了动作空间，使模型即使用错误特征也能碰巧选到不太差的动作

### 修复

使用 C++ 返回的 `sample["features"]` 作为训练输入，不再调用 `encode_state`。

### 教训

**永远不要在 Python 侧重新实现 C++ 已经做好的事**（详见 CLAUDE.md 的 "Training Pipeline" 原则）。features 的编码必须发生在采样点的实际游戏状态上，而 C++ selfplay runner 正是这么做的。

---

## [BUG-008] Splendor temperature_schedule 被静默忽略

**状态**：已修复
**文件**：`training/pipeline.py`
**严重程度**：中 — Splendor 训练时温度衰减完全失效

### 问题描述

`pipeline.py` 读取 `train_cfg.get("temperature_initial", -1.0)` 等平坦键来配置温度衰减。但 Splendor 的 `game.json` 使用嵌套结构：

```json
"temperature_schedule": {
    "enabled": true,
    "initial": 1.0,
    "final": 0.1,
    "decay_plies": 30
}
```

平坦键 `temperature_initial` 不存在，pipeline 得到默认值 -1.0，C++ 侧判断为"未启用"，温度衰减被静默跳过。

### 修复

添加 `_get_temperature_key(train_cfg, key, default)` 辅助函数：先查找平坦键 `temperature_{key}`，不存在则回退到 `temperature_schedule.{key}`。两种配置格式均可正确读取。平坦键优先，确保向后兼容。

### 教训

**配置读取必须有对应的测试**。pipeline 读取的每个 `game.json` 键都应该有测试验证实际值是否到达 C++ 侧。静默的默认回退是最危险的 bug 类别之一。

---

## [BUG-009] pipeline.py 重写丢失三项训练改进

**状态**：已修复
**文件**：`training/pipeline.py`
**严重程度**：高 — 三项 AlphaZero 标准训练技术被静默丢弃

### 问题描述

修复 BUG-007 时重写了 `pipeline.py` 的样本收集逻辑。重写过程中遗漏了三项此前已实现的训练改进：

1. **Legal mask masking**：`train_step()` 不再对非法动作的 policy logits 做 mask（`masked_fill(mask == 0, -1e9)`）。结果：网络可以在非法动作上分配概率，policy loss 包含无意义的梯度信号。
2. **AdamW 优化器**：优化器从 `AdamW`（带 weight decay 的 L2 正则化）退化为 `Adam`。结果：缺失正则化，训练后期容易过拟合。
3. **梯度裁剪**：`clip_grad_norm_` 调用丢失。结果：训练不稳定时梯度爆炸无保护。

### 根因分析

三项改进的代码散落在 `train_step()` 和 `run_training_loop()` 中，没有独立的单元测试保护。重写 `pipeline.py` 时注意力集中在样本收集逻辑的修复上，没有逐行比对旧代码。

### 如何发现

通过测试驱动发现：编写 `test_sample_collection.py` 时为每个训练特性写了独立断言（检查 optimizer 类型为 AdamW、train_step 参数列表包含 legal_mask、grad_clip_norm 参数被使用），这些断言立即暴露了缺失。

### 修复

```python
# 1. Legal mask masking（train_step 内）
if legal_mask is not None:
    policy_logits = policy_logits.masked_fill(legal_mask == 0, -1e9)

# 2. AdamW 优化器（run_training_loop 内）
weight_decay = train_cfg.get("weight_decay", 1e-4)
optimizer = torch.optim.AdamW(net.parameters(), lr=learning_rate, weight_decay=weight_decay)

# 3. 梯度裁剪（train_step 内）
if grad_clip_norm > 0:
    torch.nn.utils.clip_grad_norm_(net.parameters(), grad_clip_norm)
```

### 教训

**每个行为特性都需要独立的测试保护，而非只依赖集成测试**。BUG-007 的 fix 通过了"selfplay 能跑、训练 loss 能降"的集成测试，但这些粗粒度的测试无法检测到 legal mask、optimizer 类型、梯度裁剪等细节是否存在。对于训练管线，应该有：

- `train_step` 的参数完整性断言（接受 legal_mask、grad_clip_norm）
- optimizer 类型检查（`isinstance(optimizer, torch.optim.AdamW)`）
- legal mask 功能验证（非法动作概率接近 0）
- 梯度裁剪效果验证（大梯度被截断）

这些断言编写成本极低，但能防止重写时的静默退化。

---

## [BUG-010] pipeline.py 重写丢失 Replay Buffer

**状态**：已修复
**文件**：`training/pipeline.py`
**严重程度**：高 — 自博弈训练无法累积学习

### 问题描述

BUG-007 修复时重写了 `pipeline.py`，丢失了跨步累积的 replay buffer。旧代码维护一个 `deque(maxlen=100000)` 的滑动窗口 buffer，每步新 selfplay 样本追加到 buffer，训练从整个 buffer 采样。重写后变为每步只用当前步的 ~7000 个样本训练，训完就丢弃。

### 根因分析

重写注意力集中在样本收集逻辑（`encode_state` → `sample["features"]`），没有注意到 replay buffer 的存在。原代码中 buffer 是一个模块级别的 list 加手动 truncation，容易被忽略。

### 如何发现

v10 训练 150 步后对比 warm start 模型和 step 50/100/150 模型的 eval 战绩，发现完全一致（35% constrained win rate vs heuristic）。模型权重在变（ONNX 文件 hash 不同），但策略没有改善。

### 影响

- 每步只从 ~7000 个样本学习 3 次（3 个 batch × 2048），数据利用率极低
- 模型每步都在"从头学"当前步的数据，无法在多步间累积知识
- 旧代码（v6）日志显示 `samples=100000`（buffer 满载），新代码显示 `samples=6916`（仅当前步）

### 修复

```python
replay_buffer: deque[tuple] = deque(maxlen=episodes_per_step * 50 * 20)

for step in range(1, steps + 1):
    # ... selfplay ...
    for sample in ep["samples"]:
        replay_buffer.append((feats, policy, z, mask, aux))
    
    # 从 buffer 随机抽 train_batches_per_step 个 batch 训练
    buf = list(replay_buffer)
    for b in range(train_batches_per_step):
        idx = torch.randint(n, (batch_size,))
        train_step(net, optimizer, feat_tensor[idx], ...)
```

配置项：
- `train_batches_per_step`（默认 3）：每步从 buffer 抽几个 batch
- `batch_size`（默认 2048）：每个 batch 的大小

### 教训

BUG-009 的教训同样适用：**重写代码时必须逐行比对旧代码**。这已经是 pipeline 重写丢失的第四项特性（legal mask、AdamW、梯度裁剪、replay buffer）。replay buffer 尤其危险——没有它训练也能跑、loss 也能降，但模型不会真正进步。

---

## [BUG-011] ONNX 未编译导致 MCTS 使用均匀策略

**状态**：已修复
**文件**：`setup.py`, `engine/infer/onnx_policy_value_evaluator.cpp`
**严重程度**：致命 — 神经网络完全没有参与 MCTS 搜索

### 问题描述

`setup.py` 默认 `BOARD_AI_WITH_ONNX=0`，需要手动设置环境变量 `BOARD_AI_WITH_ONNX=1` 和 `BOARD_AI_ONNXRUNTIME_ROOT` 才能编译 ONNX 支持。未设置时，`OnnxPolicyValueEvaluator` 在运行时静默回退到均匀策略（`priors = uniform, values = 0`）——**不报错、不警告、返回 true**。

结果：MCTS 搜索完全不使用神经网络。selfplay 生成的是纯搜索数据（无 NN 引导），eval 中所有模型行为完全相同（因为都是均匀 MCTS）。

### 如何发现

不同训练步骤（warm, step50, step100, step150）的 constrained eval 结果逐局完全一致（winner、total_plies 完全相同）。即使一个是随机初始化模型、另一个是训练 150 步后的模型。

验证方式：
1. 检查 .so 文件中是否存在 `"onnx runtime not enabled at build time"` 字符串 → 存在
2. 检查 `"dino_onnx_eval"` 字符串（ONNX 环境名称） → 不存在
3. `otool -L` 检查动态库依赖 → 无 onnxruntime

### 根因分析

`OnnxPolicyValueEvaluator::evaluate()` 在 ONNX 未编译时，静默返回均匀策略并返回 `true`。调用者无法区分"ONNX 正常运行"和"回退到均匀"。这是一个**静默降级**设计缺陷。

### 修复

1. **`setup.py`**：添加自动检测逻辑，检查 `/opt/homebrew` 和 `/usr/local` 是否存在 onnxruntime 头文件。如果存在则自动启用 ONNX，无需手动设置环境变量。未找到时打印 WARNING。
2. **`onnx_policy_value_evaluator.cpp`**：构造函数在 ONNX 未编译时抛出 `std::runtime_error`（而非静默设置 ready=false）。`evaluate()` 所有失败路径均抛异常（不再有 `return false`）。
3. **`net_mcts.h`**：删除 `UniformPolicyValueEvaluator` 类。不再存在均匀策略回退。
4. **`bindings/py_engine.cpp`**：`run_selfplay_episode`、`run_arena_match`、`run_constrained_eval_vs_heuristic` 在 `model_path` 为空时直接 `throw std::invalid_argument`。

### 教训

**静默降级是最危险的设计模式**。当一个关键子系统不可用时，系统应该 fail-fast 而不是继续运行。以下代码模式应被视为 bug：

```cpp
// 错：静默降级为无效数据
if (!ready_) {
    priors = uniform;
    return true;  // 调用者以为一切正常
}

// 对：告诉调用者此功能不可用
if (!ready_) {
    return false;  // 调用者可以采取补救措施
}
```

---

## [BUG-012] model_init.onnx 导出顺序错误

**状态**：已修复
**文件**：`training/pipeline.py`
**严重程度**：低 — 只影响 init 模型文件的正确性，不影响训练

### 问题描述

`pipeline.py` 在 warm-start 训练完成后才导出 `model_init.onnx`。此时 `net` 已经包含 warm-start 训练后的权重，所以 `model_init.onnx` 和 `model_warm.onnx` 完全相同（同一个 md5 hash）。

```python
# 修复前（错误）：
net = create_model(...)   # 随机权重
# ... warm start training ...
export_onnx(net, "model_warm.onnx")   # warm 权重
export_onnx(net, "model_init.onnx")   # 也是 warm 权重！相同 md5
current_model_path = "model_init.onnx"

# 修复后（正确）：
net = create_model(...)   # 随机权重
export_onnx(net, "model_init.onnx")   # 真正的初始权重
# ... warm start training ...
export_onnx(net, "model_warm.onnx")   # warm 权重
current_model_path = "model_warm.onnx"
```

### 影响

- `model_init.onnx` 不代表真正的初始（随机）模型
- 这与 BUG-011 结合时，使调试更加困难——即使手动比较 init 和 warm 的输出，也看不到差异
- 训练本身不受影响（`current_model_path` 指向的模型权重是对的）

---

## 通用踩坑事项

### 1. UndoToken 的 undo_depth 必须在 push 前设置

```cpp
UndoToken token{};
token.undo_depth = static_cast<std::uint32_t>(s->undo_stack.size());  // push 之前！
// ... push UndoRecord ...
// ... 修改状态 ...
return token;
```

如果在 push 之后设置 undo_depth，undo 时会弹出错误的记录。

### 2. state_hash 必须是确定性的

对于相同的游戏状态，`state_hash()` 必须始终返回相同的哈希值。常见错误：
- 忘记哈希某个影响游戏走向的字段（如 current_player）
- 使用了内存地址或指针值
- 没有处理 `include_hidden_rng` 参数

### 3. game.json 的 feature_dim 和 action_space 必须和 encoder 一致

`config/game.json` 中的 `feature_dim` 和 `action_space` 必须精确匹配 `IFeatureEncoder` 的 `feature_dim()` 和 `action_space()` 返回值。不一致会导致：
- 训练时 PyTorch 模型输入/输出维度错误
- ONNX 推理时 tensor shape mismatch crash

### 4. 不要做棋盘旋转（canonicalize_action）

早期版本 `IFeatureEncoder` 提供了 `canonicalize_action` / `decanonicalize_action` 钩子，允许把棋盘旋转到「当前玩家视角」。实测这是陷阱：

- 格子旋转容易写对，但和格子绑定的附加结构（如 Quoridor 墙的「挡哪两条边」的语义）非常容易旋转错
- 写错时训练看上去能跑，loss 正常下降，但某一方的策略永远学不出来（因为旋转后的动作 id 对应的物理语义根本和 encoder 看到的局面不匹配）
- 这类 bug 极难被发现，Quoridor 训不动就是这么来的

正确做法：**不旋转棋盘**。视角处理只做「我的特征 / 对手的特征」的交换（把 perspective_player 放前面，对手放后面），再加一个 scalar 特征告诉网络「我是先手还是后手」（或等价的「我走哪个方向」）。网络自己会学到 P0/P1 的不对称，省下来的复杂度远超过这个 scalar 的代价。

### 5. do_action_fast 和 undo_action 必须完美逆操作

`undo_action` 必须将状态精确恢复到 `do_action_fast` 之前的状态。常见遗漏：
- 忘记恢复 `current_player`、`winner`、`terminal`
- 忘记恢复 score 数组
- 忘记清除放置的棋子/墙/牌

这个 bug 特别隐蔽：MCTS 的 tree search 重度依赖 do/undo 循环，状态恢复不完整会导致搜索树污染，表现为莫名其妙的走法。

### 6. 随机游戏的 rng_nonce() 必须在每次随机事件后变化

`default_stochastic_detector` 通过比较 `rng_nonce()` 的变化来检测随机转移。如果你的游戏有隐藏信息或随机事件（如翻牌、抽卡），`rng_nonce()` 必须在每次这类事件后返回不同的值。否则 NoPeek 系统无法正确工作。

### 7. 使用 checked_cast 而非 static_cast 做状态类型转换

引擎提供了 `board_ai::checked_cast<T>(state)` 辅助函数，它在 cast 失败时抛出 `std::invalid_argument`。在 Rules 和 Encoder 的实现中，始终使用 `checked_cast` 而非 `static_cast` 来确保类型安全。

### 8. legal_actions 在 terminal 状态必须返回空

如果 `is_terminal()` 为 true，`legal_actions()` 必须返回空 vector。否则 selfplay 循环不会正确终止，可能导致无限循环或崩溃。

### 9. 多人变体的 feature_dim 和 2p 不同

如果你的游戏支持 3p/4p 变体，**每个变体的 feature_dim 通常不相等**。因为 encoder 会为每个对手编码独立的特征通道——2p 有 1 个对手通道，3p 有 2 个，4p 有 3 个。

实测数据（单位：float 个数）：

| 游戏 | 2p | 3p | 4p |
|------|-----|-----|-----|
| Splendor | 295 | 355 | 415 |
| Azul | 163 | 235 | 307 |

**影响**：
- `game.json` 中的 `feature_dim` 只记录了 2p 的值——因为 3p/4p 变体共享同一个 `game.json`
- 多人变体的 encoder 在运行时报告正确的 `feature_dim()`，**网络创建和 ONNX 导出必须使用 encoder 报告的值**，不能从 config 文件读
- **每个人数变体需要独立训练独立的网络**——2p 模型无法用于 3p/4p 对局（tensor shape 不匹配）

**常见错误**：

```python
# 错：用 config 里的 feature_dim 创建 3p 模型
cfg = load_game_config("splendor")  # feature_dim=295, 但 3p 实际是 355
net = PVNet(cfg["feature_dim"], ...)  # shape 错误

# 对：用 encoder 报告的实际值
info = dinoboard_engine.encode_state("splendor_3p", seed=42)
net = PVNet(info["feature_dim"], ...)  # 355, 正确
```

**建议**：如果要支持多人变体训练，每个变体应有独立的训练配置（或由 pipeline 动态查询 encoder 的 feature_dim），不能假设和 2p 相同。

---

## [BUG-013] ONNX 不是每步导出，selfplay 用旧模型

**状态**：已修复
**文件**：`training/pipeline.py`
**严重程度**：高 — 训练数据与网络严重脱节

### 问题描述

`run_training_loop` 只在 `step % save_every == 0` 时导出 ONNX 并更新 `current_model_path`。`save_every` 默认等于 `eval_every`（默认 50）。这意味着 step 1~49 的 selfplay 全部使用同一个旧模型（warm/init），但 PyTorch 网络已经更新了 ~150 次梯度。

### 修复

每步训练后都导出到 `model_latest.onnx`，selfplay 立刻用新权重。`save_every` 只控制是否额外保存 `model_step_NNNNN.onnx` 存档。

---

## [BUG-014] best model 路径指向 latest 文件被覆盖

**状态**：已修复
**文件**：`training/pipeline.py`
**严重程度**：高 — gating 机制完全失效

### 问题描述

Gating 通过时 `best_model_path = eval_model`，而 `eval_model` 指向 `model_latest.onnx`。下一步训练重新导出 `model_latest.onnx` 后，`best_model_path` 指向的文件内容就变成了新模型。best 永远等于 latest，eval 对弈变成自己打自己。

### 修复

Gating 通过时用 `shutil.copy2()` 复制到 `model_best.onnx`，best 的内容不会被后续训练覆盖。

---

## [BUG-015] pipeline.py 用 z_values 取值但 C++ 不总是填充

**状态**：已修复（两次）
**文件**：`training/pipeline.py`
**严重程度**：致命 — 训练直接崩溃

### 问题描述

清理兜底逻辑时，将 sample 的 value target 提取改为 `z_vals = sample["z_values"]; z = z_vals[sample["player"]]`。但 C++ 侧 `z_values`（per-player 值向量）只在 `is_terminal()` 路径填充。adjudicator 判定和非终局截断路径返回空列表。

### 修复历史

**第一次修复**：统一使用标量 `sample["z"]`（+1/-1/0）。

**第二次修复（N 维 value head）**：value head 改为 N 维输出后，训练需要 per-player 向量而非标量。改为使用 `rotate_z_values(z_vals, player, num_players)`，空 `z_values`（非终局截断）降级为全零向量。adjudicator 路径的 `z_values` 同时修复为零和（BUG-018）。

### 教训

z_values 为空的路径（非终局截断，无 adjudicator）仍然存在。`rotate_z_values` 必须处理空列表情况。

---

## [BUG-016] legal mask 被 filter 缩小导致 free 模式失效

**状态**：已修复
**文件**：`engine/runtime/selfplay_runner.cpp`
**严重程度**：高 — 模型无法学会在完整动作空间下游戏

### 问题描述

`selfplay_runner.cpp` 中 `encoder->encode()` 传入的 `legal` 来自 `effective_rules.legal_actions()`——当 training filter 生效时，`legal` 是过滤后的子集。这导致训练样本的 `legal_mask` 只在 filtered 动作上为 1。

训练时 `train_step()` 用 `legal_mask == 0` 的位置填 `-1e9`，这些位置在 softmax 后概率趋近 0，不参与 cross entropy 梯度计算。被 filter 排除的合法动作从未收到任何梯度信号，logit 保持随机初始化值。

在 free 模式（不用 filter）下，这些随机 logit 参与 softmax 计算，可能产生较高概率，导致模型选择垃圾动作。表现为 heuristic_free eval 胜率 0%，gating 对局全部平局（双方都乱走到超时）。

### 根因分析

`legal_mask` 的设计本意是排除**真正不合法的动作**。被 filter 排除的动作是合法但较差的动作，不应该被 mask 掉——模型需要通过 cross entropy 梯度学到这些动作的概率应为 0。

### 修复

`encoder->encode()` 始终传入完整的 `rules.legal_actions()`（不经过 filter），只在 MCTS 搜索和动作选择时使用 filtered rules。被过滤动作在 policy target 中 visits 为 0，cross entropy 梯度自然将其概率压低。

### 教训

mask 机制有两种语义：(1) "不合法，不存在"——应该 mask 掉；(2) "合法但不好"——应该让模型学到概率为 0。Training filter 属于后者，不能复用 legal mask 通道。

---

## [BUG-017] SplendorBeliefTracker 偷看牌堆内容

**分类**：游戏层（Splendor）— 开发新游戏时请参考此案例避免 tracker 读 state 隐藏字段
**状态**：已修复
**文件**：`games/splendor/splendor_net_adapter.cpp`
**严重程度**：高 — AI 精确知道牌堆组成，等于作弊

### 问题描述

`SplendorBeliefTracker::randomize_unseen` 直接读取 `data.decks` 来构建 unseen pool——等于 AI 知道牌堆里有哪些牌。belief tracker 本应是"玩家的记忆"，只通过 `init` 和 `observe_action` 积累信息来推导 unseen pool。

```cpp
// 修复前（偷看）：
for (auto cid : d.decks[tier]) {
    unseen_pool.push_back(cid);  // 直接从真实牌堆读
}

// 修复后（正确）：
for (int cid = 0; cid < 90; ++cid) {
    if (seen_cards_.find(cid) == seen_cards_.end() && card_pool[cid].tier == tier) {
        unseen_pool.push_back(cid);  // 从 全卡池-seen 推导
    }
}
```

### 根因分析

初始实现把 `randomize_unseen` 当成"shuffle 已知内容"，但正确语义是"基于观察推导可能的内容并采样"。两者在单机自我对弈中看似等价（state 对自己可见），但在对外 API 场景（真实隐藏状态在别人服务器上）下完全不可行。

### 修复方案

1. `SplendorBeliefTracker` 增加 `seen_cards_: std::unordered_set<int>` 和 `initialized_: bool` 成员
2. `init(state, player)` 首次调用时扫描所有公开位置（tableau + visible reserved + 自己的 reserved）建立 seen set
3. `observe_action(before, action, after)` 增量追踪新揭示的卡：
   - BuyFaceup/ReserveFaceup → 比较 state_after 的 tableau 与 state_before，新出现的 card_id 加入 seen
   - ReserveDeck 且 actor == perspective_player → 新预留的暗牌 card_id 加入 seen
4. `randomize_unseen(state, rng)` 用 `全卡池(90) - seen_cards_` 按 tier 分组构建 unseen pool，shuffle 后回填

### 验证

新增 5 个测试（`TestSplendorBeliefTracker`）验证：
- 随机化后的牌堆组成与真实牌堆不同（证明不偷看）
- tableau 卡不出现在随机化牌堆中
- 牌堆大小不变
- 多次随机化产生不同结果
- 无重复卡牌

### 教训

**belief tracker 是玩家的记忆，不是上帝视角**。`randomize_unseen` 的正确语义是"根据我所知推测未知"，不是"重排我已知的真相"。测试验证方式：`全卡池 - seen` 与 `真实 deck 内容` 在有牌被购买后必然不同——如果每次都相同，说明在偷看。

---

## [BUG-018] Adjudicator z_values 不零和 + 标量 value head 的 3p+ 展开 bug

**状态**：已修复
**文件**：`engine/runtime/selfplay_runner.cpp`、`engine/infer/onnx_policy_value_evaluator.cpp`
**严重程度**：高 — 3+ 人游戏的训练和搜索都有误

### 问题描述

三个相关联的多人游戏 bug：

1. **Adjudicator z_values 不零和**：adjudicator 路径赋值 `+1/-1` 给赢家/输家，但 3+ 人游戏的零和目标应为 `+1/-1/(n-1)`。例如 3 人游戏：赢家 +1，输家应为 -0.5 而非 -1。sum 为 0 而非 -1。

2. **标量 evaluator 展开 bug**：ONNX evaluator 的标量分支展开为 `[v, -v, -v, ...]`，但 3+ 人游戏应展开为 `[v, -v/(n-1), -v/(n-1), ...]`。

3. **N 维分支缺少 perspective 旋转**：evaluator 的 N 维分支（`value_len >= num_players`）直接用 `value_ptr[p]` 当绝对顺序，但模型输出是 perspective-relative（与 encoder 旋转对齐），需要旋转回绝对顺序。

### 修复

1. Adjudicator: `adj_vals[p] = (p == winner) ? 1.0f : -1.0f / (n-1)`
2. 标量分支: `opponent_v = -v / (n-1)`
3. N 维分支: `values[(perspective + i) % n] = value_ptr[i]`
4. 两条 z_values 路径（terminal + adjudicator）加零和 assert

### 关联改动

配合 N 维 value head 实现：value head 从 `nn.Linear(prev, 1)` 改为 `nn.Linear(prev, num_players)`，训练 target 从标量 `z` 改为 `rotate_z_values(z_values, player)`。2 人游戏不受影响（`-v/(2-1) = -v`）。

---

## [BUG-019] 多人模式全链路 2p 硬编码

**状态**：已修复
**文件**：`engine/runtime/heuristic_runner.cpp`、`bindings/py_engine.cpp`、`training/pipeline.py`、`platform/game_service/sessions.py`、`platform/game_service/routes.py`、`platform/game_service/pipeline.py`、`platform/static/general/sidebar.js`、`platform/static/general/app.js`
**严重程度**：高 — 3+ 人游戏全链路无法正确运行

### 问题描述

三处独立的 2-player 硬编码阻碍了 3+ 人游戏的正确运行：

1. **heuristic_runner.cpp adjudicator z_values**：与 BUG-018 中 selfplay_runner 相同的 bug —— adjudicator 路径给输家赋 `-1.0f` 而非 `-1.0f/(n-1)`，导致 3p+ 训练数据不零和。

2. **arena match pybind 只支持 2 个模型**：`run_arena_match_py` 接受 `model_path_0`/`model_path_1` 两个参数，无法用于 3p+ gating/eval。C++ 侧的 `run_arena_match` 已支持 N 个 player config，但 pybind 接口是瓶颈。

3. **Web 前端全链路假设 2 人**：session 数据模型使用单一 `ai_player: int`；routes 中 `1 - ai_player` 计算对手；pipeline 分析中 value 取反仅对 2 人零和正确；前端 JS 用 `state.aiPlayer` 单值判断 AI 回合。

### 修复

1. **heuristic_runner.cpp**：adjudicator z_values 改为 `(winner) ? 1.0f : -1.0f/(n-1)`，加零和 assert。

2. **arena pybind**：`run_arena_match_py` 改为接受 `model_paths: list[str]` 和 `simulations_list: list[int]`，加载 N 个 evaluator，factory 用 `player % n_eval` 分配。`training/pipeline.py` 的 `_worker_arena` 和 `run_eval_batch` 同步适配——candidate 轮流坐每个座位，其余填 opponent。

3. **Web 全链路**：
   - session 数据模型：`ai_player: int` → `human_player: int` + `ai_players: list[int]`
   - routes：`CreateGameRequest` 改为 `human_player` 参数；AI 回合判定改为 `current_player in ai_players`
   - pipeline：AI 连续落子循环（while loop 直到轮到人类）；3p+ 跳过 probe analysis（value 取反不适用）
   - 前端 sidebar：新增人数选择按钮组（仅 max > 2 时显示）；多人时座位选择替代先手/后手
   - 前端 app.js：`state.aiPlayer` → `state.aiPlayers`（array）+ `state.humanPlayer`；所有 AI 判定改为 `includes()`

### 教训

在框架"支持多人"的高层设计之后，每个组件（训练数据生成、评估、pybind 接口、web session 模型、前端交互）都需要独立审计多人路径。C++ 核心正确不代表上层接口正确。

---

## [BUG-020] Pipeline 分析使用错误的 stats key 导致 expert AI 卡死

**状态**：已修复
**文件**：`platform/game_service/pipeline.py`
**严重程度**：高 — expert 难度 AI 完全无法走子

### 问题描述

Expert 难度 AI 在落子后 pipeline 立即进入 `error` 状态，表现为 AI 永远不走。根因是 `_analyze_user_move()` 中访问 `precompute_result["stats"]["best_action_value"]`，但 pybind 导出的 key 实际上是 `"best_value"`。

C++ 结构体字段名为 `NetMctsStats::best_action_value`，但 `bindings/py_engine.cpp` 第 630 行导出时做了重命名：`st["best_value"] = stats.best_action_value;`。pipeline.py 使用了 C++ 字段名而非 Python 导出名，导致 `KeyError: 'best_action_value'`。

三处均受影响（pipeline.py 第 134、148、178 行）：
- precompute 结果读取
- fallback inline MCTS 结果读取
- post-move probe 结果读取

### 修复

将 `pipeline.py` 中所有 `["stats"]["best_action_value"]` 替换为 `["stats"]["best_value"]`。

**复发（2026-05-03）**：前端 `platform/static/general/pipeline.js:37-38` 犯了**完全相同的错**——读 `st.ai_stats.best_action_value` 而不是 `best_value`。表现为 Web 对局里"对手预估胜率"永远是 "--"。已修复。这证明教训里说的"新增代码时确认实际 key"在实际开发里很容易被忘——同一个 pit 隔几周在不同文件里被踩了两次。

### 教训

pybind 导出层可能对字段名做重命名。引用 C++ 导出数据时，应以 Python 侧实际拿到的 key 为准，而非 C++ 结构体字段名。这个坑**任何消费 MCTS stats 的代码都可能踩**（后端 pipeline.py、前端 pipeline.js、未来的日志/分析工具等），两次已经验证。

**防止再次复发的查表**——`stats` dict 的 Python 侧 key（见 `bindings/py_engine.cpp` 中 `get_ai_action` 的 `st[...]`）：

| Python key | C++ struct 字段 |
|-----------|----------------|
| `simulations` | `simulations_done` |
| `best_value` | `best_action_value` ← 重命名 |
| `root_values` | `root_values` |
| `action_values` | `root_edge_values`（展开成 `{action_id: [values]}` dict） ← 重命名 + 结构变化 |
| `tail_solved` | `tail_solved` |
| `tail_solve_value` | `tail_solve_value` |
| `traversal_stops` | `traversal_stops` |

**调试姿势**：新增读 stats 的代码之前先 `print(st.keys(), st['ai_stats'].keys())` 一次，或直接查上面这张表。别猜。

---

## [BUG-021] fly 动画继承源容器尺寸，导致 Azul 砖巨大化 + 顺序播放卡顿

**状态**：已修复
**文件**：`platform/static/general/animate.js`，`games/azul/web/azul.js`
**严重程度**：中 — 前端可用性问题，不影响训练 / AI 决策，但 Azul 对局手感非常糟糕

### 问题描述

Azul 前端每次出动作后，飞行的砖动画看起来"巨大"，而且拿的砖、落到 center 的剩余砖、first-player 令牌是**一个接一个**播的，整局下来每步等 1s+ 才能继续。

两个子 bug：

1. **尺寸 bug**：`animate.js::stepFly` 用 `fromRect.width/height` 作为飞行元素的 css 尺寸。Azul `describeTransition` 的 `from` 指向整个工厂 disc（`[data-factory="0"]`，~120×120px），结果飞的砖是 120×120 的巨型 blob，而棋盘上的 `.tile` 实际只有 34×34。

2. **顺序播放 bug**：`describeTransition` 给每个颜色去向推一个 fly step，`playTransition` 逐步 `await`，每步 350ms + 60ms 间隔。如果一次出牌有 1 个目标色 + 3 个去 center 的剩余色 + FP 令牌，总耗时 ~1700ms，直观感受是"一个一个飞"。

### 修复

**框架层** (`animate.js`):
- 新增 `flyGroup` step type：`flights[]` 数组里的所有子飞行并行 `Promise.all`，总耗时 = 最慢的一条
- `fly` step 支持可选 `width`/`height` 字段，显式指定时覆盖 `fromRect` 的继承。默认行为不变（继承 rect 尺寸），Splendor 等现有游戏不受影响
- 飞行定位从"左上角对齐"改为"中心对齐 fromRect / toRect"——小 flyer 在大容器里不再跑偏到角落

**Azul 前端** (`azul.js`):
- 把原来的多个 sequential fly step 合并为一个 `flyGroup`（选中色去目标 + 剩余色去 center + FP 令牌去地板，全部并行）
- 每个 flight 明确 `width: 34, height: 34`（= 棋盘 tile 实际尺寸）

### 教训

1. **飞行动画不应该默认继承源容器的 bounding rect**——当 `from` 是一个大容器（工厂 disc、对手 board 区）而飞行的"逻辑对象"是小单位（一块砖、一个 gem）时，尺寸会错得离谱。应该以飞行元素自身的 CSS 尺寸或调用方显式指定为准。
2. **多段连续 fly 默认顺序播放对桌游来说太慢**：一个真实物理动作（从一堆砖里拿同色、剩的推中间）在桌面上是一瞬间发生，不是慢动作分解。新游戏接入动画时，**凡是一个游戏动作物理上同时发生的多个位移，应该用 `flyGroup`，不是多个 `fly` 串联**。
3. 新游戏如果 `from` 选择器指向容器而非单体元素，写 `describeTransition` 时必须显式传 `width`/`height`，否则就会撞这个坑。新游戏验收时应该实际在浏览器里走几步看动画，不能只靠单元测试通过。
4. 框架层的默认行为（继承 rect 尺寸）保留是为了向后兼容 Splendor——Splendor 的 `from` 一般就是 token 或卡牌本身，大小合适。但该默认行为是"陷阱型"默认——以后可能考虑改成必须显式传尺寸，或者把默认行为改为"从 createElement 的元素自然尺寸推导"。

---

## [BUG-022] cancel_pipeline 误清 precompute，导致 expert 第一手 AI 响应多 8 秒

**状态**：已修复
**文件**：`platform/game_service/pipeline.py`
**严重程度**：中 — 前端性能问题，不影响训练和 AI 决策质量

### 问题描述

Expert 难度下，人类作为先手走第一步后，AI 要等 10 秒左右才落子。按配置（analysis=500 sims, expert=500 sims）理论耗时应该 < 100ms。

### 根因

`signal_cancel()` 在同一个函数里做了两件事：取消 pipeline 阶段机 + 清空 precompute 缓存。而 `apply_action` 路由处理人类动作时第一步调 `cancel_pipeline()`（为了确保没有在跑的 pipeline worker 还会写 session），间接触发了 `precompute_clear()`。

踩坑流程：
1. 游戏创建时人类先手 → `schedule_precompute(初始局面)` 立刻后台跑 MCTS，~30ms 完成，结果缓存在 `sess["precompute"]["result"]`
2. 人类点击落子 → `POST /action` → `apply_action` 首行 `cancel_pipeline(sess)`
3. `cancel_pipeline → signal_cancel → precompute_clear` —— 把刚刚缓存的 precompute 结果清成 `None`
4. Pipeline worker 进入 `_analyze_user_move`，while 循环等 `pc["result"] is not None`
5. 等不到 → 撑满 8 秒 deadline → 触发 fallback inline MCTS
6. 总耗时 = 8 秒等待 + fallback MCTS + AI decision ≈ 10 秒

AI 后续每一步就正常了（因为从 pipeline 结束开始，precompute 是在 AI 回合之后重新 schedule 的）。

### 修复

`signal_cancel` 里移除 `precompute_clear` 调用。precompute 是独立于 pipeline 的后台任务（在 `PRECOMPUTE_EXECUTOR`，与 `PIPELINE_EXECUTOR` 不共享状态），pipeline 取消不应该波及它。

真正需要清 precompute 的场景（undo / state 重建）在 `step_back` handler 里原本就显式调用 `precompute_clear`，不依赖 `cancel_pipeline` 的副作用，所以删除后不会回归 undo 逻辑。

### 教训

1. **副作用打包到"看起来该一起做"的函数里是坑**：`cancel_pipeline` 在语义上只负责 pipeline，但它悄悄顺带清 precompute，调用方不看实现就猜不到。修复后 `signal_cancel` 的 docstring 明写"does NOT touch precompute — callers that need it clear must call precompute_clear() explicitly"。
2. **这个 bug 的症状（"第一手慢 10 秒"）很容易被错误归因到"AI 计算慢"**。排查时发现 10 秒 ≈ 8 秒等待 + 两次实际 MCTS 时，8 秒这个具体数字暴露了是 deadline 超时而不是计算量爆炸。调试类似问题时，**对着"奇怪的整数时长"回头查代码里的等待上限**是一个有效快捷方式。
3. precompute 和 pipeline 应该是两个解耦的任务系统。代码里分两个 ThreadPoolExecutor 是对的，但 helper 函数混在一起就没起到隔离作用。以后如果要做类似并发架构，helper 函数的命名要严格反映其副作用边界。

---

## [BUG-024] GameSession MCTS 搜索在真实状态上跑，应隔离为 AI view

**状态**：已修复（架构重构）
**文件**：`bindings/py_engine.cpp` GameSessionWrapper
**严重程度**：严重（BUG-023 的根因）
**关联**：BUG-023 是这个根因在 Love Letter 上的具体表现

### 问题描述

发现 BUG-023 后用户追问："那之前训练的时候自博弈也有这个问题吗"、"本质上模拟游戏的地方和你跑 AI 的地方根本不在一起"。深挖发现：

- **AI API 路径**（`test_ai_api_separation` 覆盖的那条）和 **GameSession 路径**（web / selfplay 用的）**共享同一份 MCTS 代码**（`NetMcts::search_root`）和同一个 `GameSessionWrapper`
- 所谓"API 模式隔离"其实没有物理隔离 —— `bundle_->state` 在两种模式下都持有完整的隐藏字段。API 模式之所以"看起来没 bug"只是因为 seed 生成的 `bundle_->state` 里对手的隐藏字段是"任意合法占位"，对 AI 而言是无信号的随机值；而 GameSession 模式下那里装的是真相
- MCTS `search_root(*bundle_->state, ...)` 直接在真相状态上展开。rules.apply 读 `d.hand[opp]`、`d.reserved[opp][i]` 等隐藏字段时拿到的是真实值
- NoPeek 是事后挡板，依赖 nonce 变化判断"这条边需要随机化"。任何走不到 nonce 变化路径的动作（BUG-023 的 terminal-by-elimination、Splendor 的 BuyReserved 不抽新牌、等等）都是潜在漏点
- 每出现一个同类 bug 修一次是打地鼠，根本做法是 **MCTS 搜索用的状态必须和游戏真相是两个对象**

### 修复（最终架构：ISMCTS-A Method 1 + 2b）

经过迭代收敛到下面这套。关键认知来自用户反复质询："搜索能看见真相本身就是越界"、"对手不同隐藏牌合法集不一样你怎么办"、"对手采样进 UCB 的 prior 是第一次到达决定的吗"。

**第一层——ai_view 架构隔离**（GameSessionWrapper 内）：

```cpp
std::unique_ptr<IGameState> bundle_->state;                      // 真相
std::vector<std::unique_ptr<IGameState>> ai_views_;              // 每个视角一份 AI view
std::vector<std::unique_ptr<IBeliefTracker>> ai_trackers_;
std::vector<std::unique_ptr<IFeatureEncoder>> ai_encoders_;
std::vector<std::unique_ptr<OnnxPolicyValueEvaluator>> ai_evaluators_;
```

- 构造时：对每个 perspective p 从真相克隆后走 `initial_observation_extractor`/`applier` 把观察者不该看到的字段掩成占位。其他 per-perspective 对象从 fresh bundle 偷 unique_ptr（不改变堆地址，encoder 内部 raw ptr 照旧有效）
- `apply_action`：真相侧推进；同时对每个 perspective 经 `public_event_extractor(truth_before, action, truth_after, p)` 提取观察事件，按 `pre → action → post` 顺序在 ai_view 上回放

**第二层——ISMCTS-A 搜索**（`engine/search/net_mcts.cpp`）：

- 每次 simulation 起点：`sim_state = clone(ai_view); randomize_unseen(sim_state, per_sim_rng)`——从 belief 采一个具体世界
- 搜索过程中 `rules.apply(sim_state, action)` 严格按规则结算，读隐藏字段时读的是**这次 sim 采样的值**。**不跳过、不 chance-random-pick**
- 节点首次扩展时请求网络对全 action space 输出 prior，UCB 用 per-sim `legal_actions(sim_state)` 过滤；不同 world 的 legal 集不同是被这个过滤吸收的
- Prior floor `0.01/|A|`：防止"首次访问那个 world 下不合法"的动作 prior 被压到 0，保证后续 sim 里一旦合法了仍有机会被探索
- Paranoid `validate_action` 断言：过滤失败时崩溃而不是 silent apply

**第三层——Observer-hash 树共享**（`IGameState::state_hash_observer(perspective)`）：

新增虚方法，返回"观察者视角下 information set"的 hash（排除观察者看不见的隐藏字段）。MCTS 用它做树节点键 + `chance_children` 分流。效果：不同采样世界的对手节点共享一个 tree node，Q/N 跨世界聚合，避免树按采样世界分裂导致有效搜索深度稀释。Love Letter 和 Splendor 各自覆盖实现。

**三条路径统一到 ISMCTS-A**：
- `GameSessionWrapper::get_ai_action`（web / 实时）
- `run_selfplay_episode_py` → `selfplay_runner`（训练数据生成）
- `run_arena_match_py` → `arena_runner`（eval / model 对比）

都在 `NetMctsConfig` 里塞 `root_belief_tracker` + `root_observer_perspective` + `full_action_space`。旧的 NoPeek `traversal_limiter` 在 ISMCTS-A 路径下置 nullptr（不删除，给没 belief_tracker 的纯物理随机游戏留着）。

**API 模式兼容**：`external_obs_mode_` flag，`apply_initial_observation` / `apply_observation` / `apply_event` 会翻 true。外部调用方通过事件协议驱动 `bundle_->state`，此模式下 bundle_->state 就是 AI view，MCTS 走老路径（测试用）

同时 **revert 了 BUG-023 的 loveletter nonce bump**。新架构让那个补丁变成无用代码——即使规则里的 `apply()` 读了 `d.hand[target]`，读的也是 `ai_views_[p]` 里的占位值而不是真相。BUG-023 的黑盒测试（Guard 命中率 ~14% 而不是 76%）依然通过。

### 连带好处

1. **彻底解决一整类 bug**：Love Letter Guard/Baron/King/Prince、Splendor BuyReserved（包括 `legal_actions` 读隐藏字段的泄漏）、以及任何未来"读隐藏字段但不消耗 RNG"的动作都被架构层拦住。开发者不再需要在游戏的 rules 里做 defensive 的 `++nonce`
2. **selfplay 训练一并修复**：selfplay 也走 `GameSessionWrapper`（via `run_selfplay_episode_py` → `GameSessionWrapper` 指导 search），同样受益。之前污染的 Love Letter 训练数据建议重训一次得到干净模型
3. **web UI 的 "AI 只看到该看到的" 成为架构保证**：物理层面 MCTS 拿不到 `bundle_->state` pointer
4. **未来 Coup 的 bluff-biased 采样直接插在 `init_ai_views_` 的 `randomize_unseen` 钩子上**，与框架一致，不需要专门路径

### 回归测试

**统计层**：`tests/test_hidden_info_coup_loveletter.py::TestLoveLetterGuardAccuracy::test_guard_accuracy_not_better_than_bounded_inference`——跑 60 局 vs 随机对手，无先验信息时 Guard 命中率必须 < 40%。BUG-023 时是 76%，当前架构下是 ~20%，留 2 倍以上 headroom。

**不变性层**：`tests/test_api_mcts_policy_invariance.py::test_api_mcts_policy_matches_selfplay`——直击"selfplay 猛如虎 / API 变弱"的信息泄漏病症。流程：
1. 用 seed_gt 跑 selfplay，记录观察历史 + 每个 ply 的 MCTS visit distribution（在 GameSession 真相驱动路径下计算）
2. 用 seed_api（不同）起 API 会话，replay 同一观察历史
3. 在 perspective 行动的 ply 上对比两条路径各自 `get_ai_action` 的 argmax：要求偏差率 ≤ 40%（MCTS tie-breaking RNG 下是可容忍噪声；真正的泄漏会让一条路径系统性选"凑巧好"的动作）

两测合起来：统计层抓"作弊具体症状"（Guard 命中率），不变性层抓"两条路径是否真的走同一个信息"。

### 性能成本

每个 session 多持有 N 个 `IGameState` 克隆 + N 个 tracker + N 个 encoder + N 个 evaluator（N = 玩家数，2–4）。ONNX 模型 N 倍显存（对当前 1-2MB 级别的小模型可接受）。每次 `apply_action` 多做 N 次事件提取+应用。Love Letter / Splendor 级别游戏未观察到明显性能影响。如果未来上大模型或者玩家数更多，可以考虑共享 evaluator 但 per-perspective 切换 encoder。

### 教训

1. **测试通过 ≠ 架构正确**：三层测试（API 契约 / belief 等价 / encoder 不泄漏）都是必要条件，不是充分条件。MCTS 实际搜索用的状态是不是隔离的，需要单独的黑盒测试覆盖（见 `TestLoveLetterGuardAccuracy`）。
2. **"现有 API 模式测试过了，让其他地方也用 API 模式的路径"不一定对**：本次第一版方案就是这么说的，但深挖发现 API 模式自己也没物理隔离，只是"对手字段是随机占位"的偶然掩盖。正确方案是直接在 `GameSessionWrapper` 层做物理隔离。
3. **当用户反复让检测某个怀疑方向时，比起反复跑既有测试、应该主动设计一个能直接测量该怀疑的新测试**。BUG-023 本来如果更早写黑盒命中率测试，就不需要用户催五次。
4. **架构层修复优先于补丁层修复**。BUG-023 单个修的话是 5 行 nonce bump，但一眼看上去不知道是不是还有同类 bug 漏。架构修完，一整类根本问题清零。

---

## [BUG-023] Love Letter AI 永远猜对 Guard — terminal-by-elimination 漏过 NoPeek 检测

**分类**：游戏层（Love Letter）— 只在 ISMCTS-v1 / NoPeek 架构下成立，**ISMCTS-v2 重构后不再可能**（root 采样不依赖 rng_nonce 触发）。保留作为"游戏规则中隐藏-信息-依赖动作要触发随机化"的历史教训
**状态**：已修复（后被 ISMCTS-v2 整体架构替代）
**文件**：`games/loveletter/loveletter_rules.cpp`
**严重程度**：严重 — AI 对隐藏信息游戏直接读取真实状态决策，相当于作弊

### 问题描述

用户实测发现 Love Letter AI 打 Guard 时命中率异常高（后经量化：无 Priest/Baron 先验的情况下命中率 76%，随机基线 14.3%）。既有两层 AI API 分离测试（`test_ai_api_separation` 和 `test_api_belief_matches_selfplay`）全部通过，说明 API 契约层面没有泄漏；feature encoder 直检也确认不经由特征通道泄漏对手手牌。

### 根因

问题在 **NoPeek traversal limiter 的激活条件**。框架用"rng_nonce 是否改变"判定 stochastic 转移（`default_stochastic_detector`），只有跨越 stochastic 边界时才 `randomize_unseen` + 重新应用动作。

但 Love Letter 的 Guard 正确猜中→对手淘汰→`advance_turn` 开头 `check_end_game` 发现 2p 终局→直接 return，**不抽牌**→`draw_nonce` 不变→stochastic_detector 返回 false→NoPeek 不触发→MCTS 看到的 child 是用**真实手牌**算出来的终局 win 结果（Q=1.0）。

对比猜错分支：对手不死→`advance_turn` 抽牌→nonce 变→NoPeek 正常触发→随机化后重算 Guard→有一定概率命中→Q ≈ 1/7。

结果：MCTS 把"猜中对应的 guess"这条分支估得 Q=1.0，其他 guess 都是 ~0.14。AI 每次都精确选中真实手牌那一个 guess。本质上 MCTS 偷看了一次真实状态来做局部决策。

同类问题存在于 Baron（比大小直接淘汰到终局）、Prince（牌堆空了 draw 不到会不抽，此时无 nonce 变化）、King（交换手牌无 draw）。Priest 不受影响——其效果是 `hand_exposed[target]=1`，并不依赖隐藏信息做分支。

### 修复

`loveletter_rules.cpp::do_action_fast` 在 switch 之后、`advance_turn` 之前，对 Guard/Baron/Prince/King 无条件 `++d.draw_nonce`。这样任何读取过隐藏手牌的动作都会产生 nonce 变化，NoPeek 正常触发并随机化对手手牌。

修复前后实测（2p，80 盘，AI=player0，对手随机）：

| 指标 | 修复前 | 修复后 | 基线 |
|------|--------|--------|------|
| Guard 无先验命中率 | 76.0% | 14.0% | 14.3% |
| tracker 有先验时的命中率 | ~100% | ~100% | — |

修复后命中率严格匹配 1/7 基线；有 Priest/Baron 先验时仍然 100% 命中（合法使用公开信息）。

### 教训

1. **nonce-based stochastic detector 的语义是"存在 RNG 消耗"，而我们真正需要的语义是"转移结果依赖观察者不知道的信息"**。这两者大多数时候等价（draw 是最常见的隐藏消耗），但在 terminal-by-elimination 或 deck-empty 的边界上不等价。此类游戏中所有"读隐藏手牌"的动作都需要显式 nonce 增量。
2. **两层 API 分离测试不能抓这类 bug**。现有测试覆盖"API 契约是否携带隐藏字段"和"observation-only belief 是否等价于 selfplay belief"，但不覆盖 "MCTS 在 GameSession 路径下实际搜索的世界是否真的用了 belief 而不是 true state"。修复后应补一个"AI 决策不应显著优于无先验基线"的统计测试。
3. **用户主观"AI 太强"的反馈在隐藏信息游戏里永远是硬信号**，要优先怀疑泄漏，不要先辩护"也许是合理推断"。本次排查先假设是合理的 Priest 先验推断（对 AI 有利的解释），险些漏掉 bug；直到跑量化检查才暴露。
4. **新游戏接入 Checklist 里应加一条**：对隐藏信息读取型动作（Guard/Baron/King 类），确认 nonce 会在应用时变化。在 `docs/NEW_GAME_TEST_GUIDE.md` 里加一个对应测试模板。
5. 现有 Coup 暂停开发的理由（诈唬核心游戏 uniform-sampling ISMCTS 不适用）是更抽象的 bias 问题；本 bug 是具体实现问题。两者都属于 ISMCTS 采样逻辑的潜在坑，开发隐藏信息游戏时都要盯。

---

## [BUG-025] pipeline.py `nopeek_enabled` off-by-one：peek_steps=0 被错误解读为"第 0 步 peek"

**分类**：框架层
**状态**：已修复
**文件**：`training/pipeline.py:439`
**严重程度**：高 — peek 模式在 ISMCTS-v2 下让 MCTS 在 truth 上搜索，破坏 DAG hash 的 info-set 语义；训练第一步直接崩

### 问题描述

peek_steps 字段语义：**前 N 步训练用 peek（MCTS 看真相），之后切回 ISMCTS**。默认 0 表示始终 ISMCTS。

pipeline.py 里原来写的：

```python
"nopeek_enabled": step > peek_steps,
```

当 `peek_steps=0, step=0`（训练第一步）时，`0 > 0 = False` → `nopeek_enabled=False` → peek 模式被**意外开启**。意思变成了"前 1 步用 peek"。

### 根因

off-by-one：`step > N` 表达的是 "step 不在前 N+1 个里面"，但 "前 N 步" 的正确判定是 `step < N`，取反（不在前 N 步 → ISMCTS 开启）就是 `step >= N`。

### 修复

```python
"nopeek_enabled": step >= peek_steps,
```

加了注释说明 off-by-one caveat。

### 教训

"前 N 步做某事" 这类表达翻译成代码时要显式写 `step < N`，不要用 `step > N-1` 之类的等价变形——容易和边界对不上。测试应该覆盖 `peek_steps=0`（完全不 peek）的 case：训练第 0 步 nopeek_enabled 必须为 True。

---

## [BUG-026] ISMCTS-v2 DAG hash collision → MCTS 选中非法 action 崩溃

**分类**：框架层
**状态**：已兜底修复（真正的 Hasher 强化是未来工作）
**文件**：`engine/search/net_mcts.cpp::search_root`
**严重程度**：高 — 启用 Dirichlet 噪声的隐藏信息游戏训练中概率触发（Love Letter 2p 训练第 5 步就挂）

### 问题描述

ISMCTS-v2 用 DAG 节点共享：同一 `state_hash_for_perspective(current_player)` 的 sims 复用同一节点的 edges（UCB 统计在该节点聚合）。训练中偶尔报：

```
MCTS: selected action failed validate_action — node/state inconsistency
```

诊断 dump 显示：

```
Node edges=[46(Princess), 32(Baron-self)]
Current legal_actions=[45(Countess)]
```

两个 sim 都到达这个 DAG 节点，但 sim_state 的 (hand, drawn_card) 不同：
- 第一个 sim：手牌是 `(Princess, Baron)` 或 `(Baron, Princess)` 且全部 opp protected → legal 包含 Princess 和 Baron-self（fallback 自选）。Expansion 按这套 legal 建边
- 第二个 sim：手牌是 `(Countess, King)` → must_countess 强制 → legal 只剩 Countess

两套 state 在 `Hasher::combine`（XOR + shift）下**产生了相同 hash**。DAG 复用了第一个 sim 的 edges。UCB 选中 Princess → 在第二个 sim 的 state 上非法 → 崩。

### 根因

`Hasher::combine(uint64_t v)`：`seed_ ^= v + kGoldenRatio64 + (seed_ << 6) + (seed_ >> 2)`。64 位空间下理论上可碰撞；Love Letter 私有字段组合（hand × drawn_card = 约 64 种）和公共字段某些序列相乘，碰撞概率非零。Dirichlet 噪声让 sims 探索更多分支，放大了碰撞触发概率——关噪声 + greedy 选择时观察不到。

### 修复（兜底）

把硬 `throw` 改成**重新选边**：

```cpp
if (!rules.validate_action(*sim_state, chosen_action)) {
  auto legal = rules.legal_actions(*sim_state);
  std::unordered_set<ActionId> legal_set(legal.begin(), legal.end());
  // Re-pick best UCB edge restricted to currently-legal actions.
  int fallback = -1; float fb_score = -inf;
  for (each edge) if (legal_set.count(edge.action)) {
    float s = q + u; if (s > fb_score) fallback = i;
  }
  if (fallback < 0) {
    // Zero overlap — terminate this simulation at leaf with value estimate.
    break;
  }
  best_edge = fallback;
}
```

兜底**不影响 correctness**：碰撞时 stale edges 只是这一次 sim 被忽略，visit 统计和 value 更新都在真正合法的动作上。UCT2 的 DAG 共享即使在碰撞下仍然渐进正确。

### 真正的 root-cause fix（未做，未来 work）

强化 `Hasher` 抗碰撞：
- 选项 A：`combine` 用更强的混合（如 xxhash 或 MurmurHash3 的 finalize 步骤）
- 选项 B：`state_hash_for_perspective` 在 finalize 后做一轮额外 avalanche
- 选项 C：用 128-bit hash 拼接，节点 key 用 pair

当前兜底的 cost：每次碰撞多一次 legal_actions 调用（O(legal) 的小成本），碰撞率估计 < 0.01%，实战可忽略。

### 教训

- **DAG + hash 共享节点的搜索**永远要在 edge 传播前 validate，不能假设"hash 相同 → state 行为相同"。这是 ISMCTS-v2 设计的 invariant **要**然成立但**不能**假设 100% 无碰撞
- 隐藏信息游戏 + Dirichlet 噪声是最容易暴露 hash collision 的组合，因为噪声扩大了搜索分支、不同 sim 走到同 info-set 的机会增多
- 诊断要早一点写——第一次挂的时候信息几乎为零（`node/state inconsistency`），加 dump 之后立刻看明白是 edge set vs legal set 不匹配

### 剩余未做

- `tests/test_dag_hash_collision.py`：构造已知碰撞的状态对（如果能找到）断言 fallback 路径触发且 MCTS 正常完成
- Hasher 升级到 128-bit 或更强混合

---

> 历史上这里列有 DESIGN-001..DESIGN-004 四条"设计取舍"（tail solve margin_weight、selfplay 必走 C++、web 平台禁全局锁做重计算、gating 模型管理）。这些已成为项目长期原则，统一记录在 `CLAUDE.md` 里，不再在 KNOWN_ISSUES 中重复。本文件聚焦"踩过的坑"而非"一直遵循的原则"。
