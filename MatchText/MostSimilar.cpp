#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <fstream>
#include <limits>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Statistics.h"
#include "Utils.h"

namespace fs = std::filesystem;

namespace {

std::string CsvEscape(const std::string& field) {
  bool needs_quotes = false;
  for (char ch : field) {
    if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return field;
  }
  std::string out;
  out.reserve(field.size() + 2);
  out.push_back('"');
  for (char ch : field) {
    if (ch == '"') {
      out.push_back('"');
    }
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string OutputNameForDir(const fs::path& dir_path, bool use_hash) {
  // Use the input directory name as the CSV basename.
  std::error_code ec;
  fs::path resolved = fs::absolute(dir_path, ec);
  if (ec) {
    resolved = dir_path.lexically_normal();
  }
  fs::path name = resolved.filename();
  if (name.empty() || name == "." || name == "..") {
    name = resolved.parent_path().filename();
  }
  if (name.empty() || name == "." || name == "..") {
    name = "output";
  }
  std::string base = name.string();
  if (use_hash) {
    return base + "_mostsimilar_hash.csv";
  }
  return base + "_mostsimilar.csv";
}

std::string MaskedPath(const fs::path& path, const fs::path& mask_root) {
  // Replace the root prefix with .../ for portable output.
  std::error_code ec;
  fs::path absolute_path = fs::absolute(path, ec);
  if (ec) {
    absolute_path = path;
  }
  fs::path absolute_root = fs::absolute(mask_root, ec);
  if (ec) {
    absolute_root = mask_root;
  }
  fs::path relative = absolute_path.lexically_relative(absolute_root);
  bool contains_parent = false;
  for (const auto& part : relative) {
    if (part == "..") {
      contains_parent = true;
      break;
    }
  }
  if (!relative.empty() && !contains_parent) {
    return std::string(".../") + relative.generic_string();
  }
  return absolute_path.string();
}

void PrintProgress(const char* label, size_t current, size_t total) {
  // Simple progress indicator for long runs.
  std::fprintf(stderr, "\r%s: %zu/%zu", label, current, total);
  std::fflush(stderr);
}

void PrintProgressWithThreads(const char* label,
                              size_t current,
                              size_t total,
                              unsigned int threads) {
  // Progress indicator with worker count for file loading.
  std::fprintf(stderr, "\r%s: %zu/%zu  Threads: %u", label, current, total, threads);
  std::fflush(stderr);
}

std::string FormatScore(double score) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(8) << score;
  return out.str();
}

struct VersionInfo {
  bool has_version = false;
  bool is_date = false;
  std::vector<int> parts;
  int suffix = 0;
  bool has_tag = false;
};

int CompareVersionInfo(const VersionInfo& left, const VersionInfo& right);

struct FileTimeInfo {
  bool valid = false;
  fs::file_time_type value{};
};

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

int ParseSuffixValue(const std::string& suffix) {
  if (suffix.empty()) {
    return 0;
  }
  const char ch = suffix[0];
  if (ch >= 'a' && ch <= 'z') {
    return (ch - 'a') + 1;
  }
  return 0;
}

std::vector<int> ParseVersionParts(const std::string& value) {
  std::vector<int> parts;
  size_t start = 0;
  while (start < value.size()) {
    size_t end = value.find('.', start);
    if (end == std::string::npos) {
      end = value.size();
    }
    const std::string segment = value.substr(start, end - start);
    if (!segment.empty()) {
      try {
        parts.push_back(std::stoi(segment));
      }
      catch (...) {
        parts.push_back(0);
      }
    }
    if (end == value.size()) {
      break;
    }
    start = end + 1;
  }
  return parts;
}

VersionInfo ExtractVersionInfo(const fs::path& path) {
  VersionInfo best;
  bool have_best = false;
  const std::string name = ToLowerAscii(path.stem().string());
  static const std::regex version_re(R"((^|[^a-z0-9])v?(\d+(?:\.\d+)*)([a-z]?))");
  static const std::regex v_separator_re(
    R"((^|[^a-z0-9])v[._-]+(\d+(?:\.\d+)*)([a-z]?))");
  static const std::regex separator_v_re(
    R"((^|[^a-z0-9])[._-]+v[._-]+(\d+(?:\.\d+)*)([a-z]?))");
  static const std::regex prefix_version_re(
    R"((^|[^a-z0-9])(ver|version|rel|release|build|b)(\d+(?:\.\d+)*)([a-z]?))");
  static const std::regex rev_re(R"((^|[^a-z0-9])(rev|revision|r)(\d+)?([a-z]?))");
  static const std::regex tag_version_re(
    R"((^|[^a-z0-9])(final|latest|new|updated|update|revised)(\d+)?([a-z]?))");
  static const std::regex tag_re(
    R"((^|[^a-z0-9])(new|revised|revision|rev|latest|final|updated|update)($|[^a-z0-9]))");
  static const std::regex date_re(
    R"((^|[^0-9])(\d{4})[-_\.]?(\d{2})[-_\.]?(\d{2})(?:[tT_\. -]?(\d{2})[:_\-\.]?(\d{2})(?:[:_\-\.]?(\d{2}))?)?)");
  static const std::regex date_compact_re(
    R"((^|[^0-9])(\d{8})(\d{4}|\d{6})?($|[^0-9]))");
  static const std::regex year_month_re(
    R"((^|[^0-9])(\d{4})[-_\.]?(\d{2})($|[^0-9]))");
  static const std::regex quarter_re(
    R"((^|[^0-9])(\d{4})[-_\.]?(q|quarter)([1-4])($|[^0-9]))");
  const bool has_tag = std::regex_search(name, tag_re);

  auto consider = [&](const VersionInfo& candidate) {
    if (!have_best) {
      best = candidate;
      have_best = true;
      return;
    }
    if (CompareVersionInfo(candidate, best) > 0) {
      best = candidate;
    }
  };

  for (std::sregex_iterator it(name.begin(), name.end(), date_re), end; it != end; ++it) {
    const int year = std::stoi((*it)[2].str());
    const int month = std::stoi((*it)[3].str());
    const int day = std::stoi((*it)[4].str());
    const int hour = (*it)[5].matched ? std::stoi((*it)[5].str()) : 0;
    const int minute = (*it)[6].matched ? std::stoi((*it)[6].str()) : 0;
    const int second = (*it)[7].matched ? std::stoi((*it)[7].str()) : 0;
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
      continue;
    }
    VersionInfo candidate;
    candidate.has_version = true;
    candidate.is_date = true;
    candidate.parts = {year, month, day, hour, minute, second};
    candidate.suffix = 0;
    candidate.has_tag = has_tag;
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), date_compact_re), end; it != end; ++it) {
    const std::string ymd = (*it)[2].str();
    const int year = std::stoi(ymd.substr(0, 4));
    const int month = std::stoi(ymd.substr(4, 2));
    const int day = std::stoi(ymd.substr(6, 2));
    int hour = 0;
    int minute = 0;
    int second = 0;
    if ((*it)[3].matched) {
      const std::string hms = (*it)[3].str();
      if (hms.size() == 4) {
        hour = std::stoi(hms.substr(0, 2));
        minute = std::stoi(hms.substr(2, 2));
      }
      else if (hms.size() == 6) {
        hour = std::stoi(hms.substr(0, 2));
        minute = std::stoi(hms.substr(2, 2));
        second = std::stoi(hms.substr(4, 2));
      }
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
      continue;
    }
    VersionInfo candidate;
    candidate.has_version = true;
    candidate.is_date = true;
    candidate.parts = {year, month, day, hour, minute, second};
    candidate.suffix = 0;
    candidate.has_tag = has_tag;
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), year_month_re), end; it != end; ++it) {
    const int year = std::stoi((*it)[2].str());
    const int month = std::stoi((*it)[3].str());
    if (month < 1 || month > 12) {
      continue;
    }
    VersionInfo candidate;
    candidate.has_version = true;
    candidate.is_date = true;
    candidate.parts = {year, month, 0, 0, 0, 0};
    candidate.suffix = 0;
    candidate.has_tag = has_tag;
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), quarter_re), end; it != end; ++it) {
    const int year = std::stoi((*it)[2].str());
    const int quarter = std::stoi((*it)[4].str());
    const int month = quarter * 3;
    VersionInfo candidate;
    candidate.has_version = true;
    candidate.is_date = true;
    candidate.parts = {year, month, 0, 0, 0, 0};
    candidate.suffix = 0;
    candidate.has_tag = has_tag;
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), version_re), end; it != end; ++it) {
    VersionInfo candidate;
    candidate.has_version = true;
    candidate.is_date = false;
    candidate.parts = ParseVersionParts((*it)[2].str());
    candidate.suffix = ParseSuffixValue((*it)[3].str());
    candidate.has_tag = has_tag;
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), v_separator_re), end; it != end; ++it) {
    VersionInfo candidate;
    candidate.has_version = true;
    candidate.is_date = false;
    candidate.parts = ParseVersionParts((*it)[2].str());
    candidate.suffix = ParseSuffixValue((*it)[3].str());
    candidate.has_tag = has_tag;
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), separator_v_re), end; it != end; ++it) {
    VersionInfo candidate;
    candidate.has_version = true;
    candidate.is_date = false;
    candidate.parts = ParseVersionParts((*it)[2].str());
    candidate.suffix = ParseSuffixValue((*it)[3].str());
    candidate.has_tag = has_tag;
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), prefix_version_re), end; it != end; ++it) {
    VersionInfo candidate;
    candidate.has_version = true;
    candidate.is_date = false;
    candidate.parts = ParseVersionParts((*it)[3].str());
    candidate.suffix = ParseSuffixValue((*it)[4].str());
    candidate.has_tag = true;
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), rev_re), end; it != end; ++it) {
    VersionInfo candidate;
    candidate.is_date = false;
    candidate.has_tag = true;
    const std::string digits = (*it)[3].str();
    const std::string suffix = (*it)[4].str();
    if (!digits.empty()) {
      candidate.has_version = true;
      candidate.parts = {std::stoi(digits)};
      candidate.suffix = ParseSuffixValue(suffix);
    }
    else if (!suffix.empty()) {
      candidate.has_version = true;
      candidate.parts = {0};
      candidate.suffix = ParseSuffixValue(suffix);
    }
    consider(candidate);
  }

  for (std::sregex_iterator it(name.begin(), name.end(), tag_version_re), end; it != end; ++it) {
    VersionInfo candidate;
    candidate.is_date = false;
    candidate.has_tag = true;
    const std::string digits = (*it)[3].str();
    const std::string suffix = (*it)[4].str();
    if (!digits.empty()) {
      candidate.has_version = true;
      candidate.parts = {std::stoi(digits)};
      candidate.suffix = ParseSuffixValue(suffix);
    }
    else if (!suffix.empty()) {
      candidate.has_version = true;
      candidate.parts = {0};
      candidate.suffix = ParseSuffixValue(suffix);
    }
    consider(candidate);
  }

  if (!have_best && has_tag) {
    best.has_tag = true;
  }
  return best;
}

