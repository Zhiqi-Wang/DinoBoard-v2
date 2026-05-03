#include "loveletter_state.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace board_ai::loveletter {

namespace {

std::int8_t draw_from_deck(std::vector<std::int8_t>& deck, std::uint64_t& nonce) {
  if (deck.empty()) return 0;
  const std::uint64_t r = splitmix64(nonce);
  const size_t idx = static_cast<size_t>(r % static_cast<std::uint64_t>(deck.size()));
  const std::int8_t card = deck[idx];
  if (idx + 1 < deck.size()) {
    deck[idx] = deck.back();
  }
  deck.pop_back();
  return card;
}

}  // namespace

template <int NPlayers>
LoveLetterState<NPlayers>::LoveLetterState() {
  reset_with_seed(0xC0FFEEu);
}

template <int NPlayers>
void LoveLetterState<NPlayers>::reset_with_seed(std::uint64_t seed) {
  rng_salt = sanitize_seed(seed);
  auto& d = data;
  d.draw_nonce = sanitize_seed(seed);
  d.current_player = 0;
  d.first_player = 0;
  d.winner = -1;
  d.terminal = false;
  d.ply = 0;
  d.hand.fill(0);
  d.drawn_card = 0;
  d.alive.fill(1);
  d.protected_flags.fill(0);
  d.set_aside_card = 0;
  d.face_up_removed.clear();
  for (int p = 0; p < Cfg::kPlayers; ++p) {
    d.discard_piles[static_cast<size_t>(p)].clear();
  }
  undo_stack.clear();

  d.deck.clear();
  d.deck.reserve(kTotalCards);
  for (int card = 1; card <= kCardTypes; ++card) {
    for (int c = 0; c < kCardCounts[static_cast<size_t>(card)]; ++c) {
      d.deck.push_back(static_cast<std::int8_t>(card));
    }
  }

  d.set_aside_card = draw_from_deck(d.deck, d.draw_nonce);

  if constexpr (NPlayers == 2) {
    for (int i = 0; i < 3; ++i) {
      d.face_up_removed.push_back(draw_from_deck(d.deck, d.draw_nonce));
    }
  }

  for (int p = 0; p < Cfg::kPlayers; ++p) {
    d.hand[static_cast<size_t>(p)] = draw_from_deck(d.deck, d.draw_nonce);
  }

  d.drawn_card = draw_from_deck(d.deck, d.draw_nonce);
}

template <int NPlayers>
StateHash64 LoveLetterState<NPlayers>::state_hash(bool include_hidden_rng) const {
  const auto& d = data;
  std::size_t h = 0;
  hash_combine(h, static_cast<std::size_t>(d.current_player + 3));
  hash_combine(h, static_cast<std::size_t>(d.first_player + 5));
  hash_combine(h, static_cast<std::size_t>(d.ply + 7));
  hash_combine(h, static_cast<std::size_t>(d.winner + 11));
  hash_combine(h, static_cast<std::size_t>(d.terminal ? 1 : 0));

  for (int p = 0; p < Cfg::kPlayers; ++p) {
    hash_combine(h, static_cast<std::size_t>(d.alive[p] + 13));
    hash_combine(h, static_cast<std::size_t>(d.protected_flags[p] + 17));
    if (p == d.current_player || include_hidden_rng) {
      hash_combine(h, static_cast<std::size_t>(d.hand[p] + 19));
    } else {
      hash_combine(h, static_cast<std::size_t>(0 + 19));
    }
    for (auto c : d.discard_piles[static_cast<size_t>(p)]) {
      hash_combine(h, static_cast<std::size_t>(c + 23));
    }
    hash_combine(h, static_cast<std::size_t>(d.discard_piles[static_cast<size_t>(p)].size() + 29));
  }

  if (d.drawn_card != 0) {
    hash_combine(h, static_cast<std::size_t>(d.drawn_card + 31));
  }

  hash_combine(h, static_cast<std::size_t>(d.deck.size() + 37));
  if (include_hidden_rng) {
    hash_combine(h, static_cast<std::size_t>(d.set_aside_card + 41));
    for (auto c : d.deck) {
      hash_combine(h, static_cast<std::size_t>(c + 43));
    }
    hash_combine(h, static_cast<std::size_t>(d.draw_nonce));
    hash_combine(h, static_cast<std::size_t>(rng_salt));
  }

  for (auto c : d.face_up_removed) {
    hash_combine(h, static_cast<std::size_t>(c + 47));
  }

  return static_cast<StateHash64>(h);
}

template <int NPlayers>
int LoveLetterState<NPlayers>::current_player() const {
  return data.current_player;
}

template <int NPlayers>
int LoveLetterState<NPlayers>::first_player() const {
  return data.first_player;
}

template <int NPlayers>
bool LoveLetterState<NPlayers>::is_terminal() const {
  return data.terminal;
}

template <int NPlayers>
int LoveLetterState<NPlayers>::winner() const {
  return data.winner;
}

template <int NPlayers>
std::uint64_t LoveLetterState<NPlayers>::rng_nonce() const {
  return data.draw_nonce;
}

template struct LoveLetterData<2>;
template struct LoveLetterData<3>;
template struct LoveLetterData<4>;
template struct LoveLetterState<2>;
template struct LoveLetterState<3>;
template struct LoveLetterState<4>;

}  // namespace board_ai::loveletter
