# MCTS 算法：ISMCTS-v2 实现细节

本文档面向想理解 DinoBoard 搜索算法的开发者。读完能回答：
- 一次 `search_root` 从头到尾做了什么
- MCTS 如何同时处理"信息不对称"和"物理随机"
- DAG 节点复用是怎么实现的、为什么是 DAG 不是 tree
- UCT2 为什么不是直接抄 AlphaZero
- 新游戏该做哪些接口实现才能享受完整算法支持

如果你只是要给新游戏加 state/rules/encoder，先看 [`GAME_DEVELOPMENT_GUIDE.md`](GAME_DEVELOPMENT_GUIDE.md)，它讲"接入点"。本文讲"接入点背后发生了什么"。

配套代码：
- `engine/search/net_mcts.{h,cpp}` — 主算法
- `engine/core/game_interfaces.h` — `IGameState` / `IGameRules` 接口
- `engine/core/belief_tracker.h` — 信息追踪器接口
- `engine/core/types.h` — `Hasher` 工具

---

## 1. 算法概览

算法叫 **ISMCTS-v2**（Information-Set MCTS 第二版）。核心特性按重要程度排列：

1. **Root 采样**（determinization）：每次 simulation 开头从观察者的 belief 采一个完整世界（包括 opp 手牌 + deck 顺序 + 任何未来随机）。采样后 descent 完全确定性
2. **Per-acting-player 节点 keying**：每个决策节点用 `state.state_hash_for_perspective(state.current_player())` 作 key——哪位玩家在决策，就用那位玩家的信息集。每个节点真正代表一个合法的 info set
3. **博弈 DAG**（不是 tree）：全局 `unordered_map<StateHash64, int>` 表，同一个 `(public + current 玩家 private + step_count)` 无论从哪条路径到达都是同一个节点。visit / Q 统计跨路径聚合
4. **UCT2 UCB 公式**：`sqrt()` 分子底用"刚经过的那条入边"的 visit_count，不是 node 的 global visit_count。这是 DAG 下保持 UCB 正确性的关键调整
5. **无 chance node 专门机制**：物理随机（抽牌、掷骰）被 root 采样吞掉；不同 sim 采不同世界 → 不同观察者可见后继 → 不同 hash → 自然分叉
6. **Step counter 防环**：`IGameState::step_count_` 每次 `do_action_fast` 递增，纳入 public hash。DAG 结构性 acyclic
7. **Encoder 对齐 hash scope**：encoder 只读 `public + current player's private` 三元组以内的字段，和 hash scope 一致

这七点互锁——去掉任一个都会破坏其他。

---

## 2. 三类游戏，统一算法

DinoBoard 支持：

| 游戏类型 | 例子 | 代码开关 |
|---|---|---|
| 完全公开 + 确定 | TicTacToe、Quoridor | `cfg.root_belief_tracker == nullptr` |
| 完全公开 + 物理随机（对称不确定） | Azul（袋子） | `root_belief_tracker != nullptr`，但 `hash_private_fields` 为空 |
| 信息不对称 + 物理随机 | Love Letter、Splendor、Coup | `root_belief_tracker != nullptr`，且每游戏有非空 `hash_private_fields(p)` |

**同一套 MCTS 代码**处理三类——区别只在游戏注册时给 `GameBundle` 配了哪些字段。

---

## 3. 数据结构

```cpp
struct Edge {
  ActionId action;
  float prior;               // 网络首次 expand 时给的先验
  int child;                 // 指向 nodes[] 的 index（-1 表示这条边从未被走过）
  int visit_count;
  float value_sum;           // 累积 backup 的 Q × visits
};

struct Node {
  int to_play;               // 哪位玩家在这个节点决策（acting player）
  bool expanded;
  int visit_count;
  float value_sum;
  std::vector<Edge> edges;   // 一条 edge = 一个合法动作
};

// 实际存储：
std::vector<Node> nodes;                                  // 所有节点线性存储
std::unordered_map<StateHash64, int> node_index;          // 全局 DAG 查找表
```

