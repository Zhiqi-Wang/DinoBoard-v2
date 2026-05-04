#include "coup_net_adapter.h"

#include <algorithm>
#include <numeric>

namespace board_ai::coup {

namespace {

// Maps a claim-bearing action to the character being claimed, or -1 if the
// action itself is not a claim.
int claim_role_for_action(ActionId action) {
  if (action == kTaxAction) return kDuke;
  if (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) return kAssassin;
  if (action >= kStealOffset && action < kStealOffset + kStealCount) return kCaptain;
  if (action == kExchangeAction) return kAmbassador;
  if (action == kBlockDukeAction) return kDuke;
  if (action == kBlockContessaAction) return kContessa;
  if (action == kBlockCaptainAction) return kCaptain;
  if (action == kBlockAmbassadorAction) return kAmbassador;
  return -1;
}

// Prior multiplier per unit of signal count. prior(R) = 1 + alpha * count[R].
// With alpha=0.5 and count=3, prior=2.5 — a 2.5× bias over uniform, not
// extreme. Tuned for soft boost that keeps sampling feasible but still shifts
// MCTS toward plausible worlds.
constexpr double kSignalAlpha = 0.5;

template <int NPlayers>
int influence_count(const CoupData<NPlayers>& d, int p) {
  int n = 0;
  if (!d.revealed[p][0]) ++n;
  if (!d.revealed[p][1]) ++n;
  return n;
}

int action_type_index(ActionId action) {
  if (action == kIncomeAction) return 0;
  if (action == kForeignAidAction) return 1;
  if (action >= kCoupOffset && action < kCoupOffset + kCoupCount) return 2;
  if (action == kTaxAction) return 3;
  if (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) return 4;
  if (action >= kStealOffset && action < kStealOffset + kStealCount) return 5;
  if (action == kExchangeAction) return 6;
  return -1;
}

}  // namespace

template <int NPlayers>
void CoupFeatureEncoder<NPlayers>::encode_public(
    const IGameState& state,
    int perspective_player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const CoupState<NPlayers>*>(&state);
  if (!s || !out || perspective_player < 0 || perspective_player >= NPlayers) return;
  const auto& d = s->data;

  // Per-player public (13 each): alive, coins, inf>=1, inf>=2,
  // revealed-character counts (5), active/target/blocker/challenger (4).
  for (int pi = 0; pi < NPlayers; ++pi) {
    const int pid = (perspective_player + pi) % NPlayers;

    out->push_back(d.alive[pid] ? 1.0f : 0.0f);
    out->push_back(static_cast<float>(d.coins[pid]) / 12.0f);

    int inf = influence_count(d, pid);
    out->push_back(inf >= 1 ? 1.0f : 0.0f);
    out->push_back(inf >= 2 ? 1.0f : 0.0f);

    for (int c = 0; c < kCharacterCount; ++c) {
      int count = 0;
      for (int sl = 0; sl < 2; ++sl) {
        if (d.revealed[pid][sl] && d.influence[pid][sl] == c) ++count;
      }
      out->push_back(static_cast<float>(count));
    }

    out->push_back(d.active_player == pid ? 1.0f : 0.0f);
    out->push_back(d.action_target == pid ? 1.0f : 0.0f);
    out->push_back(d.blocker == pid ? 1.0f : 0.0f);
    out->push_back(d.challenger == pid ? 1.0f : 0.0f);
  }

  // Global public (21).
  constexpr int kStageCount = 11;
  int stage_idx = static_cast<int>(d.stage);
  for (int i = 0; i < kStageCount; ++i) {
    out->push_back(i == stage_idx ? 1.0f : 0.0f);
  }

  int action_type = action_type_index(d.declared_action);
  constexpr int kActionTypeCount = 7;
  for (int i = 0; i < kActionTypeCount; ++i) {
    out->push_back(i == action_type ? 1.0f : 0.0f);
  }

  constexpr float kMaxPlies = 200.0f;
  out->push_back(static_cast<float>(d.ply) / kMaxPlies);
  out->push_back(static_cast<float>(d.court_deck.size()) / 15.0f);
  out->push_back(d.first_player == perspective_player ? 1.0f : 0.0f);
}

template <int NPlayers>
void CoupFeatureEncoder<NPlayers>::encode_private(
    const IGameState& state,
    int player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const CoupState<NPlayers>*>(&state);
  if (!s || !out || player < 0 || player >= NPlayers) return;
  const auto& d = s->data;

  // Per-player private (5 each, in player's perspective order):
  // unrevealed character counts. Only the owner (pid == player) knows
  // their own unrevealed cards; for others we output zeros to avoid
  // leaking opp hidden info.
  for (int pi = 0; pi < NPlayers; ++pi) {
    const int pid = (player + pi) % NPlayers;
    const bool is_self = (pid == player);
    for (int c = 0; c < kCharacterCount; ++c) {
      if (is_self) {
        int count = 0;
        for (int sl = 0; sl < 2; ++sl) {
          if (!d.revealed[pid][sl] && d.influence[pid][sl] == c) ++count;
        }
        out->push_back(static_cast<float>(count));
      } else {
        out->push_back(0.0f);
      }
    }
  }
}

template <int NPlayers>
void CoupBeliefTracker<NPlayers>::init(
    int perspective_player, const AnyMap& /*initial_observation*/) {
  perspective_player_ = perspective_player;
  for (auto& row : signals_) row.fill(0);
  pending_claimer_ = -1;
  pending_claim_role_ = -1;
  pending_challenged_ = false;
}

template <int NPlayers>
void CoupBeliefTracker<NPlayers>::observe_public_event(
    int actor,
    ActionId action,
    const std::vector<PublicEvent>& /*pre_events*/,
    const std::vector<PublicEvent>& post_events) {
  // ------------------------------------------------------------------
  // Phase 1: interpret `action` itself.
  // ------------------------------------------------------------------
  const int claimed = claim_role_for_action(action);
  if (claimed >= 0) {
    // A claim action starts a new claim cycle. Any unresolved pending claim
    // (shouldn't normally happen, but be safe) is discarded.
    pending_claimer_ = actor;
    pending_claim_role_ = claimed;
    pending_challenged_ = false;
  } else if (action == kChallengeAction) {
    // Challenger implicitly signals "I may hold `pending_claim_role_`" —
    // that's why they dare to challenge.
    if (pending_claim_role_ >= 0 && actor >= 0 && actor < NPlayers) {
      signals_[actor][pending_claim_role_] += 1;
    }
    pending_challenged_ = true;
  } else if (action == kAllowAction || action == kAllowNoBlockAction) {
    // No one challenged this claim (or the target chose not to block). If
    // the current claim cycle went unchallenged, the claimer's role signal
    // strengthens.
    if (pending_claimer_ >= 0 && pending_claim_role_ >= 0 && !pending_challenged_) {
      signals_[pending_claimer_][pending_claim_role_] += 1;
    }
    // Note: if pending_challenged_ is true, allow fires in a different
    // sub-flow (e.g. allowing the aftermath of a successful challenge);
    // do nothing in that case.
    // Only clear pending when the claim cycle is fully settled. A simple
    // heuristic: Allow means "we move on from this claim" — clear.
    if (!pending_challenged_) {
      pending_claimer_ = -1;
      pending_claim_role_ = -1;
    }
  }

  // ------------------------------------------------------------------
  // Phase 2: process post_events (revealed cards, exchange completions).
  // ------------------------------------------------------------------
  for (const auto& evt : post_events) {
    if (evt.first == "card_revealed") {
      const auto& payload = evt.second;
      auto it_p = payload.find("player");
      auto it_r = payload.find("role");
      if (it_p == payload.end() || it_r == payload.end()) continue;
      int p = std::any_cast<int>(it_p->second);
      int r = std::any_cast<int>(it_r->second);
      if (p < 0 || p >= NPlayers) continue;
      if (r < 0 || r >= kCharacterCount) continue;

      // Revealed role is publicly known now; any accumulated signal for
      // (p, r) is "spent" — the earlier claims are explained by this reveal
      // rather than by additional hidden copies.
      signals_[p][r] = 0;

      // Resolve pending challenge if this reveal is the claimer's response.
      if (pending_challenged_ && p == pending_claimer_ && pending_claim_role_ >= 0) {
        // Either outcome clears the claimer's signal on the claimed role:
        //   - r == pending_claim_role_: true claim, card reshuffled back to
        //     deck; claimer no longer demonstrably holds it.
        //   - r != pending_claim_role_: bluff exposed; earlier claims of
        //     pending_claim_role_ were false signals.
        signals_[pending_claimer_][pending_claim_role_] = 0;
        pending_claimer_ = -1;
        pending_claim_role_ = -1;
        pending_challenged_ = false;
      }
    } else if (evt.first == "exchange_complete") {
      const auto& payload = evt.second;
      auto it_p = payload.find("player");
      if (it_p == payload.end()) continue;
      int p = std::any_cast<int>(it_p->second);
      if (p < 0 || p >= NPlayers) continue;
      // Ambassador reshuffled p's hand — prior claim signals are stale.
      signals_[p].fill(0);
    }
  }
}

template <int NPlayers>
void CoupBeliefTracker<NPlayers>::randomize_unseen(
    IGameState& state, std::mt19937& rng) const {
  auto* s = dynamic_cast<CoupState<NPlayers>*>(&state);
  if (!s) return;
  auto& d = s->data;

  // --------------------------------------------------------------
  // Step 1: compute `remaining[role]` = how many copies of each role
  // are unaccounted for (i.e. must be distributed among opp hands,
  // exchange_drawn for opp, and court_deck).
  // --------------------------------------------------------------
  std::array<int, kCharacterCount> remaining{};
  for (int c = 0; c < kCharacterCount; ++c) {
    remaining[static_cast<size_t>(c)] = kCardsPerCharacter;
  }

  auto consume = [&](CharId card) {
    if (card >= 0 && card < kCharacterCount &&
        remaining[static_cast<size_t>(card)] > 0) {
      remaining[static_cast<size_t>(card)]--;
    }
  };

  // All revealed cards (any player, any slot) are consumed.
  for (int p = 0; p < NPlayers; ++p) {
    for (int sl = 0; sl < 2; ++sl) {
      if (d.revealed[p][sl]) consume(d.influence[p][sl]);
    }
  }
  // Perspective's own unrevealed hand is known.
  if (perspective_player_ >= 0 && perspective_player_ < NPlayers) {
    for (int sl = 0; sl < 2; ++sl) {
      if (!d.revealed[perspective_player_][sl]) {
        consume(d.influence[perspective_player_][sl]);
      }
    }
  }
  // Perspective's own exchange_drawn cards are known to perspective.
  if (d.current_player == perspective_player_) {
    for (int i = 0; i < 2; ++i) {
      if (d.exchange_drawn[i] >= 0) consume(d.exchange_drawn[i]);
    }
  }

  // --------------------------------------------------------------
  // Step 2: enumerate slots to fill.
  //   - opp unrevealed hand slots
  //   - opp's exchange_drawn slots (if any and it's opp's exchange)
  //   - court_deck slots (size = sum(remaining) - other opp allocations)
  //
  // We resolve them in a randomized order to avoid ordering bias
  // (otherwise opp 0 would systematically grab rarer roles first).
  // --------------------------------------------------------------
  struct Slot {
    int owner;      // -1 = deck, else player index
    int slot_idx;   // meaning depends on owner:
                    //   opp hand: 0 or 1 (influence slot)
                    //   opp exchange_drawn: kExchangeDrawnBase + i (i=0,1)
                    //   deck: 0..deck_size-1
  };
  constexpr int kExchangeDrawnBase = 100;  // sentinel to distinguish from hand
  std::vector<Slot> slots;
  slots.reserve(static_cast<size_t>(NPlayers * 2 + 2 + 15));

  for (int p = 0; p < NPlayers; ++p) {
    if (p == perspective_player_) continue;
    for (int sl = 0; sl < 2; ++sl) {
      if (!d.revealed[p][sl]) slots.push_back({p, sl});
    }
  }
  if (d.current_player != perspective_player_ && d.current_player >= 0) {
    for (int i = 0; i < 2; ++i) {
      if (d.exchange_drawn[i] >= 0) {
        slots.push_back({d.current_player, kExchangeDrawnBase + i});
      }
    }
  }
  // Deck size is public — preserve it.
  const int deck_size = static_cast<int>(d.court_deck.size());
  for (int i = 0; i < deck_size; ++i) {
    slots.push_back({-1, i});
  }

  // Sanity: total slots should equal total remaining copies.
  const int total_remaining =
      std::accumulate(remaining.begin(), remaining.end(), 0);
  if (total_remaining != static_cast<int>(slots.size())) {
    // Pool / slot-count mismatch indicates an inconsistent state (e.g.
    // pre-migration bug or API applier hasn't run). Fall back to uniform
    // sampling to avoid crashing — this mirrors the old behavior.
    std::vector<CharId> unseen;
    for (int c = 0; c < kCharacterCount; ++c) {
      for (int i = 0; i < remaining[static_cast<size_t>(c)]; ++i) {
        unseen.push_back(static_cast<CharId>(c));
      }
    }
    std::shuffle(unseen.begin(), unseen.end(), rng);
    size_t idx = 0;
    auto assign_next = [&](CharId* dst) {
      if (idx < unseen.size()) *dst = unseen[idx++];
    };
    for (int p = 0; p < NPlayers; ++p) {
      if (p == perspective_player_) continue;
      for (int sl = 0; sl < 2; ++sl) {
        if (!d.revealed[p][sl]) assign_next(&d.influence[p][sl]);
      }
    }
    for (int i = 0; i < 2; ++i) {
      if (d.exchange_drawn[i] >= 0 && d.current_player != perspective_player_) {
        assign_next(&d.exchange_drawn[i]);
      }
    }
    d.court_deck.clear();
    while (idx < unseen.size()) d.court_deck.push_back(unseen[idx++]);
    d.draw_nonce ^= static_cast<std::uint64_t>(rng());
    return;
  }

  // Randomize slot order.
  std::shuffle(slots.begin(), slots.end(), rng);

  // --------------------------------------------------------------
  // Step 3: sequential weighted sampling. For each slot, weight[R] =
  // remaining[R] * prior[R]. `remaining[R]` acts as a HARD constraint
  // (zero if pool exhausted, keeping the joint sample feasible). The
  // prior is the per-opp signal-derived bias.
  // --------------------------------------------------------------
  std::vector<CharId> assignments;
  assignments.reserve(slots.size());

  for (const Slot& slot : slots) {
    std::array<double, kCharacterCount> weights{};
    double total = 0.0;
    for (int r = 0; r < kCharacterCount; ++r) {
      const int c = remaining[static_cast<size_t>(r)];
      if (c == 0) { weights[r] = 0.0; continue; }
      double prior = 1.0;
      if (slot.owner >= 0 && slot.owner < NPlayers) {
        prior += kSignalAlpha *
                 static_cast<double>(signals_[slot.owner][r]);
      }
      weights[r] = static_cast<double>(c) * prior;
      total += weights[r];
    }

    int picked = -1;
    if (total > 0.0) {
      std::uniform_real_distribution<double> dist(0.0, total);
      double u = dist(rng);
      double acc = 0.0;
      for (int r = 0; r < kCharacterCount; ++r) {
        acc += weights[r];
        if (u < acc) { picked = r; break; }
      }
      if (picked < 0) {
        // Float accumulation edge case — pick highest non-zero weight.
        for (int r = kCharacterCount - 1; r >= 0; --r) {
          if (weights[r] > 0.0) { picked = r; break; }
        }
      }
    } else {
      // Shouldn't happen given slot count == total_remaining, but
      // defensively pick any role still available.
      for (int r = 0; r < kCharacterCount; ++r) {
        if (remaining[static_cast<size_t>(r)] > 0) { picked = r; break; }
      }
    }
    if (picked < 0) picked = 0;  // last-resort: should never trigger

    assignments.push_back(static_cast<CharId>(picked));
    remaining[static_cast<size_t>(picked)]--;
  }

  // --------------------------------------------------------------
  // Step 4: write assignments back to state.
  // --------------------------------------------------------------
  d.court_deck.clear();
  for (size_t i = 0; i < slots.size(); ++i) {
    const Slot& slot = slots[i];
    CharId v = assignments[i];
    if (slot.owner == -1) {
      // Deck. We'll fill in index order below for determinism.
      // Here we temporarily assign into a sparse vector.
    } else if (slot.slot_idx >= kExchangeDrawnBase) {
      int i_draw = slot.slot_idx - kExchangeDrawnBase;
      d.exchange_drawn[i_draw] = v;
    } else {
      d.influence[slot.owner][slot.slot_idx] = v;
    }
  }
  // For the deck we need to preserve the original order of slot_idx.
  // Build a dense deck from the slot assignments.
  std::vector<CharId> deck(deck_size, -1);
  for (size_t i = 0; i < slots.size(); ++i) {
    if (slots[i].owner == -1) {
      int di = slots[i].slot_idx;
      if (di >= 0 && di < deck_size) deck[di] = assignments[i];
    }
  }
  for (CharId c : deck) d.court_deck.push_back(c);

  d.draw_nonce ^= static_cast<std::uint64_t>(rng());
}

template class CoupFeatureEncoder<2>;
template class CoupFeatureEncoder<3>;
template class CoupFeatureEncoder<4>;
template class CoupBeliefTracker<2>;
template class CoupBeliefTracker<3>;
template class CoupBeliefTracker<4>;

}  // namespace board_ai::coup
