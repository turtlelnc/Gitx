#include "gitx/ai_client.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

// The AI toolchain is optional: when libcurl is present we build the real
// client; otherwise these become stubs that explain how to enable AI.
#ifdef GITX_HAVE_CURL
#include <curl/curl.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#endif

namespace fs = std::filesystem;

namespace gitx {
namespace {

const char* kAiSectionKey = "gitx-ai-key";

// ---- minimal JSON helpers (no external dependency) ----

std::string json_escape(const std::string& input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (const char ch : input) {
    switch (ch) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", ch);
          output += buf;
        } else {
          output += ch;
        }
    }
  }
  return output;
}

// Extracts the value of the first top-level member named `key` from a JSON
// object. Handles strings and simple values; good enough for API replies.
std::string json_get(const std::string& json, const std::string& key) {
  // Find `"key"` followed by optional whitespace and ':'.
  const std::string needle = "\"" + key + "\"";
  std::size_t pos = 0;
  while ((pos = json.find(needle, pos)) != std::string::npos) {
    std::size_t cursor = pos + needle.size();
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\n' || json[cursor] == '\r')) ++cursor;
    if (cursor < json.size() && json[cursor] == ':') {
      ++cursor;
      while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\n' || json[cursor] == '\r')) ++cursor;
      if (cursor < json.size() && json[cursor] == '"') {
        ++cursor;
        std::string value;
        while (cursor < json.size() && json[cursor] != '"') {
          if (json[cursor] == '\\' && cursor + 1 < json.size()) {
            const char next = json[cursor + 1];
            switch (next) {
              case 'n': value += '\n'; break;
              case 't': value += '\t'; break;
              case 'r': value += '\r'; break;
              case '"': value += '"'; break;
              case '\\': value += '\\'; break;
              case 'u': {
                if (cursor + 5 < json.size()) {
                  value += "?";
                  cursor += 5;
                }
                break;
              }
              default: value += next; break;
            }
            cursor += 2;
          } else {
            value += json[cursor];
            ++cursor;
          }
        }
        return value;
      }
      // Non-string value: capture until comma or closing brace.
      std::string value;
      while (cursor < json.size() && json[cursor] != ',' && json[cursor] != '}' && json[cursor] != ']') {
        value += json[cursor];
        ++cursor;
      }
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n' || value.back() == '\r')) value.pop_back();
      return value;
    }
    pos += needle.size();
  }
  return "";
}

// ---- keyring helpers ----

std::optional<std::string> keyring_load() {
  if (const char* env = std::getenv("GITX_AI_KEY")) return std::string(env);

#ifdef _WIN32
  PCREDENTIALW credential = nullptr;
  if (CredReadW(L"gitx-ai-key", CRED_TYPE_GENERIC, 0, &credential) == 0) return std::nullopt;
  std::string value(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
  CredFree(credential);
  if (!value.empty()) return value;
  return std::nullopt;
#elif defined(__APPLE__)
  // macOS Keychain: security find-generic-password
  std::string command = "security find-generic-password -s gitx -a gitx -w 2>/dev/null";
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return std::nullopt;
  char buffer[256];
  std::string value;
  while (fgets(buffer, sizeof buffer, pipe) != nullptr) value += buffer;
  pclose(pipe);
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
  if (!value.empty()) return value;
  return std::nullopt;
#else
  // Linux: libsecret via secret-tool, falling back to a 600-permission file.
  std::string command = "secret-tool lookup " + std::string(kAiSectionKey) + " gitx 2>/dev/null";
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe != nullptr) {
    char buffer[256];
    std::string value;
    while (fgets(buffer, sizeof buffer, pipe) != nullptr) value += buffer;
    pclose(pipe);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    if (!value.empty()) return value;
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr) return std::nullopt;
  const fs::path file = fs::path(home) / ".config" / "gitx" / "secret";
  std::ifstream in(file);
  std::string value;
  if (in && std::getline(in, value) && !value.empty()) return value;
  return std::nullopt;
#endif
}

void keyring_store(const std::string& key) {
#ifdef _WIN32
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = const_cast<wchar_t*>(L"gitx-ai-key");
  credential.UserName = const_cast<wchar_t*>(L"gitx");
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential.CredentialBlobSize = static_cast<DWORD>(key.size());
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(key.data()));
  if (CredWriteW(&credential, 0) == 0) throw std::runtime_error("无法写入 Windows 凭据管理器。");
#elif defined(__APPLE__)
  // Only store when the item does not already exist (add vs update).
  const std::string check = "security find-generic-password -s gitx -a gitx 2>/dev/null";
  const auto exists = std::system(check.c_str()) == 0;
  std::string command;
  if (exists) {
    command = "security delete-generic-password -s gitx -a gitx 2>/dev/null; ";
  }
  command += "security add-generic-password -s gitx -a gitx -w ";
  // Shell-escape the key: wrap in single quotes and escape embedded quotes.
  std::string escaped;
  for (const char ch : key) {
    if (ch == '\'') escaped += "'\\''";
    else escaped += ch;
  }
  command += "'" + escaped + "' 2>/dev/null";
  if (std::system(command.c_str()) != 0) throw std::runtime_error("无法写入 macOS 钥匙串。");