int CompareVersionInfo(const VersionInfo& left, const VersionInfo& right) {
  if (left.is_date != right.is_date) {
    return left.is_date ? 1 : -1;
  }
  if (left.has_version && right.has_version) {
    const size_t max_parts = std::max(left.parts.size(), right.parts.size());
    for (size_t i = 0; i < max_parts; i++) {
      const int left_part = (i < left.parts.size()) ? left.parts[i] : 0;
      const int right_part = (i < right.parts.size()) ? right.parts[i] : 0;
      if (left_part != right_part) {
        return (left_part < right_part) ? -1 : 1;
      }
    }
    if (left.suffix != right.suffix) {
      return (left.suffix < right.suffix) ? -1 : 1;
    }
    if (left.has_tag != right.has_tag) {
      return left.has_tag ? 1 : -1;
    }
    return 0;
  }
  if (left.has_version != right.has_version) {
    if (!left.has_version && right.has_version) {
      return left.has_tag ? 1 : -1;
    }
    if (left.has_version && !right.has_version) {
      return right.has_tag ? -1 : 1;
    }
  }
  if (left.has_tag != right.has_tag) {
    return left.has_tag ? 1 : -1;
  }
  return 0;
}

FileTimeInfo GetLastWriteTime(const fs::path& path) {
  std::error_code ec;
  FileTimeInfo info;
  info.value = fs::last_write_time(path, ec);
  info.valid = !ec;
  return info;
}