关键点：
- **Edge 里没有 chance_children / child_state_hash**——DAG 查找统一走全局表，edge 不需要自己的 outcome 索引
- **每个节点一个 to_play**——这是谁的决策节点，UCB 选边是从他的 info set 看
- **`nodes[0]` 是根**

---

## 4. 一次 `search_root` 的完整流程

```
输入: root (观察者状态), rules, evaluator, value_model, NetMctsConfig, seed

【初始化】
1. 清空 nodes, node_index
2. nodes.push_back(Node{ root.current_player(), ..., edges=[] })
   node_index[root.state_hash_for_perspective(root.current_player())] = 0
3. expand_node(nodes[0], root)
   - legal = rules.legal_actions(root)
   - evaluator.evaluate(root, current_player, legal) → priors, values
   - 给每个 legal action 建一条 edge，prior 归一
4. 对 root edges 加 Dirichlet 噪声（用于训练探索）

【每次 simulation, 重复 simulations 次】
1. sim_state = root.clone()
2. 如果有 belief_tracker：
     per_sim_rng = 从 root_sample_rng 派生
     belief_tracker.randomize_unseen(sim_state, per_sim_rng)
     → sim_state 现在是一个 belief 采样出的完整世界
     → 未来 descent 纯 deterministic
3. path_nodes = [0]，path_edges = []，cur = 0
   incoming_edge_visits = nodes[0].visit_count  # root 没有入边，用总 sim 数代替
4. while depth < max_depth:
     if sim_state.is_terminal(): break
     if not nodes[cur].expanded: expand(nodes[cur], sim_state); break
     
     # UCT2 edge 选择
     sqrt_parent = sqrt(max(1, incoming_edge_visits))
     best_edge = argmax over e in nodes[cur].edges:
       q = e.value_sum / e.visit_count (if visits > 0 else 0)
       u = c_puct * e.prior * sqrt_parent / (1 + e.visit_count)
       score = q + u
     
     rules.do_action_fast(sim_state, best_edge.action)
     next_hash = sim_state.state_hash_for_perspective(sim_state.current_player())
     
     # DAG 查找
     if next_hash in node_index:
       next_idx = node_index[next_hash]        # 命中，复用节点
       dag_reuse_hits++
     else:
       nodes.push_back(Node{ sim_state.current_player(), ... })
       next_idx = len(nodes) - 1
       node_index[next_hash] = next_idx
     
     path_edges.append(best_edge)
     path_nodes.append(next_idx)
     incoming_edge_visits = nodes[cur].edges[best_edge].visit_count  # UCT2 关键
     cur = next_idx
     depth++

【backup】
5. 沿 path 从 leaf 到 root 反向走：
     node.visit_count++
     node.value_sum += leaf_values[node.to_play]
     for edge on path before this node:
       edge.visit_count++
       edge.value_sum += leaf_values[parent_node.to_play]

【选 root 最优动作】
6. argmax over root.edges.visit_count（tie 用 RNG 随机）
```

---

## 5. DAG：信息集节点如何跨路径共享

### 5.1 为什么是 DAG 不是 tree

两种分享来源：

**来源 A：信息不对称下的世界汇聚**。observer 看自己出 Handmaid → 下家决策：

- sim_1 采样 opp=Countess
- sim_2 采样 opp=Priest

下家决策节点的 key = `(public, 下家的 private)`。public 里只有 observer 能看见的东西（出牌历史、公开弃牌等），下家的 private 是下家的手牌。两个 sim 在下家决策节点，key 一样吗？

- public 部分：observer 出 Handmaid 后的公开状态，两个 sim 一样 ✓
- 下家 private 部分：下家手牌 = sim 采样的 opp 牌，两个 sim **不一样**

所以两个 sim 在下家节点会**分叉到不同节点**。这在 opp 回合是正确的——opp 真实决策确实取决于自己的手，不同 opp 手牌 = 不同决策节点。

**但**当路径再走回到 observer 的下一决策节点时：