#else
  // Prefer libsecret; fall back to a 600-permission file.
  std::string escaped;
  for (const char ch : key) {
    if (ch == '\'') escaped += "'\\''";
    else escaped += ch;
  }
  const std::string command = "secret-tool store --label=gitx-ai " + std::string(kAiSectionKey) + " gitx <<'GITXEOF'\n" + key + "\nGITXEOF";
  if (std::system(command.c_str()) == 0) return;
  const char* home = std::getenv("HOME");
  if (home == nullptr) throw std::runtime_error("无法定位 HOME 目录。");
  const fs::path dir = fs::path(home) / ".config" / "gitx";
  fs::create_directories(dir);
  const fs::path file = dir / "secret";
  std::ofstream out(file, std::ios::trunc);
  if (!out) throw std::runtime_error("无法写入密钥文件: " + file.string());
  out << key << "\n";
  out.close();
  try {
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  } catch (...) {}
#endif
}

}  // namespace

AiConfig resolve_ai_config(const TeamConfig& team) {
  AiConfig config;
  config.provider = team.ai.provider;
  config.base_url = team.ai.base_url;
  config.model = team.ai.model;
  config.timeout_seconds = team.ai.timeout_seconds;
  config.max_diff_chars = team.ai.max_diff_chars;
  if (config.provider.empty()) config.provider = "deepseek";
  if (config.provider == "deepseek") {
    if (config.base_url.empty()) config.base_url = "https://api.deepseek.com/v1";
    if (config.model.empty()) config.model = "deepseek-chat";
  } else if (config.provider == "openai") {
    if (config.base_url.empty()) config.base_url = "https://api.openai.com/v1";
    if (config.model.empty()) config.model = "gpt-4o-mini";
  } else if (config.provider == "ollama") {
    if (config.base_url.empty()) config.base_url = "http://localhost:11434/v1";
    if (config.model.empty()) config.model = "qwen2.5:7b";
  } else if (config.provider == "custom") {
    if (config.base_url.empty()) throw std::runtime_error("AI provider=custom 时必须配置 base_url。");
    if (config.model.empty()) throw std::runtime_error("AI provider=custom 时必须配置 model。");
  } else {
    throw std::runtime_error("未知 AI provider: " + config.provider + "（支持 deepseek/openai/ollama/custom）");
  }
  return config;
}

AiClient::AiClient(AiConfig config, std::string api_key) : config_(std::move(config)), api_key_(std::move(api_key)) {}

bool AiClient::available() {
#ifdef GITX_HAVE_CURL
  return true;
#else
  return false;
#endif
}

std::optional<std::string> AiClient::load_key() {
  return keyring_load();
}

void AiClient::store_key(const std::string& key) {
  keyring_store(key);
}

std::string AiClient::complete(const std::vector<ChatMessage>& messages) const {
#ifndef GITX_HAVE_CURL
  (void)messages;
  throw std::runtime_error("当前构建未启用 AI 功能：缺少 libcurl。请安装 libcurl 开发包后重新构建（macOS/Linux 系统自带，Windows 用 vcpkg 安装 curl）。");
#else
  if (api_key_.empty()) throw std::runtime_error("未提供 AI API 密钥。请先运行 gitx save --ai 并按提示配置，或设置 GITX_AI_KEY 环境变量。");

  // Build the JSON request body.
  std::ostringstream body;
  body << "{\"model\":\"" << json_escape(config_.model) << "\",\"messages\":[";
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (index != 0) body << ",";
    body << "{\"role\":\"" << json_escape(messages[index].role) << "\",\"content\":\"" << json_escape(messages[index].content) << "\"}";
  }
  body << "],\"temperature\":0.3}";
  const std::string request_body = body.str();

  const std::string url = config_.base_url + "/chat/completions";

  std::string response_body;
  char error_buffer[CURL_ERROR_SIZE] = {0};

  curl_global_init(CURL_GLOBAL_DEFAULT);
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    curl_global_cleanup();
    throw std::runtime_error("无法初始化网络库。");
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key_).c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  // Local providers such as Ollama and the test mock must not be sent through
  // a globally configured corporate/debug proxy. Besides failing locally, a
  // proxy would unnecessarily expose requests intended to remain on-device.
  if (config_.base_url.starts_with("http://127.0.0.1") || config_.base_url.starts_with("http://localhost") ||
      config_.base_url.starts_with("https://127.0.0.1") || config_.base_url.starts_with("https://localhost")) {
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_body.size()));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(config_.timeout_seconds));
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
  });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

  const CURLcode result = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  curl_global_cleanup();

  if (result != CURLE_OK) {
    const std::string message = error_buffer[0] != '\0' ? error_buffer : "网络错误";
    throw std::runtime_error("AI 请求失败: " + message);
  }
  if (http_code != 200) {
    throw std::runtime_error("AI 服务返回错误 (HTTP " + std::to_string(http_code) + "): " + response_body.substr(0, 300));
  }

  // Extract choices[0].message.content
  const auto content = json_get(response_body, "content");
  if (content.empty()) {
    throw std::runtime_error("AI 响应解析失败（未找到 content 字段）。服务返回: " + response_body.substr(0, 300));
  }
  return content;
#endif
}

}  // namespace gitx
