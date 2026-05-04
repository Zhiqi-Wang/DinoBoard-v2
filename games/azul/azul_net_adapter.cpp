#include "azul_net_adapter.h"

#include <algorithm>
#include <array>

namespace board_ai::azul {

namespace {
constexpr int kFirstPlayerToken = -2;

void append_wall_features(const PlayerState& p, std::vector<float>* out) {
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < kColors; ++c) {
      out->push_back(((p.wall_mask[r] >> c) & 1U) ? 1.0f : 0.0f);
    }
  }
}

void append_pattern_features(const PlayerState& p, std::vector<float>* out) {
  for (int row = 0; row < kRows; ++row) {
    std::array<float, kColors> counts{};
    if (p.line_color[row] >= 0) {
      counts[static_cast<size_t>(p.line_color[row])] = static_cast<float>(p.line_len[row]);
    }
    const float cap = static_cast<float>(row + 1);
    for (float v : counts) {
      out->push_back(v / cap);
    }
    out->push_back(static_cast<float>(p.line_len[row]) / cap);
  }
}

void append_floor_features(const PlayerState& p, std::vector<float>* out) {
  std::array<float, 6> counts{};
  for (int i = 0; i < p.floor_count && i < 7; ++i) {
    const int t = p.floor[i];
    if (t >= 0 && t < kColors) {
      counts[static_cast<size_t>(t)] += 1.0f;
    } else if (t == kFirstPlayerToken) {
      counts[5] = 1.0f;
    }
  }
  for (float& v : counts) {
    v /= 7.0f;
    out->push_back(v);
  }
}

}  // namespace

template <int NPlayers>
void AzulFeatureEncoder<NPlayers>::encode_public(
    const IGameState& state,
    int perspective_player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const AzulState<NPlayers>*>(&state);
  if (!s || !out || perspective_player < 0 || perspective_player >= Cfg::kPlayers) {
    return;
  }

  // Walls for all players (me first)
  for (int i = 0; i < Cfg::kPlayers; ++i) {
    const int pid = (perspective_player + i) % Cfg::kPlayers;
    append_wall_features(s->players[pid], out);
  }

  // Pattern lines for all players
  for (int i = 0; i < Cfg::kPlayers; ++i) {
    const int pid = (perspective_player + i) % Cfg::kPlayers;
    append_pattern_features(s->players[pid], out);
  }

  // Floors for all players
  for (int i = 0; i < Cfg::kPlayers; ++i) {
    const int pid = (perspective_player + i) % Cfg::kPlayers;
    append_floor_features(s->players[pid], out);
  }

  // Scores
  for (int i = 0; i < Cfg::kPlayers; ++i) {
    const int pid = (perspective_player + i) % Cfg::kPlayers;
    out->push_back(std::min(s->players[pid].score, 200) / 200.0f);
  }

  // Factories
  for (int f = 0; f < Cfg::kFactories; ++f) {
    for (int c = 0; c < kColors; ++c) {
      out->push_back(static_cast<float>(s->factories[f][c]) / 4.0f);
    }
  }

  // Center
  for (int c = 0; c < kColors; ++c) {
    out->push_back(static_cast<float>(s->center[c]) / 20.0f);
  }

  // Bag composition (aggregate count per color — observer-visible:
  // all players know how many of each color remain in the bag, only
  // the draw order is hidden).
  std::array<int, kColors> bag_counts{};
  bag_counts.fill(0);
  for (std::int8_t t : s->bag) {
    if (t >= 0 && t < kColors) {
      bag_counts[static_cast<size_t>(t)] += 1;
    }
  }
  for (int c = 0; c < kColors; ++c) {
    out->push_back(static_cast<float>(bag_counts[static_cast<size_t>(c)]) / 20.0f);
  }

  // Metadata
  out->push_back(s->first_player_token_in_center ? 1.0f : 0.0f);
  out->push_back(s->current_player_ == perspective_player ? 1.0f : 0.0f);
  out->push_back(std::min(s->round_index, 20) / 20.0f);
  out->push_back(static_cast<float>(s->bag.size()) / 100.0f);
}

template <int NPlayers>
void AzulBeliefTracker<NPlayers>::init(
    int perspective_player, const AnyMap& /*initial_observation*/) {
  perspective_player_ = perspective_player;
}

template <int NPlayers>
void AzulBeliefTracker<NPlayers>::observe_public_event(
    int /*actor*/,
    ActionId /*action*/,
    const std::vector<PublicEvent>& /*pre_events*/,
    const std::vector<PublicEvent>& /*post_events*/) {
  // Azul has no explicit belief state (bag is shuffled from full counts in
  // randomize_unseen; tracker doesn't need to maintain progress).
}

template <int NPlayers>
void AzulBeliefTracker<NPlayers>::randomize_unseen(IGameState& state, std::mt19937& rng) const {
  auto* s = dynamic_cast<AzulState<NPlayers>*>(&state);
  if (!s) return;

  std::shuffle(s->bag.begin(), s->bag.end(), rng);
  s->rng_salt ^= static_cast<std::uint64_t>(rng());
}

template class AzulFeatureEncoder<2>;
template class AzulFeatureEncoder<3>;
template class AzulFeatureEncoder<4>;
template class AzulBeliefTracker<2>;
template class AzulBeliefTracker<3>;
template class AzulBeliefTracker<4>;

}  // namespace board_ai::azul