size_t ChooseMoveIndex(size_t left,
                        size_t right,
                        const std::vector<VersionInfo>& versions,
                        const std::vector<FileTimeInfo>& times) {
  const int version_cmp = CompareVersionInfo(versions[left], versions[right]);
  if (version_cmp != 0) {
    return (version_cmp > 0) ? right : left;
  }
  const FileTimeInfo& left_time = times[left];
  const FileTimeInfo& right_time = times[right];
  if (left_time.valid && right_time.valid && left_time.value != right_time.value) {
    return (left_time.value < right_time.value) ? left : right;
  }
  if (left_time.valid != right_time.valid) {
    return left_time.valid ? right : left;
  }
  return (left > right) ? left : right;
}

bool IsDedupScore(double score, double threshold) {
  // Apply the same 8-decimal resolution used in output formatting.
  const double epsilon = 0.5e-8;
  return (score + epsilon) >= threshold;
}

bool ParseThreshold(const char* value, double* out) {
  if (out == nullptr) {
    return false;
  }
  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value || *end != '\0') {
    return false;
  }
  if (parsed < 0.0 || parsed > 1.0) {
    return false;
  }
  *out = parsed;
  return true;
}

bool ContainsParentReference(const fs::path& path) {
  for (const auto& part : path) {
    if (part == "..") {
      return true;
    }
  }
  return false;
}

