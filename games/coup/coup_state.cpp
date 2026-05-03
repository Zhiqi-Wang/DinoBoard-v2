#include "coup_state.h"

#include <functional>

namespace board_ai::coup {

namespace {

std::uint64_t sanitize_seed(std::uint64_t seed) {
  if (seed == 0) seed = 0xDEADBEEF;
  seed ^= seed >> 33;
  seed *= 0xff51afd7ed558ccdULL;
  seed ^= seed >> 33;
  return seed;
}

CharId draw_from_deck(std::vector<CharId>& deck, std::uint64_t& nonce) {
  if (deck.empty()) return -1;
  const std::uint64_t r = splitmix64(nonce);
  const size_t idx = static_cast<size_t>(r % static_cast<std::uint64_t>(deck.size()));
  const CharId card = deck[idx];
  if (idx + 1 < deck.size()) {
    deck[idx] = deck.back();
  }
  deck.pop_back();
  return card;
}

}  // namespace

template <int NPlayers>
CoupState<NPlayers>::CoupState() = default;

template <int NPlayers>
void CoupState<NPlayers>::reset_with_seed(std::uint64_t seed) {
  data = CoupData<NPlayers>{};
  undo_stack.clear();
  rng_salt = sanitize_seed(seed);
  data.draw_nonce = rng_salt;

  data.court_deck.clear();
  data.court_deck.reserve(kTotalCards);
  for (CharId c = 0; c < kCharacterCount; ++c) {
    for (int i = 0; i < kCardsPerCharacter; ++i) {
      data.court_deck.push_back(c);
    }
  }

  for (int p = 0; p < NPlayers; ++p) {
    data.influence[p][0] = draw_from_deck(data.court_deck, data.draw_nonce);
    data.influence[p][1] = draw_from_deck(data.court_deck, data.draw_nonce);
    data.revealed[p] = {false, false};
    data.coins[p] = kStartingCoins;
    data.alive[p] = true;
  }

  data.current_player = 0;
  data.first_player = 0;
  data.active_player = 0;
  data.stage = CoupStage::kDeclareAction;
}

template <int NPlayers>
StateHash64 CoupState<NPlayers>::state_hash(bool include_hidden_rng) const {
  std::size_t h = 0;
  auto combine = [&](std::size_t v) {
    h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2);
  };

  combine(static_cast<std::size_t>(data.current_player));
  combine(static_cast<std::size_t>(data.stage));
  combine(static_cast<std::size_t>(data.ply));
  combine(static_cast<std::size_t>(data.active_player));
  combine(static_cast<std::size_t>(data.declared_action + 1));
  combine(static_cast<std::size_t>(data.action_target + 1));
  combine(static_cast<std::size_t>(data.challenger + 1));
  combine(static_cast<std::size_t>(data.blocker + 1));
  combine(static_cast<std::size_t>(data.challenge_check_index));
  combine(static_cast<std::size_t>(data.action_challenged));
  combine(static_cast<std::size_t>(data.action_challenge_succeeded));
  combine(static_cast<std::size_t>(data.counter_challenged));
  combine(static_cast<std::size_t>(data.counter_challenge_succeeded));

  for (int p = 0; p < NPlayers; ++p) {
    combine(static_cast<std::size_t>(data.alive[p]));
    combine(static_cast<std::size_t>(data.coins[p]));
    for (int s = 0; s < 2; ++s) {
      combine(static_cast<std::size_t>(data.revealed[p][s]));
      if (data.revealed[p][s]) {
        combine(static_cast<std::size_t>(data.influence[p][s] + 1));
      }
    }
  }

  combine(data.court_deck.size());

  if (include_hidden_rng) {
    for (int p = 0; p < NPlayers; ++p) {
      for (int s = 0; s < 2; ++s) {
        if (!data.revealed[p][s]) {
          combine(static_cast<std::size_t>(data.influence[p][s] + 1));
        }
      }
    }
    for (auto c : data.court_deck) {
      combine(static_cast<std::size_t>(c + 1));
    }
    combine(static_cast<std::size_t>(data.draw_nonce));
    combine(static_cast<std::size_t>(rng_salt));
    for (int i = 0; i < 2; ++i) {
      combine(static_cast<std::size_t>(data.exchange_drawn[i] + 1));
    }
  }

  return static_cast<StateHash64>(h);
}

template <int NPlayers>
int CoupState<NPlayers>::current_player() const {
  return data.current_player;
}

template <int NPlayers>
int CoupState<NPlayers>::first_player() const {
  return data.first_player;
}

template <int NPlayers>
bool CoupState<NPlayers>::is_terminal() const {
  return data.terminal;
}

template <int NPlayers>
int CoupState<NPlayers>::winner() const {
  return data.winner;
}

template <int NPlayers>
std::uint64_t CoupState<NPlayers>::rng_nonce() const {
  return data.draw_nonce;
}

template <int NPlayers>
bool CoupState<NPlayers>::is_turn_start() const {
  return data.stage == CoupStage::kDeclareAction;
}

template struct CoupData<2>;
template struct CoupData<3>;
template struct CoupData<4>;
template struct CoupState<2>;
template struct CoupState<3>;
template struct CoupState<4>;

}  // namespace board_ai::coup
