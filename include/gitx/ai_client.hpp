#pragma once

#include "gitx/config.hpp"

#include <optional>
#include <string>
#include <vector>

namespace gitx {

// Configuration for the AI toolchain, read from [ai] in the team config.
struct AiConfig {
  std::string provider;      // deepseek | openai | ollama | custom
  std::string base_url;      // override
  std::string model;         // override
  int timeout_seconds = 60;
  std::size_t max_diff_chars = 8000;
};

// Resolves [ai] settings with provider defaults applied.
AiConfig resolve_ai_config(const TeamConfig& team);

// A single LLM chat message.
struct ChatMessage {
  std::string role;  // system | user | assistant
  std::string content;
};

// Thin wrapper around the OpenAI-compatible chat completions API.
// Requires libcurl at build time; when unavailable these functions are
// replaced by stubs that throw a Chinese "AI 功能需要 libcurl" error.
class AiClient {
 public:
  explicit AiClient(AiConfig config, std::string api_key);

  // Sends the conversation and returns the assistant reply text.
  // Throws std::runtime_error with a Chinese message on any failure.
  std::string complete(const std::vector<ChatMessage>& messages) const;

  // True when this build actually has network support compiled in.
  static bool available();

  // Reads the API key from the OS keyring (or GITX_AI_KEY env var).
  // Returns nullopt when no key is stored and prompting is needed.
  static std::optional<std::string> load_key();

  // Stores the API key in the OS keyring.
  static void store_key(const std::string& key);

 private:
  AiConfig config_;
  std::string api_key_;
};

}  // namespace gitx
