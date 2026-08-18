#include "gitx/bundle.hpp"
#include <zlib.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
namespace gitx { namespace {
constexpr char Magic[] = "GITXBND3";
constexpr std::uint32_t kMaxFileCount = 10000;
constexpr std::uint32_t kMaxNameLength = 4096;
constexpr std::uint32_t kMaxEntryLength = 4096;
constexpr std::uint64_t kMaxFileSize = 512ULL * 1024 * 1024;
constexpr std::uint64_t kMaxBundleSize = 2ULL * 1024 * 1024 * 1024;
struct Footer {
  char magic[8];
  std::uint64_t offset;   // payload start (right after the launcher copy)
  std::uint32_t crc;      // CRC-32 of the payload region [offset, footer)
};
template <class T> void put(std::ofstream& s, T value) { s.write(reinterpret_cast<const char*>(&value), sizeof value); }
template <class T> T get(std::ifstream& s) { T value{}; s.read(reinterpret_cast<char*>(&value), sizeof value); if (!s) throw std::runtime_error("自解压包已损坏"); return value; }

// CRC-32 over the payload region of `path` starting at `offset`.
std::uint32_t payload_crc(const fs::path& path, std::uint64_t offset, std::uint64_t length) {
  std::ifstream in(path, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(offset));
  uLong crc = crc32(0L, Z_NULL, 0);
  std::vector<char> buffer(1 << 16);
  std::uint64_t remaining = length;
  while (remaining > 0 && in) {
    const auto chunk = static_cast<std::streamsize>((std::min<std::uint64_t>)(remaining, buffer.size()));
    in.read(buffer.data(), chunk);
    const auto got = in.gcount();
    if (got <= 0) break;
    crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(got));
    remaining -= static_cast<std::uint64_t>(got);
  }
  if (remaining > 0) throw std::runtime_error("自解压包读取不完整");
  return static_cast<std::uint32_t>(crc);
}

void add_file(std::ofstream& out, const fs::path& source, const std::string& name) {
  std::ifstream in(source, std::ios::binary); std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
  uLongf bound = compressBound(static_cast<uLong>(raw.size())); std::vector<Bytef> zipped(bound);
  const auto code = compress2(zipped.data(), &bound, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION);
  if (code != Z_OK) throw std::runtime_error("无法压缩: " + source.string());
  const std::uint32_t length = static_cast<std::uint32_t>(name.size());
  const std::uint32_t mode = static_cast<std::uint32_t>(fs::status(source).permissions());
  const std::uint64_t original = raw.size(), compressed = bound;
  put(out, length); put(out, mode); put(out, original); put(out, compressed);
  out.write(name.data(), length); out.write(reinterpret_cast<const char*>(zipped.data()), static_cast<std::streamsize>(bound));
}
bool safe_name(const fs::path& name) {
  if (name.empty() || name.is_absolute()) return false;
  for (const auto& part : name) if (part == "..") return false;
  return true;
}

void read_exact(std::ifstream& input, char* data, std::streamsize size) {
  input.read(data, size);
  if (input.gcount() != size) throw std::runtime_error("自解压包读取不完整");
}

fs::path make_temp_root() {
  std::random_device random;
  const auto base = fs::temp_directory_path();
  for (int attempt = 0; attempt < 32; ++attempt) {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(random());
    const auto root = base / ("gitx-bundle-" + suffix);
    if (fs::create_directory(root)) return root;
  }
  throw std::runtime_error("无法创建安全的临时解压目录");
}
} 
void Bundle::create(const fs::path& launcher, const fs::path& output, const fs::path& project, const std::string& entry, const std::vector<fs::path>& runtimes) {
  if (!fs::is_regular_file(launcher)) throw std::runtime_error("找不到 gitx 可执行文件: " + launcher.string());
  fs::copy_file(launcher, output, fs::copy_options::overwrite_existing); std::ofstream out(output, std::ios::binary | std::ios::app); const auto offset = static_cast<std::uint64_t>(out.tellp());
  std::vector<std::pair<fs::path,std::string>> files;
  const auto git_dir = (project / ".git").string();
  for (auto iterator = fs::recursive_directory_iterator(project); iterator != fs::recursive_directory_iterator(); ++iterator) {
    if (iterator->is_directory() && iterator->path().filename() == ".git") {
      iterator.disable_recursion_pending();
      continue;
    }
    if (!iterator->is_regular_file() || iterator->path().string().find(git_dir) == 0) continue;
    files.emplace_back(iterator->path(), (fs::path("source") / fs::relative(iterator->path(), project)).generic_string());
  }
  for (const auto& runtime : runtimes) { if (fs::is_regular_file(runtime)) files.emplace_back(runtime, (fs::path("runtime") / runtime.filename()).generic_string()); else for (const auto& item : fs::recursive_directory_iterator(runtime)) if (item.is_regular_file()) files.emplace_back(item.path(), (fs::path("runtime") / runtime.filename() / fs::relative(item.path(), runtime)).generic_string()); }
  put(out, static_cast<std::uint32_t>(files.size())); const auto entry_name = (fs::path("runtime") / entry).generic_string(); put(out, static_cast<std::uint32_t>(entry_name.size())); out.write(entry_name.data(), entry_name.size()); for (const auto& [file, name] : files) add_file(out, file, name);
  out.flush();
  Footer foot{}; std::memcpy(foot.magic, Magic, 8); foot.offset = offset;
  const auto payload_length = static_cast<std::uint64_t>(out.tellp()) - offset;
  foot.crc = payload_crc(output, offset, payload_length);
  out.write(reinterpret_cast<const char*>(&foot), sizeof foot);
}
bool Bundle::extract_and_launch_if_present(const fs::path& executable) {
  const auto file_size = fs::file_size(executable);
  if (file_size < static_cast<std::uint64_t>(sizeof(Footer))) return false;
  std::ifstream in(executable, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(-static_cast<std::int64_t>(sizeof(Footer))), std::ios::end);
  Footer foot{}; in.read(reinterpret_cast<char*>(&foot), sizeof foot);
  if (std::memcmp(foot.magic, Magic, 8) != 0) return false;
  if (foot.offset >= file_size || foot.offset + sizeof(Footer) > file_size) throw std::runtime_error("自解压包损坏（偏移非法）");
  const auto payload_length = file_size - foot.offset - sizeof(Footer);
  if (payload_length > kMaxBundleSize) throw std::runtime_error("自解压包过大，已拒绝解压");
  // Integrity check before touching the filesystem.
  if (payload_crc(executable, foot.offset, payload_length) != foot.crc) throw std::runtime_error("自解压包完整性校验失败，包可能已损坏");
  in.seekg(static_cast<std::streamoff>(foot.offset));
  const auto count = get<std::uint32_t>(in);
  const auto entry_len = get<std::uint32_t>(in);
  if (count > kMaxFileCount || entry_len == 0 || entry_len > kMaxEntryLength) throw std::runtime_error("自解压包元数据非法");
  std::string entry(entry_len, '\0');
  read_exact(in, entry.data(), static_cast<std::streamsize>(entry_len));
  if (!safe_name(fs::path(entry))) throw std::runtime_error("包内入口路径非法");
  const auto root = make_temp_root();
  // Always remove the temporary directory, including on failure or after launch.
  try {
    std::uint64_t total_size = 0;
    for (std::uint32_t n = 0; n < count; ++n) {
      const auto name_length = get<std::uint32_t>(in);
      const auto mode = get<std::uint32_t>(in);
      const auto original = get<std::uint64_t>(in);
      const auto compressed = get<std::uint64_t>(in);
      if (name_length == 0 || name_length > kMaxNameLength || original > kMaxFileSize || compressed > kMaxFileSize ||
          compressed > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
          original > static_cast<std::uint64_t>(std::numeric_limits<uLongf>::max()) || total_size > kMaxBundleSize - original) {
        throw std::runtime_error("包内文件元数据非法或过大");
      }
      total_size += original;
      std::string name(name_length, '\0');
      read_exact(in, name.data(), static_cast<std::streamsize>(name_length));
      if (!safe_name(fs::path(name))) throw std::runtime_error("包内路径非法");
      std::vector<Bytef> zipped(static_cast<std::size_t>(compressed));
      std::vector<Bytef> raw(static_cast<std::size_t>(original));
      read_exact(in, reinterpret_cast<char*>(zipped.data()), static_cast<std::streamsize>(compressed));
      uLongf size = static_cast<uLongf>(original);
      if (uncompress(raw.data(), &size, zipped.data(), static_cast<uLong>(compressed)) != Z_OK || size != original) {
        throw std::runtime_error("包内文件解压失败");
      }
      const auto target = root / name;
      fs::create_directories(target.parent_path());
      std::ofstream out(target, std::ios::binary);
      if (!out) throw std::runtime_error("无法写入解压文件");
      out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(original));
      if (!out) throw std::runtime_error("写入解压文件失败");
      fs::permissions(target, static_cast<fs::perms>(mode), fs::perm_options::replace);
    }
    const auto target = root / entry;
    if (!fs::is_regular_file(target)) throw std::runtime_error("包内入口不存在: " + entry);
    const auto exit_code = std::system((std::string("\"") + target.string() + "\"").c_str());
    if (exit_code != 0) throw std::runtime_error("包内入口程序执行失败");
  } catch (...) {
    fs::remove_all(root);
    throw;
  }
  fs::remove_all(root);
  return true;
}
}
