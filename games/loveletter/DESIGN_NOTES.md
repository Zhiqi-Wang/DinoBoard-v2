# Love Letter — Design Notes

## Modeling Scope

- Only model a single round (one deal until someone wins or deck runs out). No multi-round token collection — strategy depth is within a round, not across rounds.

## Player Elimination

- Players can be knocked out mid-round (Guard guess, Baron comparison, Princess discard). Framework currently assumes all players play until game end — this is the first game with mid-game elimination.
- `current_player` must skip eliminated players. `legal_actions` targeting must exclude eliminated players.
- Terminal value assignment for eliminated players: they lose (negative value), survivors compete for the win.

## Search Architecture（双人局）

两层不确定性，两种处理方式：

| 不确定性类型 | 处理方式 | 时机 |
|------------|---------|------|
| 信息缺口（对手手牌） | ISMCTS：每次 simulation 从 belief 采样 | simulation 开头 |
| 物理随机（抽牌） | chance node 展开（cap=10） | 遍历到随机边界时 |

对手节点的 encoder：对手看到自己的（采样的）牌，看我的牌是占位符。和 Splendor 相同的 `is_self` 逻辑。

## Belief Tracker

双人局只需维护一个方向：**我对对手手牌的认知**。不需要追踪对手对我的认知。

核心变量：`known_opponent_card`（nullable）。

知识来源与过期：
- Priest 偷看 → 固定对手手牌
- King 交换 → 固定对手手牌（我的旧牌）
- 对手打出已知牌 → 新摸的牌未知，清除
- 对手打出非已知牌 → 保持（他手里还是那张）
- Prince 强制弃牌重摸 → 清除

`randomize_unseen`：unseen = 全部 21 张 - 已打出 - 我的手牌 - 开局弃牌。若 known_opponent_card 有值则固定，否则从 unseen 均匀采样。

第一版可以完全不追踪，均匀采样。后续逐步加：Priest/King 精确知识 → Guard 排除 → Baron 范围约束。

## Feature Encoding 补偿

在我的特征中加标志位："我的手牌已被对手看过"（Priest/King 后）。NN 从训练中学到被偷看后应尽快出掉该牌。弥补框架无法建模对手知识的限制。

