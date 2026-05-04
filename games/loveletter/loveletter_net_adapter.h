#pragma once

#include <array>
#include <random>
#include <vector>

#include "../../engine/core/belief_tracker.h"
#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"
#include "loveletter_state.h"

namespace board_ai::loveletter {

template <int NPlayers>
class LoveLetterBeliefTracker;

template <int NPlayers>
class LoveLetterFeatureEncoder final : public IFeatureEncoder {
 public:
  using Cfg = LoveLetterConfig<NPlayers>;
  explicit LoveLetterFeatureEncoder(const LoveLetterBeliefTracker<NPlayers>* tracker = nullptr)
      : tracker_(tracker) {}
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

 private:
  const LoveLetterBeliefTracker<NPlayers>* tracker_ = nullptr;
};

template <int NPlayers>
class LoveLetterBeliefTracker final : public IBeliefTracker {
 public:
  using Cfg = LoveLetterConfig<NPlayers>;

  void init(int perspective_player, const AnyMap& initial_observation) override;
  void observe_public_event(
      int actor,
      ActionId action,
      const std::vector<PublicEvent>& pre_events,
      const std::vector<PublicEvent>& post_events) override;
  void randomize_unseen(IGameState& state, std::mt19937& rng) const override;
  AnyMap serialize() const override;

  std::int8_t known_hand(int player) const {
    if (player < 0 || player >= Cfg::kPlayers) return 0;
    return known_hand_[player];
  }

 private:
  int perspective_player_ = -1;
  std::array<std::int8_t, Cfg::kPlayers> known_hand_{};
  // Perspective's own hand tracking. Maintained from init + events so
  // the tracker can reason about the King swap without touching state.
  std::int8_t own_hand_ = 0;
  std::int8_t own_drawn_card_ = 0;
  std::array<bool, Cfg::kPlayers> alive_tracked_{};
  bool init_once_ = false;
};

extern template class LoveLetterFeatureEncoder<2>;
extern template class LoveLetterFeatureEncoder<3>;
extern template class LoveLetterFeatureEncoder<4>;
extern template class LoveLetterBeliefTracker<2>;
extern template class LoveLetterBeliefTracker<3>;
extern template class LoveLetterBeliefTracker<4>;

}  // namespace board_ai::loveletter
