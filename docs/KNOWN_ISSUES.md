# 已知问题与踩坑汇总

> 记录 DinoBoard-v2 开发过程中发现并修复的 bug，以及需要注意的设计取舍。
> 目的：避免未来开发者重复踩坑。

---

## 目录

1. [BUG-001] Tail Solver 转置表标志位反转
2. [BUG-002] 平局 z 值赋值错误
3. [BUG-003] 训练-评估动作空间不一致
4. [BUG-004] Heuristic Runner 缺少 Adjudicator 支持
5. [BUG-005] FilteredRulesWrapper 的 const_cast
6. [BUG-006] Replay Buffer 样本利用率
7. [DESIGN-001] Tail Solver 分差偏好 (margin_weight)
8. [DESIGN-002] 自博弈全链路必须走 C++
9. [DESIGN-003] Web 平台禁止占全局锁做重计算
10. [DESIGN-004] Gating 与模型管理
11. [BUG-007] pipeline.py 用初始局面特征训练所有样本
12. [BUG-008] Splendor temperature_schedule 被静默忽略
13. [BUG-009] pipeline.py 重写丢失三项训练改进
14. [BUG-010] pipeline.py 重写丢失 Replay Buffer
15. [BUG-011] ONNX 未编译导致 MCTS 使用均匀策略
16. [BUG-012] model_init.onnx 导出顺序错误
17. [BUG-013] ONNX 不是每步导出，selfplay 用旧模型
18. [BUG-014] best model 路径指向 latest 文件被覆盖
19. [BUG-015] pipeline.py 用 z_values 取值但 C++ 不总是填充
20. [BUG-016] legal mask 被 filter 缩小导致 free 模式失效
21. [BUG-017] SplendorBeliefTracker 偷看牌堆内容
22. [BUG-018] Adjudicator z_values 不零和 + 标量 value head 的 3p+ 展开 bug
23. [BUG-019] 多人模式全链路 2p 硬编码
24. [BUG-020] Pipeline 分析使用错误的 stats key 导致 expert AI 卡死

---

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

**永远不要在 Python 侧重新实现 C++ 已经做好的事**（参见 DESIGN-002）。features 的编码必须发生在采样点的实际游戏状态上，而 C++ selfplay runner 正是这么做的。

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

### 教训

pybind 导出层可能对字段名做重命名。引用 C++ 导出数据时，应以 Python 侧实际拿到的 key 为准，而非 C++ 结构体字段名。新增 pipeline 代码时可用 `print(result.keys(), result["stats"].keys())` 先确认实际 key。

---

## 设计取舍

### DESIGN-001: Tail Solver 分差偏好 (margin_weight)

**动机**：残局求解器默认只区分胜/负/平。当存在多条必赢路线时，选哪条无所谓。但在某些游戏（如 Quoridor）中，"赢得更多"有实际意义——例如在对手离目标更远时赢下比赛。

**方案**：终局节点评估公式改为 `terminal_value + margin_weight × auxiliary_scorer(state, perspective)`。`margin_weight` 设为很小的值（如 0.01），确保不影响胜负判定（terminal_value 为 ±1），但在等价必赢路线间偏好分差更大的。

**配置**：`game.json` 中设置 `tail_solve_margin_weight`，同时需要注册 `auxiliary_scorer`。如果没有注册 scorer 则该参数无效。

**采用条件**：tail solve 的结果只在 `|value| >= 1.0` 时采用（即证明必赢/必输）。平局或搜索未完成不会替换 MCTS 结果。触发条件仅为 `ply >= tail_solve_start_ply`。

**注意事项**：
- 安全约束：`margin_weight × max(|scorer_value|) < 1.0`，否则平局可能被误判为胜利
- `auxiliary_scorer` 没有接口层面的返回值范围限制，需要开发者自己保证上述约束
- Quoridor 用 `tanh` 确保了 (-1, 1)；如果你的 scorer 无界，需要相应减小 `margin_weight`
- 该功能由 `selfplay_runner` 自动将 `auxiliary_scorer` 作为 `margin_scorer` 传入 tail solver

