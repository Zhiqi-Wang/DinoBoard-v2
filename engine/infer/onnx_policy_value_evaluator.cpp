#include "onnx_policy_value_evaluator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sys/stat.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#if defined(BOARD_AI_WITH_ONNX) && BOARD_AI_WITH_ONNX
#include <onnxruntime_c_api.h>
#endif

namespace board_ai::infer {

namespace {

static void masked_softmax(
    const std::vector<float>& logits,
    const std::vector<float>& mask,
    const std::vector<ActionId>& legal_actions,
    std::vector<float>* out) {
  out->assign(legal_actions.size(), 0.0f);
  if (legal_actions.empty() || logits.empty() || mask.empty()) {
    return;
  }
  float max_logit = -std::numeric_limits<float>::infinity();
  for (ActionId a : legal_actions) {
    const int idx = static_cast<int>(a);
    if (idx >= 0 && idx < static_cast<int>(logits.size()) && idx < static_cast<int>(mask.size()) && mask[idx] > 0.0f) {
      max_logit = std::max(max_logit, logits[idx]);
    }
  }
  if (!std::isfinite(max_logit)) {
    const float u = 1.0f / static_cast<float>(legal_actions.size());
    std::fill(out->begin(), out->end(), u);
    return;
  }
  float z = 0.0f;
  for (size_t i = 0; i < legal_actions.size(); ++i) {
    const int idx = static_cast<int>(legal_actions[i]);
    if (idx >= 0 && idx < static_cast<int>(logits.size()) && idx < static_cast<int>(mask.size()) && mask[idx] > 0.0f) {
      const float e = std::exp(logits[idx] - max_logit);
      (*out)[i] = e;
      z += e;
    }
  }
  if (z <= 1e-9f) {
    const float u = 1.0f / static_cast<float>(legal_actions.size());
    std::fill(out->begin(), out->end(), u);
    return;
  }
  for (float& x : *out) {
    x /= z;
  }
}

}  // namespace

#if defined(BOARD_AI_WITH_ONNX) && BOARD_AI_WITH_ONNX
namespace {
const OrtApi& ort_api() noexcept {
  static const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  return *api;
}

void throw_on_ort_error(OrtStatus* status) {
  if (!status) return;
  const char* message = ort_api().GetErrorMessage(status);
  std::string error = message ? message : "onnxruntime error";
  ort_api().ReleaseStatus(status);
  throw std::runtime_error(error);
}

struct OrtEnvDeleter {
  void operator()(OrtEnv* ptr) const noexcept { if (ptr) ort_api().ReleaseEnv(ptr); }
};
struct OrtSessionOptionsDeleter {
  void operator()(OrtSessionOptions* ptr) const noexcept { if (ptr) ort_api().ReleaseSessionOptions(ptr); }
};
struct OrtSessionDeleter {
  void operator()(OrtSession* ptr) const noexcept { if (ptr) ort_api().ReleaseSession(ptr); }
};
struct OrtMemoryInfoDeleter {
  void operator()(OrtMemoryInfo* ptr) const noexcept { if (ptr) ort_api().ReleaseMemoryInfo(ptr); }
};
struct OrtValueDeleter {
  void operator()(OrtValue* ptr) const noexcept { if (ptr) ort_api().ReleaseValue(ptr); }
};
struct OrtTensorShapeDeleter {
  void operator()(OrtTensorTypeAndShapeInfo* ptr) const noexcept { if (ptr) ort_api().ReleaseTensorTypeAndShapeInfo(ptr); }
};

using OrtEnvPtr = std::unique_ptr<OrtEnv, OrtEnvDeleter>;
using OrtSessionOptionsPtr = std::unique_ptr<OrtSessionOptions, OrtSessionOptionsDeleter>;
using OrtSessionPtr = std::unique_ptr<OrtSession, OrtSessionDeleter>;
using OrtMemoryInfoPtr = std::unique_ptr<OrtMemoryInfo, OrtMemoryInfoDeleter>;
using OrtValuePtr = std::unique_ptr<OrtValue, OrtValueDeleter>;
using OrtTensorShapePtr = std::unique_ptr<OrtTensorTypeAndShapeInfo, OrtTensorShapeDeleter>;

std::string get_session_name(OrtSession* session, size_t index, OrtAllocator* allocator, bool input) {
  char* raw_name = nullptr;
  if (input) {
    throw_on_ort_error(ort_api().SessionGetInputName(session, index, allocator, &raw_name));
  } else {
    throw_on_ort_error(ort_api().SessionGetOutputName(session, index, allocator, &raw_name));
  }
  std::string name = raw_name ? raw_name : "";
  if (raw_name) {
    throw_on_ort_error(ort_api().AllocatorFree(allocator, raw_name));
  }
  return name;
}

struct CachedOrtSessionBundle {
  OrtEnvPtr env{};
  OrtSessionOptionsPtr opts{};
  OrtSessionPtr session{};
  std::string input_name;
  std::string policy_output_name;
  std::string value_output_name;
};
}  // namespace

struct OnnxPolicyValueEvaluator::Impl {
  std::shared_ptr<CachedOrtSessionBundle> bundle;
};

namespace {
// Heap-allocated, never destroyed — avoids static destruction order issues
// that cause "mutex lock failed" crashes at process exit.
struct OnnxSessionCache {
  std::mutex mu;
  std::unordered_map<std::string, std::shared_ptr<CachedOrtSessionBundle>> map;
};
OnnxSessionCache& session_cache() {
  static auto* cache = new OnnxSessionCache();
  return *cache;
}
constexpr std::size_t kMaxCachedOrtSessions = 8;

std::string make_session_cache_key(const std::string& model_path, const OnnxEvaluatorConfig& cfg) {
  std::string model_fingerprint = "missing";
  struct stat st{};
  if (::stat(model_path.c_str(), &st) == 0) {
    model_fingerprint = std::to_string(static_cast<unsigned long long>(st.st_size)) + "@" +
        std::to_string(static_cast<long long>(st.st_mtime));
  }
  return model_path + "|fp=" + model_fingerprint + "|intra=" + std::to_string(std::max(1, cfg.intra_threads)) +
      "|inter=" + std::to_string(std::max(1, cfg.inter_threads));
}
}  // namespace
#endif

OnnxPolicyValueEvaluator::OnnxPolicyValueEvaluator(
    std::string model_path,
    const IFeatureEncoder* encoder,
    OnnxEvaluatorConfig cfg)
    : model_path_(std::move(model_path)), encoder_(encoder), cfg_(cfg), ready_(false) {
  if (!encoder_) {
    last_error_ = "encoder is null";
    throw std::runtime_error("OnnxPolicyValueEvaluator: encoder is null");
  }

#if defined(BOARD_AI_WITH_ONNX) && BOARD_AI_WITH_ONNX
  try {
    const std::string cache_key = make_session_cache_key(model_path_, cfg_);
    std::shared_ptr<CachedOrtSessionBundle> bundle;
    {
      auto& cache = session_cache();
      std::lock_guard<std::mutex> lk(cache.mu);
      auto it = cache.map.find(cache_key);
      if (it != cache.map.end()) {
        bundle = it->second;
      }
      if (!bundle) {
        bundle = std::make_shared<CachedOrtSessionBundle>();
        OrtEnv* raw_env = nullptr;
        throw_on_ort_error(ort_api().CreateEnv(ORT_LOGGING_LEVEL_WARNING, "dino_onnx_eval", &raw_env));
        bundle->env.reset(raw_env);

        OrtSessionOptions* raw_opts = nullptr;
        throw_on_ort_error(ort_api().CreateSessionOptions(&raw_opts));
        bundle->opts.reset(raw_opts);
        throw_on_ort_error(ort_api().SetIntraOpNumThreads(bundle->opts.get(), std::max(1, cfg_.intra_threads)));
        throw_on_ort_error(ort_api().SetInterOpNumThreads(bundle->opts.get(), std::max(1, cfg_.inter_threads)));
        throw_on_ort_error(
            ort_api().SetSessionGraphOptimizationLevel(bundle->opts.get(), GraphOptimizationLevel::ORT_ENABLE_EXTENDED));

        OrtSession* raw_session = nullptr;
#if defined(_WIN32)
        std::wstring model_path_w(model_path_.begin(), model_path_.end());
        throw_on_ort_error(ort_api().CreateSession(bundle->env.get(), model_path_w.c_str(), bundle->opts.get(), &raw_session));
#else
        throw_on_ort_error(ort_api().CreateSession(bundle->env.get(), model_path_.c_str(), bundle->opts.get(), &raw_session));
#endif
        bundle->session.reset(raw_session);

        OrtAllocator* allocator = nullptr;
        throw_on_ort_error(ort_api().GetAllocatorWithDefaultOptions(&allocator));
        bundle->input_name = get_session_name(bundle->session.get(), 0, allocator, true);
        bundle->policy_output_name = get_session_name(bundle->session.get(), 0, allocator, false);
        bundle->value_output_name = get_session_name(bundle->session.get(), 1, allocator, false);
        cache.map[cache_key] = bundle;
        while (cache.map.size() > kMaxCachedOrtSessions) {
          auto erase_it = cache.map.begin();
          if (erase_it == cache.map.end()) break;
          if (erase_it->first == cache_key && cache.map.size() > 1) {
            ++erase_it;
            if (erase_it == cache.map.end()) {
              erase_it = cache.map.begin();
            }
          }
          cache.map.erase(erase_it);
        }
      }
    }
    impl_ = new Impl();
    impl_->bundle = std::move(bundle);
    ready_ = true;
  } catch (const std::exception& e) {
    last_error_ = e.what();
    ready_ = false;
    throw std::runtime_error(
        std::string("OnnxPolicyValueEvaluator: failed to load model '") +
        model_path_ + "': " + e.what());
  }
#else
  (void)model_path_;
  (void)cfg_;
  ready_ = false;
  last_error_ = "onnx runtime not enabled at build time";
  throw std::runtime_error(
      "OnnxPolicyValueEvaluator: ONNX runtime not enabled at build time. "
      "Rebuild with BOARD_AI_WITH_ONNX=1 BOARD_AI_ONNXRUNTIME_ROOT=/path/to/onnxruntime");
#endif
}

OnnxPolicyValueEvaluator::~OnnxPolicyValueEvaluator() {
#if defined(BOARD_AI_WITH_ONNX) && BOARD_AI_WITH_ONNX
  delete impl_;
  impl_ = nullptr;
#endif
}

bool OnnxPolicyValueEvaluator::evaluate(
    const IGameState& state,
    int perspective_player,
    const std::vector<ActionId>& legal_actions,
    std::vector<float>* priors,
    std::vector<float>* values) const {
  if (!priors || !values) {
    throw std::runtime_error("OnnxPolicyValueEvaluator::evaluate: priors or values pointer is null");
  }
  if (!encoder_) {
    throw std::runtime_error("OnnxPolicyValueEvaluator::evaluate: encoder is null");
  }
  const int num_players = state.num_players();

  std::vector<float> features;
  std::vector<float> legal_mask;
  if (!encoder_->encode(state, perspective_player, legal_actions, &features, &legal_mask)) {
    throw std::runtime_error("OnnxPolicyValueEvaluator::evaluate: encoder.encode() failed");
  }

#if defined(BOARD_AI_WITH_ONNX) && BOARD_AI_WITH_ONNX
  if (!ready_ || !impl_) {
    throw std::runtime_error("OnnxPolicyValueEvaluator::evaluate: model not loaded (ready=" +
        std::to_string(ready_) + ", last_error=" + last_error_ + ")");
  }

  try {
    std::array<int64_t, 2> shape = {1, static_cast<int64_t>(features.size())};
    OrtMemoryInfo* raw_memory_info = nullptr;
    throw_on_ort_error(ort_api().CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &raw_memory_info));
    OrtMemoryInfoPtr memory_info(raw_memory_info);

    OrtValue* raw_input = nullptr;
    throw_on_ort_error(ort_api().CreateTensorWithDataAsOrtValue(
        memory_info.get(),
        features.data(),
        features.size() * sizeof(float),
        shape.data(),
        shape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &raw_input));
    OrtValuePtr input(raw_input);

    const char* in_names[] = {impl_->bundle->input_name.c_str()};
    const char* out_names[] = {impl_->bundle->policy_output_name.c_str(), impl_->bundle->value_output_name.c_str()};
    const OrtValue* input_values[] = {input.get()};
    OrtValue* raw_outputs[2] = {nullptr, nullptr};
    throw_on_ort_error(ort_api().Run(
        impl_->bundle->session.get(), nullptr, in_names, input_values, 1, out_names, 2, raw_outputs));
    OrtValuePtr policy_output(raw_outputs[0]);
    OrtValuePtr value_output(raw_outputs[1]);

    OrtTensorTypeAndShapeInfo* raw_policy_shape = nullptr;
    throw_on_ort_error(ort_api().GetTensorTypeAndShape(policy_output.get(), &raw_policy_shape));
    OrtTensorShapePtr policy_shape(raw_policy_shape);
    size_t dim_count = 0;
    throw_on_ort_error(ort_api().GetDimensionsCount(policy_shape.get(), &dim_count));
    if (dim_count == 0) {
      throw std::runtime_error("OnnxPolicyValueEvaluator::evaluate: policy output has 0 dimensions");
    }

    std::vector<int64_t> policy_dims(dim_count, 0);
    throw_on_ort_error(ort_api().GetDimensions(policy_shape.get(), policy_dims.data(), policy_dims.size()));
    const int64_t policy_len64 = policy_dims.back();
    if (policy_len64 <= 0 || policy_len64 > static_cast<int64_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("OnnxPolicyValueEvaluator::evaluate: invalid policy output length: " +
          std::to_string(policy_len64));
    }
    void* policy_raw = nullptr;
    throw_on_ort_error(ort_api().GetTensorMutableData(policy_output.get(), &policy_raw));
    const float* policy_ptr = static_cast<const float*>(policy_raw);
    const int policy_len = static_cast<int>(policy_len64);
    std::vector<float> logits(static_cast<size_t>(policy_len), 0.0f);
    std::copy(policy_ptr, policy_ptr + policy_len, logits.begin());

    void* value_raw = nullptr;
    throw_on_ort_error(ort_api().GetTensorMutableData(value_output.get(), &value_raw));
    const float* value_ptr = static_cast<const float*>(value_raw);

    OrtTensorTypeAndShapeInfo* raw_value_shape = nullptr;
    throw_on_ort_error(ort_api().GetTensorTypeAndShape(value_output.get(), &raw_value_shape));
    OrtTensorShapePtr value_shape(raw_value_shape);
    size_t value_dim_count = 0;
    throw_on_ort_error(ort_api().GetDimensionsCount(value_shape.get(), &value_dim_count));
    std::vector<int64_t> value_dims(value_dim_count, 0);
    if (value_dim_count > 0) {
      throw_on_ort_error(ort_api().GetDimensions(value_shape.get(), value_dims.data(), value_dims.size()));
    }
    const int64_t value_len = (value_dim_count > 0) ? value_dims.back() : 1;

    if (value_len >= num_players) {
      // N-dim model output is perspective-relative: index 0 = perspective_player.
      // Rotate back to absolute player ordering for MCTS backup.
      values->resize(static_cast<size_t>(num_players));
      for (int i = 0; i < num_players; ++i) {
        const int abs_player = (perspective_player + i) % num_players;
        (*values)[static_cast<size_t>(abs_player)] =
            std::max(-1.0f, std::min(1.0f, value_ptr[i]));
      }
    } else {
      const float v = std::max(-1.0f, std::min(1.0f, value_ptr[0]));
      values->resize(static_cast<size_t>(num_players));
      const float opponent_v = (num_players > 1)
          ? -v / static_cast<float>(num_players - 1) : -v;
      for (int p = 0; p < num_players; ++p) {
        (*values)[static_cast<size_t>(p)] =
            (p == perspective_player) ? v : opponent_v;
      }
    }

    masked_softmax(logits, legal_mask, legal_actions, priors);
    return true;
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("OnnxPolicyValueEvaluator::evaluate: ") + e.what());
  }
#else
  throw std::runtime_error("OnnxPolicyValueEvaluator::evaluate: ONNX runtime not enabled at build time");
#endif
}

}  // namespace board_ai::infer