fs::path NormalizePath(const fs::path& path) {
  std::error_code ec;
  fs::path absolute_path = fs::absolute(path, ec);
  if (ec) {
    absolute_path = path;
  }
  return absolute_path.lexically_normal();
}

bool IsUnderPath(const fs::path& path, const fs::path& root) {
  const fs::path abs_path = NormalizePath(path);
  const fs::path abs_root = NormalizePath(root);
  const fs::path relative = abs_path.lexically_relative(abs_root);
  if (relative.empty() || ContainsParentReference(relative)) {
    return false;
  }
  return true;
}

fs::path RelativeToRoot(const fs::path& path, const fs::path& root) {
  const fs::path abs_path = NormalizePath(path);
  const fs::path abs_root = NormalizePath(root);
  const fs::path relative = abs_path.lexically_relative(abs_root);
  if (relative.empty() || ContainsParentReference(relative)) {
    return path.filename();
  }
  return relative;
}

fs::path MakeUniquePath(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return path;
  }
  const fs::path parent = path.parent_path();
  const std::string stem = path.stem().string();
  const std::string ext = path.extension().string();
  for (int i = 1; i <= 1000; i++) {
    const std::string suffix = "_" + std::to_string(i);
    const fs::path candidate = parent / (stem + suffix + ext);
    if (!fs::exists(candidate, ec)) {
      return candidate;
    }
  }
  return path;
}

std::vector<std::string> WrapText(const std::string& text, size_t width) {
  std::vector<std::string> lines;
  if (width == 0) {
    lines.push_back("");
    return lines;
  }
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t len = std::min(width, text.size() - pos);
    lines.push_back(text.substr(pos, len));
    pos += len;
  }
  if (lines.empty()) {
    lines.push_back("");
  }
  return lines;
}

void PrintSeparator(size_t file_width, size_t match_width, size_t score_width) {
  // ASCII table border.
  std::cout << '+'
            << std::string(file_width + 2, '-')
            << '+'
            << std::string(match_width + 2, '-')
            << '+'
            << std::string(score_width + 2, '-')
            << "+\n";
}

void PrintRow(const std::string& file,
              const std::string& match,
              const std::string& score,
              size_t file_width,
              size_t match_width,
              size_t score_width) {
  // Emit a single row with padded columns.
  std::cout << "| " << std::left << std::setw(static_cast<int>(file_width)) << file
            << " | " << std::left << std::setw(static_cast<int>(match_width)) << match
            << " | " << std::right << std::setw(static_cast<int>(score_width)) << score
            << " |\n";
}