- key = `(public, observer 的 private)`
- observer 的 private 两个 sim 一样（都是 observer 自己的手）
- 只要 public 一样（即下家做出的动作一样）→ 合并到同一节点

所以 DAG 的共享主要发生在**观察者自己的决策节点**——那里是信息集层面的统计聚合。

**来源 B：transposition（换序）**。Splendor 里"Take RGB 然后 Take WUB" vs "Take WUB 然后 Take RGB"可能到同样的公开状态。Quoridor 里墙的摆放顺序可任意。两条不同路径 → 同一 hash → 一个节点。

### 5.2 Step counter 为什么防环

DAG 的担心：若有 cycle（action 序列走回出发点），MCTS 陷入死循环。

`IGameState::step_count_` 由框架管理，每次 `do_action_fast` 里 `begin_step()` 递增。`hash_public_fields` 必须把它 hash 进去（框架在 `state_hash_for_perspective` 里自动做一次）。

这意味着：任何两个 state 只要 step_count 不同，hash 必不同。由于 do_action 永远是 step++，DAG 里 parent → child 永远 step 增加 → 不可能回到同 hash → **结构性 acyclic**。

游戏开发者不需要想 cycle——框架保证。唯一要求：
- `do_action_fast` 里调 `s->begin_step()`
- `undo_action` 里调 `s->end_step()`
- `reset_with_seed` 里重置 `this->step_count_ = 0`（template state 用 `this->`）

### 5.3 多父节点与 backup 正确性

一个 DAG 节点可能有多个入边（多条路径汇入）。单次 sim 的 backup 只走**这次 sim 的 path**，不会回传给其他入边。

效果：
- `node.visit_count` 是**所有路径的访问总和**——它说"这个 info set 被考察过多少次"
- 每条入边的 `edge.visit_count` 只记**自己被选中的次数**
- 两者关系：`sum(all_incoming_edges.visit_count) ≈ node.visit_count`（略少于，因初次到达未经边）

Q 值分别在 node 和 edge 层独立聚合：
- `node.Q = node.value_sum / node.visit_count` = 这个 info set 的期望价值
- `edge.Q = edge.value_sum / edge.visit_count` = 经过这条边后的期望价值

UCB 在父节点选边时用的是**edge 层**的 Q。node 的 Q 主要给 backup 用。

---

## 6. UCT2：DAG 下 UCB 的关键调整

### 6.1 经典 UCT / PUCT 的假设

AlphaZero 的 PUCT 公式：

$$ \text{score}(e) = Q(e) + c_{\text{puct}} \cdot p(e) \cdot \frac{\sqrt{N(\text{parent})}}{1 + N(e)} $$

其中 $N(\text{parent})$ 是父节点被访问的总次数。**在 tree 里** $N(\text{parent}) \approx \sum_{\text{child edges}} N(e)$，公式和理论 UCB1 的 regret bound 自洽。

### 6.2 DAG 里直接套的问题

DAG 里一个节点的 `node.visit_count` 汇总**多条入边**的访问。如果某节点 X 有两条入边 $e_a$ 和 $e_b$ 各访问 50 次：

- X.visit_count = 100
- 其中从 $e_a$ 来的 50 次到达 X 后选了 X 的出边
- 从 $e_b$ 来的 50 次也选了 X 的出边

当 sim 走 $e_a$ 到 X 后准备选 X 的出边时，直接用 `sqrt(X.visit_count = 100)` 相当于把"$e_b$ 这条路径的访问"也算进"我当前路径的 exploration"。**结果是 u 项被高估了 √2 倍**，过度探索。

### 6.3 UCT2 的修正

Childs, Brodeur, Kocsis 2008 "Transpositions and Move Groups in MCTS" 提出 UCT2：

$$ \text{score}(e) = Q(e) + c_{\text{puct}} \cdot p(e) \cdot \frac{\sqrt{N(\text{incoming edge})}}{1 + N(e)} $$

