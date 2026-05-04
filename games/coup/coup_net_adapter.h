#pragma once

#include <array>
#include <random>
#include <vector>

#include "../../engine/core/belief_tracker.h"
#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"
#include "coup_state.h"

namespace board_ai::coup {

template <int NPlayers>
class CoupFeatureEncoder final : public IFeatureEncoder {
 public:
  using Cfg = CoupConfig<NPlayers>;
  int action_space() const override { return kActionSpace; }
  int feature_dim() const override { return Cfg::kFeatureDim; }
  int public_feature_dim() const override { return Cfg::kPublicFeatureDim; }
  int private_feature_dim() const override { return Cfg::kPrivateFeatureDim; }

  void encode_public(
      const IGameState& state,
      int perspective_player,
      std::vector<float>* out) const override;

  void encode_private(
      const IGameState& state,
      int player,
      std::vector<float>* out) const override;
};

// Heuristic belief tracker for Coup. Maintains a per-opponent role-signal
// count derived from public claim/challenge/reveal/exchange history and uses
// it to bias `randomize_unseen` away from uniform sampling. See
// docs/GAME_FEATURES_OVERVIEW.md §"Future Work：概率化 Belief Tracking" for
// motivation: under uniform sampling, MCTS converges to a degenerate "never
// challenge, never bluff" equilibrium on bluff-heavy games like Coup.
template <int NPlayers>
class CoupBeliefTracker final : public IBeliefTracker {
 public:
  using Cfg = CoupConfig<NPlayers>;
  void init(int perspective_player, const AnyMap& initial_observation) override;
  void observe_public_event(
      int actor,
      ActionId action,
      const std::vector<PublicEvent>& pre_events,
      const std::vector<PublicEvent>& post_events) override;
  void randomize_unseen(IGameState& state, std::mt19937& rng) const override;

  // For tests: expose signal counts.
  int signal_count(int player, int role) const {
    if (player < 0 || player >= NPlayers) return 0;
    if (role < 0 || role >= kCharacterCount) return 0;
    return signals_[player][role];
  }

 private:
  int perspective_player_ = -1;

  // Non-negative count of "evidence that opp p may hold role R". Bumped on
  // claim/challenge signals; reset to 0 when the evidence is invalidated
  // (reveal, successful challenge reshuffling the card back to deck, etc.).
  std::array<std::array<int, kCharacterCount>, NPlayers> signals_{};

  // Multi-step claim resolution requires tracking pending state across calls:
  //   stage t  : claimer plays Tax         -> pending_claimer=P, claim_role=Duke
  //   stage t+1: opponent plays Challenge  -> pending_challenger=Q
  //   stage t+2: claimer plays RevealSlotN -> extractor emits card_revealed,
  //              compare revealed role vs pending_claim_role, resolve signals.
  //   stage t+1': opponent plays Allow     -> no challenge, claim_unchallenged
  int pending_claimer_ = -1;
  int pending_claim_role_ = -1;
  bool pending_challenged_ = false;
};

extern template class CoupFeatureEncoder<2>;
extern template class CoupFeatureEncoder<3>;
extern template class CoupFeatureEncoder<4>;
extern template class CoupBeliefTracker<2>;
extern template class CoupBeliefTracker<3>;
extern template class CoupBeliefTracker<4>;

}  // namespace board_ai::coup