void PrintWrappedRow(const std::string& file,
                     const std::string& match,
                     const std::string& score,
                     size_t file_width,
                     size_t match_width,
                     size_t score_width) {
  // Wrap long file paths without breaking table alignment.
  const std::vector<std::string> file_lines = WrapText(file, file_width);
  const std::vector<std::string> match_lines = WrapText(match, match_width);
  const size_t row_lines = std::max(file_lines.size(), match_lines.size());
  for (size_t i = 0; i < row_lines; i++) {
    const std::string file_part = (i < file_lines.size()) ? file_lines[i] : "";
    const std::string match_part = (i < match_lines.size()) ? match_lines[i] : "";
    const std::string score_part = (i == 0) ? score : "";
    PrintRow(file_part, match_part, score_part, file_width, match_width, score_width);
  }
}

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "Find the closest match for each file within a directory tree and write a CSV.\n"
                 "Usage: %s <Directory> [--hash] [--dedup] [--threads N] [--safe] [--no-convert] [--verbose]\n",
                 argv[0]);
    return 1;
  }

  bool use_hash = false;
  bool dedup = false;
  bool threads_specified = false;
  unsigned int requested_threads = 0;
  double dedup_threshold = 1.0;
  bool safe_mode = false;
  bool no_convert = false;
  bool verbose = false;
  fs::path root;
  // Parse arguments: one directory plus optional flags.
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--hash") == 0) {
      use_hash = true;
      continue;
    }
    if (strcmp(argv[i], "--dedup") == 0) {
      dedup = true;
      if (i + 1 < argc) {
        double value = 0.0;
        if (ParseThreshold(argv[i + 1], &value)) {
          dedup_threshold = value;
          i++;
        }
      }
      continue;
    }
    if (strcmp(argv[i], "--threads") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--threads requires a value.\n");
        return 1;
      }
      char* end = nullptr;
      const unsigned long value = std::strtoul(argv[i + 1], &end, 10);
      if (end == argv[i + 1] || *end != '\0' || value == 0 ||
          value > std::numeric_limits<unsigned int>::max()) {
        std::fprintf(stderr, "Invalid thread count: %s\n", argv[i + 1]);
        return 1;
      }
      threads_specified = true;
      requested_threads = static_cast<unsigned int>(value);
      i++;
      continue;
    }
    if (strcmp(argv[i], "--safe") == 0) {
      safe_mode = true;
      continue;
    }
    if (strcmp(argv[i], "--no-convert") == 0) {
      no_convert = true;
      continue;
    }
    if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
      verbose = true;
      continue;
    }
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      std::fprintf(stderr,
                   "Find the closest match for each file within a directory tree and write a CSV.\n"
                   "Usage: %s <Directory> [--hash] [--dedup] [--threads N] [--safe] [--no-convert] [--verbose]\n"
                   "  --hash  Use SimHash to compare files instead of TF-IDF cosine similarity.\n"
                   "  --dedup [threshold]  Move matches with score >= threshold (default 1.0)\n"
                   "                       into <Directory>/Duplicates.\n"
                   "  --threads N  Override the worker thread count used for file parsing.\n"
                   "  --safe  Serialize PDF extraction to avoid poppler threading issues.\n"
                   "  --no-convert  Skip format-specific extractors and read raw bytes only.\n"
                   "  --verbose  Print files as they are read and comparisons as they are scored.\n"
                   "Scores are normalized to [0, 1], where 1.0 means identical.\n",
                   argv[0]);
      return 0;
    }
    if (argv[i][0] == '-') {
      std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 1;
    }
    if (root.empty()) {
      root = fs::path(argv[i]);
    }
    else {
      std::fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
      return 1;
    }
  }
  if (root.empty()) {
    std::fprintf(stderr,
                 "Find the closest match for each file within a directory tree and write a CSV.\n"
                 "Usage: %s <Directory> [--hash] [--dedup] [--threads N] [--safe] [--no-convert] [--verbose]\n",
                 argv[0]);
    return 1;
  }
  const fs::path duplicates_dir = root / "Duplicates";
  const fs::directory_options options = fs::directory_options::skip_permission_denied;
  std::error_code ec;
  fs::recursive_directory_iterator it(root, options, ec);
  if (ec) {
    std::fprintf(stderr, "Cannot open directory: %s\n", ec.message().c_str());
    return 2;
  }

  std::vector<fs::path> all_files;
  // Collect candidate files (recursive).
  for (fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
    if (ec) {
      std::fprintf(stderr, "Skipping path due to error: %s\n", ec.message().c_str());
      ec.clear();
      continue;
    }
    if (dedup && it->is_directory(ec) && !ec && IsUnderPath(it->path(), duplicates_dir)) {
      it.disable_recursion_pending();
      continue;
    }
    if (ec) {
      ec.clear();
      continue;
    }
    if (!it->is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    if (!IsAllowedTextFile(it->path().string())) {
      continue;
    }
    all_files.push_back(it->path());
  }

  if (all_files.empty()) {
    std::fprintf(stderr, "No files found under %s\n", root.string().c_str());
    return 2;
  }

  const unsigned int max_threads = std::max(1u, std::thread::hardware_concurrency());
  const unsigned int worker_count = threads_specified ? requested_threads : max_threads;
  struct LoadedDoc {
    fs::path path;
    Statistics stats;
    Statistics::SimHash128 hash;
    size_t index = 0;
  };
  std::vector<std::vector<LoadedDoc>> worker_docs(worker_count);
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  std::atomic<size_t> next_index{0};
  std::atomic<size_t> processed{0};
  const size_t total_files = all_files.size();
  for (unsigned int i = 0; i < worker_count; i++) {
    workers.emplace_back([&, i]() {
      for (;;) {
        const size_t index = next_index.fetch_add(1);
        if (index >= total_files) {
          break;
        }
        const fs::path& file_path = all_files[index];
        Statistics stat;
        if (verbose) {
          std::fprintf(stderr, "\nReading file: %s\n", file_path.string().c_str());
        }
        if (!ReadFileToStatistics(file_path.string(), &stat, safe_mode, no_convert)) {
          processed.fetch_add(1);
          continue;
        }
        if (stat.IsEmpty()) {
          std::fprintf(stderr, "\nSkipping empty file %s\n", file_path.string().c_str());
          processed.fetch_add(1);
          continue;
        }
        LoadedDoc doc;
        doc.path = file_path;
        doc.stats = std::move(stat);
        if (use_hash) {
          doc.hash = doc.stats.SimHash128Signature();
        }
        doc.index = index;
        worker_docs[i].push_back(std::move(doc));
        processed.fetch_add(1);
      }
    });
  }
  size_t last_progress = 0;
  PrintProgressWithThreads("Reading files", 0, total_files, worker_count);
  while (processed.load() < total_files) {
    const size_t current = processed.load();
    if (current != last_progress) {
      PrintProgressWithThreads("Reading files", current, total_files, worker_count);
      last_progress = current;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  PrintProgressWithThreads("Reading files", total_files, total_files, worker_count);
  std::fprintf(stderr, "\n");
  for (auto& worker : workers) {
    worker.join();
  }

  std::vector<LoadedDoc> docs;
  size_t total_entries = 0;
  for (const auto& worker_doc : worker_docs) {
    total_entries += worker_doc.size();
  }
  docs.reserve(total_entries);
  for (auto& worker_doc : worker_docs) {
    for (auto& doc : worker_doc) {
      docs.push_back(std::move(doc));
    }
  }
  std::sort(docs.begin(), docs.end(), [](const LoadedDoc& left, const LoadedDoc& right) {
    return left.index < right.index;
  });
  std::vector<fs::path> files;
  std::vector<Statistics> stats;
  std::vector<Statistics::SimHash128> hashes;
  files.reserve(docs.size());
  stats.reserve(docs.size());
  if (use_hash) {
    hashes.reserve(docs.size());
  }
  for (auto& doc : docs) {
    files.push_back(doc.path);
    if (use_hash) {
      hashes.push_back(doc.hash);
    }
    stats.push_back(std::move(doc.stats));
  }

  if (files.size() < 2) {
    std::fprintf(stderr, "Need at least two non-empty files to compare.\n");
    return 2;
  }

  const size_t n = files.size();
  std::vector<VersionInfo> version_infos;
  std::vector<FileTimeInfo> mod_times;
  version_infos.reserve(n);
  mod_times.reserve(n);
  for (const auto& path : files) {
    version_infos.push_back(ExtractVersionInfo(path));
    mod_times.push_back(GetLastWriteTime(path));
  }
  std::vector<size_t> best_index(n, n);
  std::vector<double> best_score(n, -1.0);
  // O(n^2) comparison: compute the closest match for each file.
  for (size_t i = 0; i < n; i++) {
    PrintProgressWithThreads("Computing matches", i + 1, n, worker_count);
    for (size_t j = i + 1; j < n; j++) {
      if (verbose) {
        std::fprintf(stderr,
                     "\nComparing: %s <> %s\n",
                     files[i].string().c_str(),
                     files[j].string().c_str());
      }
      const double score = use_hash
        ? Statistics::SimHashSimilarity(hashes[i], hashes[j])
        : Statistics::TfIdfCosineSimilarity(stats[i], stats[j]);
      if (score > best_score[i]) {
        best_score[i] = score;
        best_index[i] = j;
      }
      if (score > best_score[j]) {
        best_score[j] = score;
        best_index[j] = i;
      }
    }
  }
  std::fprintf(stderr, "\n");

  std::vector<fs::path> dedup_sources;
  if (dedup) {
    std::unordered_set<std::string> seen;
    seen.reserve(n);
    for (size_t i = 0; i < n; i++) {
      if (best_index[i] >= n) {
        continue;
      }
      if (!IsDedupScore(best_score[i], dedup_threshold)) {
        continue;
      }
      const size_t match_index = best_index[i];
      if (best_index[match_index] == i &&
          IsDedupScore(best_score[match_index], dedup_threshold) &&
          i != match_index) {
        const size_t move_index = ChooseMoveIndex(i, match_index, version_infos, mod_times);
        if (move_index != i) {
          continue;
        }
      }
      const fs::path& candidate = files[match_index];
      if (IsUnderPath(candidate, duplicates_dir)) {
        continue;
      }
      const std::string key = NormalizePath(candidate).string();
      if (seen.insert(key).second) {
        dedup_sources.push_back(candidate);
      }
    }
  }

  const fs::path output_path = fs::current_path() / OutputNameForDir(root, use_hash);
  std::error_code mask_ec;
  fs::path mask_root = fs::absolute(root, mask_ec);
  if (mask_ec) {
    mask_root = root;
  }

  struct Row {
    std::string file;
    std::string match;
    double score = 0.0;
    size_t pair_id = 0;
  };
  std::vector<Row> rows;
  rows.reserve(n);
  const double output_pair_threshold = 1e-8;
  struct PairKey {
    size_t left = 0;
    size_t right = 0;
    bool operator==(const PairKey& other) const {
      return left == other.left && right == other.right;
    }
  };
  struct PairKeyHash {
    size_t operator()(const PairKey& key) const {
      return std::hash<size_t>()(key.left) ^ (std::hash<size_t>()(key.right) << 1);
    }
  };
  std::unordered_map<PairKey, size_t, PairKeyHash> pair_ids;
  pair_ids.reserve(n);
  auto get_pair_id = [&](size_t left, size_t right) {
    PairKey key{left, right};
    if (right < left) {
      key.left = right;
      key.right = left;
    }
    const auto it = pair_ids.find(key);
    if (it != pair_ids.end()) {
      return it->second;
    }
    const size_t next_id = pair_ids.size() + 1;
    pair_ids.emplace(key, next_id);
    return next_id;
  };
  // Prepare rows for console and CSV output.
  for (size_t i = 0; i < n; i++) {
    const size_t match_index = best_index[i];
    if (match_index < n && best_index[match_index] == i &&
        IsDedupScore(best_score[i], output_pair_threshold) &&
        IsDedupScore(best_score[match_index], output_pair_threshold)) {
      const size_t duplicate_index = ChooseMoveIndex(i, match_index, version_infos, mod_times);
      const size_t keeper_index = (duplicate_index == i) ? match_index : i;
      if (i != keeper_index) {
        continue;
      }
      const std::string file_path = MaskedPath(files[keeper_index], mask_root);
      const std::string match_path = MaskedPath(files[duplicate_index], mask_root);
      const double score = best_score[keeper_index];
      const size_t pair_id = get_pair_id(keeper_index, duplicate_index);
      rows.push_back({file_path, match_path, score, pair_id});
      continue;
    }
    const std::string file_path = MaskedPath(files[i], mask_root);
    const std::string match_path = (match_index < n)
      ? MaskedPath(files[match_index], mask_root)
      : std::string();
    const double score = best_score[i];
    const size_t pair_id = (match_index < n) ? get_pair_id(i, match_index) : get_pair_id(i, i);
    rows.push_back({file_path, match_path, score, pair_id});
  }
  // Highest similarity first, stable for ties.
  std::stable_sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
    return left.score > right.score;
  });

  std::vector<std::string> score_texts;
  score_texts.reserve(rows.size());
  const std::string header_file = "File";
  const std::string header_match = "MostSimilar";
  const std::string header_score = "Score";
  size_t file_width = header_file.size();
  size_t match_width = header_match.size();
  size_t score_width = header_score.size();
  for (const auto& row : rows) {
    const std::string score_text = FormatScore(row.score);
    score_texts.push_back(score_text);
    file_width = std::max(file_width, row.file.size());
    match_width = std::max(match_width, row.match.size());
    score_width = std::max(score_width, score_text.size());
  }

  const size_t max_total_width = 132;
  // Clamp table width by shrinking file/match columns proportionally.
  if (file_width + match_width + score_width + 10 > max_total_width) {
    const size_t min_width = 10;
    const size_t max_sum = (max_total_width > score_width + 10)
      ? (max_total_width - score_width - 10)
      : (min_width * 2);
    const size_t total_text = std::max<size_t>(1, file_width + match_width);
    file_width = std::max(min_width,
                          std::min(file_width, (max_sum * file_width) / total_text));
    match_width = std::max(min_width, max_sum - file_width);
    if (file_width + match_width > max_sum) {
      match_width = max_sum - file_width;
    }
  }

  PrintSeparator(file_width, match_width, score_width);
  PrintRow(header_file, header_match, header_score, file_width, match_width, score_width);
  PrintSeparator(file_width, match_width, score_width);
  for (size_t i = 0; i < rows.size(); i++) {
    PrintWrappedRow(rows[i].file, rows[i].match, score_texts[i], file_width, match_width, score_width);
  }
  PrintSeparator(file_width, match_width, score_width);
  std::cout << "* Reciprocal best matches are shown once; the left column is the preferred file\n"
            << "  and the right column is the duplicate candidate (threshold 0.00000001), chosen\n"
            << "  by filename version/date markers, then modification time, then scan order.\n";

  std::ofstream csv(output_path);
  if (!csv) {
    std::fprintf(stderr, "Failed to open output file: %s\n", output_path.string().c_str());
    return 2;
  }
  csv << "file,most_similar,score,pair_id\n";
  csv << std::fixed << std::setprecision(8);
  for (const auto& row : rows) {
    csv << CsvEscape(row.file) << "," << CsvEscape(row.match) << ","
        << row.score << "," << row.pair_id << "\n";
  }

  std::fprintf(stderr, "CSV generated: %s\n", output_path.string().c_str());
  int status = 0;
  if (dedup) {
    if (dedup_sources.empty()) {
      std::fprintf(stderr, "Dedup: no matches at or above the threshold.\n");
    }
    else {
      std::error_code dir_ec;
      fs::create_directories(duplicates_dir, dir_ec);
      if (dir_ec) {
        std::fprintf(stderr,
                     "Dedup: failed to create %s (%s)\n",
                     duplicates_dir.string().c_str(),
                     dir_ec.message().c_str());
        status = 2;
      }
      else {
        size_t moved = 0;
        size_t failed = 0;
        for (const auto& source : dedup_sources) {
          std::error_code exists_ec;
          if (!fs::exists(source, exists_ec) || exists_ec) {
            failed++;
            continue;
          }
          fs::path target = duplicates_dir / RelativeToRoot(source, root);
          target = MakeUniquePath(target);
          std::error_code parent_ec;
          fs::create_directories(target.parent_path(), parent_ec);
          if (parent_ec) {
            failed++;
            continue;
          }
          std::error_code move_ec;
          fs::rename(source, target, move_ec);
          if (move_ec) {
            std::error_code copy_ec;
            fs::copy_file(source, target, fs::copy_options::none, copy_ec);
            if (copy_ec) {
              failed++;
              continue;
            }
            std::error_code remove_ec;
            fs::remove(source, remove_ec);
            if (remove_ec) {
              failed++;
              continue;
            }
          }
          moved++;
        }
        std::fprintf(stderr, "Dedup: moved %zu file(s) to %s\n", moved, duplicates_dir.string().c_str());
        if (failed > 0) {
          std::fprintf(stderr, "Dedup: %zu file(s) could not be moved.\n", failed);
          status = 2;
        }
      }
    }
  }
  if (!threads_specified) {
    std::fprintf(stderr, "Threads used (max): %u\n", worker_count);
  }
  return status;
}
