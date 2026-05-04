#pragma once

#include <vector>

#include "game_interfaces.h"

namespace board_ai {

// Feature encoder split into public / private halves.
//
// Motivation: the hash API already splits state into `hash_public_fields`
// and `hash_private_fields(p)`. The encoder must respect the same scope
// (only `public + current_player's private` may be read for the network
// input). Previously this was a convention enforced by code review and
// the black-box `test_encoder_respects_hash_scope`. Phase 6 promotes it
// to a structural invariant: `encode_public` has no player argument and
// MUST NOT read any player's private fields; `encode_private(p)` MUST NOT
// read any other player's private fields.
//
// Composition is provided by `encode` (non-virtual here): it assembles
// `[encode_public, encode_private(perspective)]` into the flat feature
// vector that the network consumes. Network architecture is unchanged —
// a single flat input tensor. The split is purely a code-structure
// guarantee.
class IFeatureEncoder {
 public:
  virtual ~IFeatureEncoder() = default;
  virtual int action_space() const = 0;
  virtual int feature_dim() const = 0;

  // Size of the public half. `feature_dim() == public_feature_dim() +
  // private_feature_dim()`.
  virtual int public_feature_dim() const = 0;

  // Size of one player's private half (same for all players).
  virtual int private_feature_dim() const = 0;

  // Encode fields visible to all players. May use `perspective_player`
  // for perspective-relative ordering (e.g. "is_self" flag on each
  // player slot), but MUST NOT read any player's private fields.
  virtual void encode_public(
      const IGameState& state,
      int perspective_player,
      std::vector<float>* out) const = 0;

  // Encode fields visible only to `player`. MUST read only fields owned
  // by `player` (own hand, own reserved cards, etc.). Reading another
  // player's private fields — even if they happen to be sampled into
  // state — is a separation violation.
  virtual void encode_private(
      const IGameState& state,
      int player,
      std::vector<float>* out) const = 0;

  // Composes public + perspective's private + legal mask into the flat
  // feature vector consumed by the network. Games should NOT override
  // this — override encode_public / encode_private instead.
  bool encode(
      const IGameState& state,
      int perspective_player,
      const std::vector<ActionId>& legal_actions,
      std::vector<float>* features,
      std::vector<float>* legal_mask) const {
    features->clear();
    features->reserve(static_cast<size_t>(feature_dim()));
    encode_public(state, perspective_player, features);
    encode_private(state, perspective_player, features);
    fill_legal_mask_impl(legal_actions, legal_mask);
    return static_cast<int>(features->size()) == feature_dim();
  }

 private:
  void fill_legal_mask_impl(
      const std::vector<ActionId>& legal_actions,
      std::vector<float>* legal_mask) const {
    const int a_space = action_space();
    legal_mask->assign(static_cast<size_t>(a_space), 0.0f);
    for (ActionId a : legal_actions) {
      if (a >= 0 && a < a_space) {
        (*legal_mask)[static_cast<size_t>(a)] = 1.0f;
      }
    }
  }
};

inline void fill_legal_mask(
    int action_space,
    const std::vector<ActionId>& legal_actions,
    std::vector<float>* legal_mask) {
  legal_mask->assign(static_cast<size_t>(action_space), 0.0f);
  for (ActionId a : legal_actions) {
    if (a >= 0 && a < action_space) {
      (*legal_mask)[static_cast<size_t>(a)] = 1.0f;
    }
  }
}

}  // namespace board_ai