**$\sqrt{}$ 的分子底换成"这次 sim 刚经过的那条入边的 visit_count"**，而不是 node 的 global count。这样就反映了"我这条路径上到达当前节点有多少次"，而不是"所有路径算上"。

代码里用一个局部变量 `incoming_edge_visits` 在 descent 循环里追踪：

```cpp
int incoming_edge_visits = nodes[0].visit_count;  // root 特例
while (...) {
  // 用 incoming_edge_visits 算 UCB
  // ...
  // 选完 best_edge 后，更新为本条边的 visits（供下一 iter 用）
  incoming_edge_visits = nodes[cur].edges[best_edge].visit_count;
  cur = next_idx;
}
```

CPU 成本：0 额外（本来就要算 sqrt）。代码复杂度：追加 ~10 行。

Childs 论文实测 UCT2 vs UCT1（直接 merge）棋力差 5-15%，越容易 transpose 的游戏（Splendor、Quoridor、Azul）差距越大。

### 6.4 Root 的特例

Root 没有入边。用 `nodes[0].visit_count` 替代——等于当前已完成的 sim 数。实质等同于 AlphaZero 的 UCT1 root（因 root tree / DAG 无差异），但和后续层的 UCT2 无缝衔接。

---

## 7. 物理随机：不是 chance node，而是 sampled world

### 7.1 观察者不可见的随机（例：opp 抽牌）

```
sim_1 采样世界：deck = [C1, C2, C3, ...]
sim_2 采样世界：deck = [C4, C5, C6, ...]
```

descent 里 opp 抽牌 → `do_action_fast` 弹 `d.deck.top()`。不同 sim 拿到不同的牌（各自世界的）。opp 把牌加进自己 hand。

之后若回到 observer 决策节点，key 里**不含 opp hand**（observer private 层面），两个 sim 合并到同节点。visit 在 observer 层正确汇总。

### 7.2 观察者可见的随机（例：Splendor tier 翻新卡）

observer 买了 tier 1 slot 0 → deck 翻出新卡放到 slot 0（**公开**）。

```
sim_1: 翻出 card#12 → public state 里 tableau[1][0] = 12
sim_2: 翻出 card#37 → tableau[1][0] = 37
```

两个 sim 的 public state 不同 → `hash_public_fields` hash 不同 → 走到不同 observer 决策节点 → 自然分叉。符合"observer 确实面对两个不同公开局面"的事实。

### 7.3 为什么不需要显式 chance node

传统 chance-node MCTS 会在每个随机事件处建一个 chance 节点，按概率权重展开 children。这对**每事件分支数巨大**的游戏（7WD 每 age 翻 20 张）是必要的。但我们的系统里：

- Root 采样把"整局所有未来随机"固定成一条具体世界路径
- observer 能分辨的随机（§7.2）自动通过 hash 差异分叉
- observer 不能分辨的随机（§7.1）自动通过 hash 合并汇聚
- N 次 sim = N 条可能世界轨迹，Q 统计近似真实期望

DAG 自然承担了"chance node 该有的分叉"的工作，不需要专门的结构。这是一个关键简化。

代价：PIMC（Perfect Information Monte Carlo）bias——每条 sim 轨迹实际上"先知"了整条世界线，结果可能略偏乐观。Long et al. 2010 分析过这种 bias 在不同游戏类型下的严重程度。对我们游戏集（LL / Splendor / Azul）实测无棋力损失。

---

## 8. Encoder 对齐 hash scope

### 8.1 原则

Encoder 输入必须由 `public + current player's private` 完全决定。换句话说：**两个 state 若 hash_public_fields 和 hash_private_fields(current_player) 都 bit-equal，encoder 输出也必须 bit-equal**。

这保证同一个 DAG 节点（对应一个 info set）喂给网络的特征向量是唯一的——网络不会因"虽然是同一个 info set，但采样世界里 opp 手牌 A 或 B 不同"而给出不同的 prior / value。

### 8.2 当前实现

Encoder 接口保留 `encode(state, perspective, legal, features, mask)`——签名不变，内部约定开发者：