### DESIGN-002: 自博弈全链路必须走 C++

**准则**：selfplay、eval vs heuristic、arena 等所有对弈路径必须完整走 C++ 实现，禁止 Python 回退。

**背景**：早期 free eval vs heuristic 使用 Python `GameSession` 逐步调用 C++ — 每一步都要 Python → C++ → Python 来回跳，有 GIL 开销和 pybind11 序列化开销。当 eval 需要上百局、每局几十步时，这个开销非常显著。

**实现**：
- `run_selfplay_episode`（C++）：selfplay 全链路
- `run_constrained_eval_vs_heuristic(constrained=True)`（C++）：约束 eval
- `run_constrained_eval_vs_heuristic(constrained=False)`（C++）：自由 eval
- `run_arena_match`（C++）：模型对弈

所有函数在 `py::gil_scoped_release` 下一次性跑完整局，GIL 释放期间不回到 Python。

**规则**：如果新增对弈路径或 eval 函数，必须在 C++ 侧实现完整对局循环。如果 C++ 侧不支持所需功能，应报错（`throw std::runtime_error`），而非写 Python 回退。

---

### DESIGN-003: Web 平台禁止占全局锁做重计算

**准则**：web 平台（`platform/game_service/`）中，永远不要持有全局锁（如 SQLite 写锁、全局 mutex）的同时进行 MCTS 搜索或其他重计算。

**背景**：MCTS 搜索可能耗时数秒到数十秒（5000 simulations）。如果在持有全局锁期间执行搜索，所有其他请求（包括不相关的 session）都会被阻塞，导致整个服务无响应。

**当前状态**：v2 web 平台使用纯内存 dict 存储 session，没有 SQLite。重计算（AI 思考、分析预计算）在 `ThreadPoolExecutor` 中执行，每个 session 有独立的 `pipeline_lock`（粒度为 session 级别）。这是正确的设计。

**规则**：
- 如果未来引入 SQLite 或其他带全局锁的存储，所有重计算必须在锁外完成——先获取所需数据，释放锁，做计算，再获取锁写回结果
- 锁的粒度应为 session 级别，不应为全局级别
- 对弈计算应在线程池中执行，不阻塞请求处理线程

---

### DESIGN-004: Gating 与模型管理

**准则**：latest model 永远不被替换。

**动机**：训练过程中需要跟踪"历史最佳"模型，用于对弈评估和最终输出。但 latest 模型（每步训练产出）必须始终保持为最新，永不回退。

**设计**：

两个独立的模型槽位：
- **latest** (`model_latest.onnx`)：**每步训练后都重新导出**，`current_model_path` 指向它。下一步 selfplay 立刻使用新权重。**永不被替换或回退**。
- **best** (`model_best.onnx`)：独立文件。初始为 init/warm model。仅在 latest vs best 对弈胜率 >= `gating_accept_win_rate`（默认 60%）时，从 latest **复制**过来。

定期存档：`model_step_NNNNN.onnx` 按 `save_every` 间隔保存，用于事后实验和分析，不参与训练流程。

**关键约束**：
- **每步必须导出 ONNX**。不导出意味着后续所有 step 的 selfplay 用的都是旧模型，训练数据和网络严重脱节。
- best 更新时必须用 `shutil.copy2()`（复制文件），而非仅更新路径指针。否则后续训练步覆盖 `model_latest.onnx` 时 best 的内容也跟着变了，gating 形同虚设。
- latest 永不被 best 替换——即使模型退化（loss 上升、win rate 下降），selfplay 仍使用最新模型。Gating 的作用是保存一个"已验证有效"的检查点，不是控制 selfplay 使用哪个模型。
- Eval 对弈的 simulations 使用最终值（不随 MCTS schedule 变化），确保评估标准一致。

**Eval 结构**：每次 eval 包含两部分——benchmark eval（可选，通过 `--eval-benchmark` 配置）和 gating eval（固定执行）。benchmark 支持 `heuristic_constrained`、`heuristic_free`、ONNX 路径，可同时指定多个。gating 是 latest vs best 对打，胜率 ≥ 阈值时更新 best。两者互不排斥。
