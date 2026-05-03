#include <stdexcept>
#include <string>
#include <vector>

#include "../../engine/core/game_registry.h"
#include "azul_state.h"
#include "azul_rules.h"
#include "azul_net_adapter.h"

namespace {

using board_ai::AnyMap;
using board_ai::ActionId;
using board_ai::IGameState;
using board_ai::EventPhase;
using board_ai::PublicEvent;
using board_ai::PublicEventTrace;

static const char* const kColorNames[5] = {"blue", "yellow", "red", "black", "white"};
static const char* const kColorLetters[5] = {"B", "Y", "R", "K", "W"};

template <int NPlayers>
int azul_wall_col_for_color(int row, int color_idx) {
  return (color_idx + row) % board_ai::azul::kColors;
}

template <int NPlayers>
AnyMap serialize_azul(const IGameState& state) {
  using Cfg = board_ai::azul::AzulConfig<NPlayers>;
  const auto& s = board_ai::checked_cast<board_ai::azul::AzulState<NPlayers>>(state);

  AnyMap m;
  m["current_player"] = std::any(s.current_player());
  m["is_terminal"] = std::any(s.is_terminal());
  m["winner"] = std::any(s.winner());
  m["num_players"] = std::any(NPlayers);
  m["round_index"] = std::any(s.round_index);
  m["first_player_token_in_center"] = std::any(s.first_player_token_in_center);

  std::vector<int> scores(NPlayers);
  for (int i = 0; i < NPlayers; ++i) scores[i] = s.scores[i];
  m["scores"] = std::any(scores);

  std::vector<std::vector<int>> factories(Cfg::kFactories);
  for (int fi = 0; fi < Cfg::kFactories; ++fi) {
    factories[fi].resize(board_ai::azul::kColors);
    for (int c = 0; c < board_ai::azul::kColors; ++c)
      factories[fi][c] = s.factories[fi][c];
  }
  std::vector<std::any> factories_any;
  for (auto& f : factories) factories_any.push_back(std::any(std::move(f)));
  m["factories"] = std::any(factories_any);

  std::vector<int> center(board_ai::azul::kColors);
  for (int c = 0; c < board_ai::azul::kColors; ++c) center[c] = s.center[c];
  m["center"] = std::any(center);

  std::vector<int> bag_counts(board_ai::azul::kColors, 0);
  for (auto tile : s.bag) {
    if (tile >= 0 && tile < board_ai::azul::kColors) bag_counts[tile]++;
  }
  m["bag_counts"] = std::any(bag_counts);
  m["bag_total"] = std::any(static_cast<int>(s.bag.size()));

  std::vector<AnyMap> players;
  for (int p = 0; p < NPlayers; ++p) {
    const auto& ps = s.players[p];
    AnyMap pm;
    pm["score"] = std::any(static_cast<int>(ps.score));

    std::vector<AnyMap> lines;
    for (int r = 0; r < board_ai::azul::kRows; ++r) {
      AnyMap line;
      line["length"] = std::any(static_cast<int>(ps.line_len[r]));
      line["color"] = std::any(static_cast<int>(ps.line_color[r]));
      line["capacity"] = std::any(r + 1);
      lines.push_back(std::move(line));
    }
    pm["pattern_lines"] = std::any(lines);

    std::vector<std::vector<int>> wall(board_ai::azul::kRows);
    for (int r = 0; r < board_ai::azul::kRows; ++r) {
      wall[r].resize(board_ai::azul::kColors);
      for (int c = 0; c < board_ai::azul::kColors; ++c)
        wall[r][c] = (ps.wall_mask[r] >> c) & 1;
    }
    std::vector<std::any> wall_any;
    for (auto& row : wall) wall_any.push_back(std::any(std::move(row)));
    pm["wall"] = std::any(wall_any);

    std::vector<int> floor;
    for (int fi = 0; fi < static_cast<int>(ps.floor_count); ++fi)
      floor.push_back(static_cast<int>(ps.floor[fi]));
    pm["floor"] = std::any(floor);
    pm["floor_count"] = std::any(static_cast<int>(ps.floor_count));

    players.push_back(std::move(pm));
  }
  m["players"] = std::any(players);

  return m;
}

template <int NPlayers>
AnyMap describe_azul(ActionId action) {
  using Cfg = board_ai::azul::AzulConfig<NPlayers>;
  AnyMap m;
  m["action_id"] = std::any(static_cast<int>(action));

  int source = static_cast<int>(action / (board_ai::azul::kColors * board_ai::azul::kTargetsPerColor));
  int color = static_cast<int>((action / board_ai::azul::kTargetsPerColor) % board_ai::azul::kColors);
  int target = static_cast<int>(action % board_ai::azul::kTargetsPerColor);

  m["source"] = std::any(source);
  m["color"] = std::any(color);
  m["target_line"] = std::any(target);

  bool is_center = (source == Cfg::kCenterSource);
  m["is_center"] = std::any(is_center);
  m["color_name"] = std::any(std::string(kColorNames[color]));
  m["color_letter"] = std::any(std::string(kColorLetters[color]));

  if (is_center) {
    m["source_name"] = std::any(std::string("center"));
  } else {
    m["source_name"] = std::any(std::string("factory_") + std::to_string(source));
  }

  if (target < board_ai::azul::kRows) {
    m["target_name"] = std::any(std::string("row_") + std::to_string(target + 1));
  } else {
    m["target_name"] = std::any(std::string("floor"));
  }

  return m;
}

// --- Public-event protocol for AI API belief-equivalence --------------
//
// Azul has one source of hidden randomness: the bag. At game start and at
// every round-end, factories are refilled by drawing from the bag. An AI
// API session with its own seed would produce different draws than ground
// truth, so we need to sync both events (initial factories + per-round
// refills). Azul's belief tracker is stateless (bag contents are fully
// derivable from public state); these events operate purely on state.

namespace azul_events {

using board_ai::azul::AzulConfig;
using board_ai::azul::AzulState;
using board_ai::azul::kColors;

template <int NPlayers>
std::vector<std::any> factories_to_any(const AzulState<NPlayers>& s) {
  std::vector<std::vector<int>> factories(AzulConfig<NPlayers>::kFactories);
  for (int f = 0; f < AzulConfig<NPlayers>::kFactories; ++f) {
    factories[f].resize(kColors);
    for (int c = 0; c < kColors; ++c) {
      factories[f][c] = static_cast<int>(s.factories[f][c]);
    }
  }
  std::vector<std::any> out;
  out.reserve(factories.size());
  for (auto& f : factories) out.push_back(std::any(std::move(f)));
  return out;
}

template <int NPlayers>
void overwrite_factories_from_any(AzulState<NPlayers>& s, const std::any& val) {
  auto factories_any = std::any_cast<std::vector<std::any>>(val);
  if (factories_any.size() != static_cast<size_t>(AzulConfig<NPlayers>::kFactories)) {
    throw std::runtime_error(
        "azul event: factories count mismatch (expected " +
        std::to_string(AzulConfig<NPlayers>::kFactories) + ")");
  }
  for (int f = 0; f < AzulConfig<NPlayers>::kFactories; ++f) {
    auto counts = std::any_cast<std::vector<int>>(factories_any[static_cast<size_t>(f)]);
    if (counts.size() != static_cast<size_t>(kColors)) {
      throw std::runtime_error(
          "azul event: color count mismatch for factory " + std::to_string(f));
    }
    for (int c = 0; c < kColors; ++c) {
      s.factories[f][c] = static_cast<std::uint8_t>(counts[static_cast<size_t>(c)]);
    }
  }
}

// After overriding visible tiles, recompute bag = 20-per-color minus all
// visible tiles of that color (factories, center, walls, pattern lines,
// floor, box_lid). This ensures MCTS determinizations drawing from bag
// during simulation will see a consistent pool — even if AI's own earlier
// RNG draws diverged from ground truth.
template <int NPlayers>
void recompute_bag_from_visible(AzulState<NPlayers>& s) {
  std::array<int, kColors> visible{};
  for (int f = 0; f < AzulConfig<NPlayers>::kFactories; ++f) {
    for (int c = 0; c < kColors; ++c) visible[c] += s.factories[f][c];
  }
  for (int c = 0; c < kColors; ++c) visible[c] += s.center[c];
  for (int p = 0; p < NPlayers; ++p) {
    const auto& ps = s.players[p];
    for (int r = 0; r < board_ai::azul::kRows; ++r) {
      for (int c = 0; c < kColors; ++c) {
        if ((ps.wall_mask[r] >> c) & 1U) visible[c]++;
      }
      if (ps.line_color[r] >= 0 && ps.line_color[r] < kColors) {
        visible[ps.line_color[r]] += ps.line_len[r];
      }
    }
    for (int i = 0; i < ps.floor_count; ++i) {
      const int t = ps.floor[i];
      if (t >= 0 && t < kColors) visible[t]++;
    }
  }
  // Box_lid holds tiles already "consumed" out of bag but recyclable. Keep
  // box_lid as-is; put the remainder into bag.
  for (int c = 0; c < kColors; ++c) {
    for (std::int8_t t : s.box_lid) {
      if (t == static_cast<std::int8_t>(c)) visible[c]++;
    }
  }
  s.bag.clear();
  for (int c = 0; c < kColors; ++c) {
    const int remaining = 20 - visible[c];
    for (int i = 0; i < remaining && remaining > 0; ++i) {
      s.bag.push_back(static_cast<std::int8_t>(c));
    }
  }
}

template <int NPlayers>
AnyMap extract_initial_observation(const IGameState& state, int /*perspective*/) {
  const auto& s = board_ai::checked_cast<AzulState<NPlayers>>(state);
  AnyMap out;
  out["factories"] = std::any(factories_to_any(s));
  return out;
}

template <int NPlayers>
void apply_initial_observation(IGameState& state, int /*perspective*/, const AnyMap& obs) {
  auto& s = board_ai::checked_cast<AzulState<NPlayers>>(state);
  auto it = obs.find("factories");
  if (it == obs.end()) {
    throw std::runtime_error("azul initial_observation missing 'factories'");
  }
  overwrite_factories_from_any(s, it->second);
  // At game start, center is always empty and box_lid is empty.
  for (int c = 0; c < kColors; ++c) s.center[c] = 0;
  s.box_lid.clear();
  recompute_bag_from_visible(s);
}

template <int NPlayers>
PublicEventTrace extract_events(
    const IGameState& before,
    ActionId /*action*/,
    const IGameState& after,
    int /*perspective*/) {
  const auto& sb = board_ai::checked_cast<AzulState<NPlayers>>(before);
  const auto& sa = board_ai::checked_cast<AzulState<NPlayers>>(after);
  PublicEventTrace out;
  // Round settlement increments round_index and triggers factory refill.
  // If the round advanced AND the game continues (not terminal), emit
  // a factory_refill event describing the newly drawn tiles.
  if (sa.round_index > sb.round_index && !sa.terminal) {
    AnyMap payload;
    payload["factories"] = std::any(factories_to_any(sa));
    out.post_events.emplace_back("factory_refill", std::move(payload));
  }
  return out;
}

template <int NPlayers>
void apply_event(IGameState& state, EventPhase phase,
                 const std::string& kind, const AnyMap& payload) {
  if (phase != EventPhase::kPostAction) {
    throw std::runtime_error(
        "azul: unexpected pre-action event '" + kind + "' (Azul has only post-action events)");
  }
  if (kind != "factory_refill") {
    throw std::runtime_error("azul: unknown event kind '" + kind + "'");
  }
  auto& s = board_ai::checked_cast<AzulState<NPlayers>>(state);
  auto it = payload.find("factories");
  if (it == payload.end()) {
    throw std::runtime_error("azul factory_refill missing 'factories'");
  }
  overwrite_factories_from_any(s, it->second);
  // Refill always clears center and resets first_player_token — these are
  // already handled by apply_round_settlement before we got here, so just
  // ensure center is empty (defensive).
  for (int c = 0; c < kColors; ++c) s.center[c] = 0;
  recompute_bag_from_visible(s);
}

}  // namespace azul_events

template <int NPlayers>
board_ai::GameBundle make_azul(const std::string& game_id, std::uint64_t seed) {
  board_ai::GameBundle b;
  b.game_id = game_id;
  auto s = std::make_unique<board_ai::azul::AzulState<NPlayers>>();
  s->reset_with_seed(seed);
  b.state = std::move(s);
  b.rules = std::make_unique<board_ai::azul::AzulRules<NPlayers>>();
  b.value_model = std::make_unique<board_ai::DefaultStateValueModel>();
  b.encoder = std::make_unique<board_ai::azul::AzulFeatureEncoder<NPlayers>>();
  b.belief_tracker = std::make_unique<board_ai::azul::AzulBeliefTracker<NPlayers>>();
  b.stochastic_detector = board_ai::default_stochastic_detector;
  b.enable_chance_sampling = false;
  b.state_serializer = serialize_azul<NPlayers>;
  b.action_descriptor = describe_azul<NPlayers>;
  b.public_event_extractor = azul_events::extract_events<NPlayers>;
  b.public_event_applier = azul_events::apply_event<NPlayers>;
  b.initial_observation_extractor = azul_events::extract_initial_observation<NPlayers>;
  b.initial_observation_applier = azul_events::apply_initial_observation<NPlayers>;
  return b;
}

board_ai::GameRegistrar reg_azul("azul", [](std::uint64_t seed) {
  return make_azul<2>("azul", seed);
});
board_ai::GameRegistrar reg_azul_2p("azul_2p", [](std::uint64_t seed) {
  return make_azul<2>("azul_2p", seed);
});
board_ai::GameRegistrar reg_azul_3p("azul_3p", [](std::uint64_t seed) {
  return make_azul<3>("azul_3p", seed);
});
board_ai::GameRegistrar reg_azul_4p("azul_4p", [](std::uint64_t seed) {
  return make_azul<4>("azul_4p", seed);
});

}  // namespace