- 读 public 字段 OK
- 读 `state.hand[perspective]` 等 perspective-own 字段 OK
- 读 `tracker->known_hand(opp)` OK（tracker 合法知识）
- **不读** `state.hand[opp]` 直接字段（即使 observer 不知道的那些）

Phase 6（计划中，未实现）会把 encoder 拆成 `encode_public + encode_private(p)` 结构化接口，和 hash API 完全并行。在那之前我们靠：
- 约定 + code review
- 结构化测试（`tests/test_encoder_respects_hash_scope.py`）

### 8.3 为什么这点必须守

观察者 MCTS 在一个 observer-keyed 节点上跑 UCB，看到的 prior 来自网络对此节点的一次 eval。若不同采样世界走到该节点 encoder 给不同特征，网络给出的 prior 也不同——UCB 从第一个到达的 sim 的特定世界看问题，是 Method 2b prior 污染的根源。

---

## 9. 游戏开发者职责清单

按游戏复杂度分档。

### 9.1 完全公开 + 确定（TicTacToe、Quoridor）

必须：
- `IGameState` 继承，实现 `current_player/is_terminal/winner/num_players/reset_with_seed`
- `state_hash(bool include_hidden_rng)` — 常规 hash（调试、tail solver）
- `hash_public_fields(Hasher&)` — 所有公开字段 hash 进去
- `hash_private_fields(int player, Hasher&)` — **空实现**
- `reset_with_seed` 里 `step_count_ = 0`
- `IGameRules::do_action_fast/undo_action` 里调 `state.begin_step()/end_step()`

不需要：belief_tracker、public_event_extractor、initial_observation_extractor

### 9.2 公开对称随机（Azul）

§9.1 + 以下：
- `IBeliefTracker`：`init/observe_public_event` 可空实现；`randomize_unseen(state, rng)` 洗袋子
- `GameBundle::belief_tracker` 注册
- `hash_private_fields` 仍然可以空——因为"袋中剩余组成"可从公开状态推导，不算非对称 private

不需要：public_event_extractor、initial_observation_extractor（因为无非对称 hidden info）

### 9.3 信息不对称（Love Letter、Splendor、Coup）

§9.2 + 以下：
- `hash_private_fields(int player, Hasher&)` — hash player 自己的 hidden 字段（手牌、盲压牌等）
- `IBeliefTracker::init(int perspective, const AnyMap& initial_obs)` — 根据初始观察构建 belief
- `IBeliefTracker::observe_public_event(actor, action, pre_events, post_events)` — 从事件流更新 belief
- `IBeliefTracker::randomize_unseen(state, rng)` — 根据 belief 为 state 填充对手 hidden 字段
- `GameBundle::public_event_extractor` — 把 (state_before, action, state_after) diff 成事件序列
- `GameBundle::public_event_applier` — 在 API 侧把事件应用到 state
- `GameBundle::initial_observation_extractor` — 从 state 提取 perspective 可见的初始信息
- `GameBundle::initial_observation_applier` — 用 initial_observation 覆盖 state 的 hidden 部分
- Feature encoder 读 `tracker->known_*()` 而非直接读 opp hidden 字段

---

## 10. 配置项速查（`NetMctsConfig`）

| 字段 | 语义 | 典型值 |
|---|---|---|
| `simulations` | 每次 `search_root` 的 sim 数 | 200（playtest）/ 1000+（serious） |
| `c_puct` | UCB exploration 系数 | 1.4 |
| `max_depth` | 每 sim 最大 descent 深度 | 128 |
| `value_clip` | leaf value 裁剪范围 | 1.0 |
| `root_dirichlet_alpha` / `epsilon` | 根探索噪声 | 训练时 0.3 / 0.25，eval 时 0 |
| `root_belief_tracker` | 非空 → 启用 root 采样 | 由 game bundle 决定 |
| `root_observer_perspective` | 保留字段（新 keying 用 `current_player()` 直接） | -1 |
| `tail_solve_enabled` | 启用 alpha-beta 残局求解 | false / true |

`traversal_limiter` / `full_action_space` / NoPeek 相关——都没了。

---

## 11. 常见误区与调试

### 11.1 `hash_public_fields` 忘记 hash 某个公开字段

症状：不同 sim 显然公开状态不同，但节点却 merge 到一起，UCB 行为异常。

排查：
- 和 `state_hash(false)` 对比。后者通常 hash 所有字段；如果 `state_hash_for_perspective(p)` 两个不同 state 得到相同结果但 `state_hash(false)` 不同，说明 public hash 漏了字段

### 11.2 `hash_private_fields` 把其他玩家的 private hash 进去了

症状：同 info set 节点被"多出的维度"细分成多个，DAG 复用率掉到接近 0，`dag_reuse_hits` 几乎不增加。

排查：
- 严格检查 `hash_private_fields(int player, ...)` 里有没有 `for p in num_players: ... state.hand[p]` 这种循环——应该只 hash player 自己的那一份
- 用 `tests/test_encoder_respects_hash_scope.py` 辅助，若它失败说明 hash 或 encoder 泄漏

### 11.3 忘记在 `do_action_fast` 里调 `begin_step()`

症状：DAG 可能成环（两个不同 state 碰巧 hash 相同，因 step_count 都是 0）。

排查：
- 跑游戏看 `stats.expanded_nodes` 是否异常低，`dag_reuse_hits` 异常高
- 在 `search_root` 里临时加 debug 断言：descent 时 `sim_state.step_count()` 应严格 > 父节点对应的 step_count

### 11.4 DAG 访问次数 / 节点数诊断

`NetMctsStats` 里几个指标：
- `simulations_done` = sim 总数
- `expanded_nodes` = 被建出来的 node 总数
- `dag_reuse_hits` = descent 中命中已有 hash 的次数
- ratio `expanded_nodes / simulations_done`：
  - **≈ 1**：每 sim 几乎独立建子树，DAG 共享失效（可能 hash 漏字段）
  - **0.2-0.5**：典型信息集共享效果
  - **< 0.1**：共享过度（可能 hash 漏了 public 字段）

---

## 12. 参考文献

1. **Cowling, Powley, Whitehouse (2012)** — "Information Set Monte Carlo Tree Search"。ISMCTS 原始论文，SO-ISMCTS 和 MO-ISMCTS 变体。我们的"单树 per-acting-player keying"是两者之间的一个具体工程选择
2. **Childs, Brodeur, Kocsis (2008)** — "Transpositions and Move Groups in MCTS"。UCT1 / UCT2 / UCT3 的对比研究。我们选 UCT2
3. **Silver et al. (2018)** — "AlphaZero"。PUCT 公式 + 网络先验的标准套路
4. **Long et al. (2010)** — "Understanding the Success of Perfect Information Monte Carlo Sampling"。讨论 PIMC（determinization）的 strategy fusion bias
5. **Paolini et al. (2024)** — "Learning to Play 7 Wonders Duel Without Human Supervision" (ZeusAI)。对纯公开物理随机游戏用 afterstate cap；我们的框架在类似场景下靠 DAG 自然共享，不需要 cap
6. **Silver & Veness (2010)** — "Monte-Carlo Planning in Large POMDPs" (POMCP)。particle filter + UCT 的思路，和我们的 root 采样在精神上相近

---

## 13. 未来工作

**Phase 6：Encoder 结构化接口**。把 `encode` 拆成 `encode_public(Hasher&)` + `encode_private(int p, Hasher&)`，和 hash API 完全并行。消除"靠约定不读 opp hidden"的脆弱点。

**观察-perspective encoding（Method 3）**。encoder 直接喂"观察者视角"的特征（opp 手牌用 belief-marginal 而非采样值）。可完全消除 PIMC 的 strategy fusion bias，但要训练侧配合。

**Root world cap**。1k+ sim 预算下目前无限制，靠 DAG 自然共享。将来如果加 4p 复杂游戏或降低 sim 预算，可加 `cap K` 限制 opp info set 多样性。
