#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "miniz.h"
#include "Statistics.h"
#include "Utils.h"

#if defined(HAVE_POPPLER_CPP)
#include <poppler-document.h>
#include <poppler-global.h>
#include <poppler-page.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string ToLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string GetLowerExtension(const std::string& filePath) {
  const fs::path path(filePath);
  return ToLowerCopy(path.extension().string());
}

bool StartsWith(const std::string& value, const char* prefix) {
  const size_t prefix_len = std::strlen(prefix);
  return value.size() >= prefix_len && value.compare(0, prefix_len, prefix) == 0;
}

bool IsAsciiTextChar(unsigned char ch) {
  return (ch >= 0x20 && ch <= 0x7E) || ch == '\n' || ch == '\r' || ch == '\t';
}

bool HasFilePrefix(const std::string& filePath, const unsigned char* prefix, size_t prefix_len) {
  if (prefix_len == 0) {
    return false;
  }
  FILE* fpin = std::fopen(filePath.c_str(), "rb");
  if (fpin == nullptr) {
    return false;
  }
  std::array<unsigned char, 8> buffer{};
  const size_t read_size = std::fread(buffer.data(), 1, std::min(buffer.size(), prefix_len), fpin);
  std::fclose(fpin);
  if (read_size < prefix_len) {
    return false;
  }
  return std::memcmp(buffer.data(), prefix, prefix_len) == 0;
}

bool LooksLikePdf(const std::string& filePath) {
  static const unsigned char kPdfSig[] = {'%', 'P', 'D', 'F', '-'};
  return HasFilePrefix(filePath, kPdfSig, sizeof(kPdfSig));
}

bool LooksLikeZip(const std::string& filePath) {
  static const unsigned char kZipSig[] = {'P', 'K'};
  return HasFilePrefix(filePath, kZipSig, sizeof(kZipSig));
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

void AppendUtf8(std::string& out, uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  }
  else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else if (codepoint <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

void FlushBuffer(StatisticsTokenizer& tokenizer, std::string& buffer) {
  if (!buffer.empty()) {
    tokenizer.AddChunk(buffer.data(), buffer.size());
    buffer.clear();
  }
}

bool DecodeXmlEntity(const char* data, size_t size, size_t& index, std::string& buffer) {
  const size_t start = index + 1;
  size_t pos = start;
  while (pos < size && pos - start <= 12 && data[pos] != ';') {
    pos++;
  }
  if (pos >= size || data[pos] != ';') {
    return false;
  }
  const size_t len = pos - start;
  if (len == 2 && std::memcmp(data + start, "lt", len) == 0) {
    buffer.push_back('<');
  }
  else if (len == 2 && std::memcmp(data + start, "gt", len) == 0) {
    buffer.push_back('>');
  }
  else if (len == 3 && std::memcmp(data + start, "amp", len) == 0) {
    buffer.push_back('&');
  }
  else if (len == 4 && std::memcmp(data + start, "quot", len) == 0) {
    buffer.push_back('"');
  }
  else if (len == 4 && std::memcmp(data + start, "apos", len) == 0) {
    buffer.push_back('\'');
  }
  else if (len == 4 && std::memcmp(data + start, "nbsp", len) == 0) {
    buffer.push_back(' ');
  }
  else if (len >= 2 && data[start] == '#') {
    uint32_t value = 0;
    bool hex = false;
    size_t cursor = start + 1;
    if (cursor < pos && (data[cursor] == 'x' || data[cursor] == 'X')) {
      hex = true;
      cursor++;
    }
    while (cursor < pos) {
      const char ch = data[cursor];
      int digit = hex ? HexValue(ch) : (ch >= '0' && ch <= '9' ? (ch - '0') : -1);
      if (digit < 0) {
        return false;
      }
      value = hex ? (value * 16 + static_cast<uint32_t>(digit))
                  : (value * 10 + static_cast<uint32_t>(digit));
      cursor++;
    }
    AppendUtf8(buffer, value);
  }
  else {
    return false;
  }
  index = pos + 1;
  return true;
}

// Strip XML tags and decode basic entities into plain text tokens.
void ExtractXmlText(const char* data, size_t size, StatisticsTokenizer& tokenizer) {
  std::string buffer;
  buffer.reserve(4096);
  bool in_tag = false;
  size_t i = 0;
  while (i < size) {
    const char ch = data[i];
    if (!in_tag && ch == '<') {
      if (i + 9 < size && std::memcmp(data + i, "<![CDATA[", 9) == 0) {
        size_t end = i + 9;
        while (end + 2 < size &&
               !(data[end] == ']' && data[end + 1] == ']' && data[end + 2] == '>')) {
          end++;
        }
        if (end + 2 >= size) {
          break;
        }
        buffer.append(data + i + 9, end - (i + 9));
        i = end + 3;
        if (buffer.size() >= 4096) {
          FlushBuffer(tokenizer, buffer);
        }
        continue;
      }
      in_tag = true;
      i++;
      continue;
    }
    if (in_tag) {
      if (ch == '>') {
        in_tag = false;
      }
      i++;
      continue;
    }
    if (ch == '&') {
      const size_t entity_start = i;
      if (DecodeXmlEntity(data, size, i, buffer)) {
        if (buffer.size() >= 4096) {
          FlushBuffer(tokenizer, buffer);
        }
        continue;
      }
      buffer.push_back('&');
      i = entity_start + 1;
    }
    else {
      buffer.push_back(ch);
      i++;
    }
    if (buffer.size() >= 4096) {
      FlushBuffer(tokenizer, buffer);
    }
  }
  FlushBuffer(tokenizer, buffer);
}

bool IsDocxLikeExtension(const std::string& ext) {
  return ext == ".docx" || ext == ".docm" || ext == ".dotx" || ext == ".dotm";
}

bool IsPptxLikeExtension(const std::string& ext) {
  return ext == ".pptx" || ext == ".pptm" || ext == ".potx" ||
         ext == ".potm" || ext == ".ppsx" || ext == ".ppsm";
}

bool IsXlsxLikeExtension(const std::string& ext) {
  return ext == ".xlsx" || ext == ".xlsm" || ext == ".xltx" || ext == ".xltm";
}

bool IsOdfExtension(const std::string& ext) {
  return ext == ".odt" || ext == ".ods" || ext == ".odp";
}

bool IsZipOfficeExtension(const std::string& ext) {
  return IsDocxLikeExtension(ext) || IsPptxLikeExtension(ext) ||
         IsXlsxLikeExtension(ext) || IsOdfExtension(ext);
}

bool IsLegacyOfficeExtension(const std::string& ext) {
  return ext == ".doc" || ext == ".dot" || ext == ".xls" || ext == ".xlt" ||
         ext == ".ppt" || ext == ".pps" || ext == ".pot";
}

bool ShouldExtractZipEntry(const std::string& ext, const std::string& name) {
  if (IsDocxLikeExtension(ext)) {
    return name == "word/document.xml" ||
           name == "word/footnotes.xml" ||
           name == "word/endnotes.xml" ||
           StartsWith(name, "word/header") ||
           StartsWith(name, "word/footer");
  }
  if (IsPptxLikeExtension(ext)) {
    return StartsWith(name, "ppt/slides/") || StartsWith(name, "ppt/notesslides/");
  }
  if (IsXlsxLikeExtension(ext)) {
    return name == "xl/sharedstrings.xml" || StartsWith(name, "xl/worksheets/");
  }
  if (IsOdfExtension(ext)) {
    return name == "content.xml" || name == "styles.xml";
  }
  return false;
}

// Extract text from zipped XML containers (OOXML/ODF formats).
bool ExtractZipXmlText(const std::string& filePath, const std::string& ext, Statistics* stats) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, filePath.c_str(), 0)) {
    return false;
  }
  bool extracted = false;
  StatisticsTokenizer tokenizer(*stats);
  const int file_count = static_cast<int>(mz_zip_reader_get_num_files(&zip));
  for (int i = 0; i < file_count; i++) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
      continue;
    }
    if (stat.m_is_directory) {
      continue;
    }
    std::string name = ToLowerCopy(stat.m_filename ? stat.m_filename : "");
    if (!ShouldExtractZipEntry(ext, name)) {
      continue;
    }
    size_t out_size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, i, &out_size, 0);
    if (data == nullptr || out_size == 0) {
      if (data != nullptr) {
        mz_free(data);
      }
      continue;
    }
    ExtractXmlText(static_cast<const char*>(data), out_size, tokenizer);
    mz_free(data);
    extracted = true;
  }
  mz_zip_reader_end(&zip);
  tokenizer.Finish();
  return extracted && !stats->IsEmpty();
}

#if defined(HAVE_POPPLER_CPP)
void SuppressPopplerErrorsOnce() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    poppler::set_debug_error_function(
      [](const std::string& message, void* /*closure*/) {
        (void)message;
      },
      nullptr);
  });
}

#endif

// Extract text from PDFs via poppler-cpp (best effort).
bool ExtractPdfText(const std::string& filePath, Statistics* stats, bool safe_mode) {
#if defined(HAVE_POPPLER_CPP)
  SuppressPopplerErrorsOnce();
  if (safe_mode) {
    static std::mutex poppler_mutex;
    std::lock_guard<std::mutex> lock(poppler_mutex);
    std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(filePath));
    if (!doc) {
      return false;
    }
    StatisticsTokenizer tokenizer(*stats);
    const int page_count = doc->pages();
    for (int i = 0; i < page_count; i++) {
      std::unique_ptr<poppler::page> page(doc->create_page(i));
      if (!page) {
        continue;
      }
      const auto bytes = page->text().to_utf8();
      if (!bytes.empty()) {
        tokenizer.AddChunk(bytes.data(), bytes.size());
        tokenizer.AddChunk("\n", 1);
      }
    }
    tokenizer.Finish();
    return !stats->IsEmpty();
  }
  std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(filePath));
  if (!doc) {
    return false;
  }
  StatisticsTokenizer tokenizer(*stats);
  const int page_count = doc->pages();
  for (int i = 0; i < page_count; i++) {
    std::unique_ptr<poppler::page> page(doc->create_page(i));
    if (!page) {
      continue;
    }
    const auto bytes = page->text().to_utf8();
    if (!bytes.empty()) {
      tokenizer.AddChunk(bytes.data(), bytes.size());
      tokenizer.AddChunk("\n", 1);
    }
  }
  tokenizer.Finish();
  return !stats->IsEmpty();
#else
  (void)filePath;
  (void)stats;
  return false;
#endif
}

// Minimal RTF parser that pulls visible text and common control breaks.
bool ExtractRtfText(const std::string& filePath, Statistics* stats) {
  const std::string data = ReadAllText(filePath);
  if (data.empty()) {
    return false;
  }
  StatisticsTokenizer tokenizer(*stats);
  std::string buffer;
  buffer.reserve(4096);
  size_t i = 0;
  while (i < data.size()) {
    const char ch = data[i];
    if (ch == '{' || ch == '}') {
      i++;
      continue;
    }
    if (ch == '\\') {
      if (i + 1 >= data.size()) {
        break;
      }
      const char next = data[i + 1];
      if (next == '\\' || next == '{' || next == '}') {
        buffer.push_back(next);
        i += 2;
      }
      else if (next == '\'' && i + 3 < data.size()) {
        const int hi = HexValue(data[i + 2]);
        const int lo = HexValue(data[i + 3]);
        if (hi >= 0 && lo >= 0) {
          buffer.push_back(static_cast<char>((hi << 4) | lo));
          i += 4;
        }
        else {
          i += 2;
        }
      }
      else if (next == 'u') {
        i += 2;
        int sign = 1;
        if (i < data.size() && data[i] == '-') {
          sign = -1;
          i++;
        }
        int value = 0;
        while (i < data.size() && std::isdigit(static_cast<unsigned char>(data[i]))) {
          value = value * 10 + (data[i] - '0');
          i++;
        }
        int32_t codepoint = sign * value;
        if (codepoint < 0) {
          codepoint += 65536;
        }
        AppendUtf8(buffer, static_cast<uint32_t>(codepoint));
        if (i < data.size() && data[i] == '?') {
          i++;
        }
        if (i < data.size() && data[i] == ' ') {
          i++;
        }
      }
      else if (std::isalpha(static_cast<unsigned char>(next))) {
        std::string word;
        size_t cursor = i + 1;
        while (cursor < data.size() && std::isalpha(static_cast<unsigned char>(data[cursor]))) {
          word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(data[cursor]))));
          cursor++;
        }
        if (cursor < data.size() && (data[cursor] == '-' || std::isdigit(static_cast<unsigned char>(data[cursor])))) {
          if (data[cursor] == '-') {
            cursor++;
          }
          while (cursor < data.size() && std::isdigit(static_cast<unsigned char>(data[cursor]))) {
            cursor++;
          }
        }
        if (word == "par" || word == "line") {
          buffer.push_back('\n');
        }
        else if (word == "tab") {
          buffer.push_back('\t');
        }
        i = cursor;
        if (i < data.size() && data[i] == ' ') {
          i++;
        }
      }
      else {
        i += 2;
      }
    }
    else {
      buffer.push_back(ch);
      i++;
    }
    if (buffer.size() >= 4096) {
      FlushBuffer(tokenizer, buffer);
    }
  }
  FlushBuffer(tokenizer, buffer);
  tokenizer.Finish();
  return !stats->IsEmpty();
}

// Heuristic scan for ASCII/UTF-16LE runs inside binary files (legacy Office).
void ExtractBinaryText(const unsigned char* data, size_t size, StatisticsTokenizer& tokenizer) {
  constexpr size_t kMinAsciiChars = 4;
  constexpr size_t kMinUtf16Chars = 4;

  size_t i = 0;
  while (i < size) {
    if (i + 1 < size && data[i + 1] == 0 && IsAsciiTextChar(data[i])) {
      size_t j = i;
      size_t count = 0;
      while (j + 1 < size && data[j + 1] == 0 && IsAsciiTextChar(data[j])) {
        count++;
        if (count >= kMinUtf16Chars) {
          break;
        }
        j += 2;
      }
      if (count >= kMinUtf16Chars) {
        std::string segment;
        j = i;
        while (j + 1 < size && data[j + 1] == 0 && IsAsciiTextChar(data[j])) {
          segment.push_back(static_cast<char>(data[j]));
          j += 2;
        }
        tokenizer.AddChunk(segment.data(), segment.size());
        tokenizer.AddChunk("\n", 1);
        i = j;
        continue;
      }
    }

    if (IsAsciiTextChar(data[i])) {
      size_t j = i;
      std::string segment;
      while (j < size && IsAsciiTextChar(data[j])) {
        segment.push_back(static_cast<char>(data[j]));
        j++;
      }
      if (segment.size() >= kMinAsciiChars) {
        tokenizer.AddChunk(segment.data(), segment.size());
        tokenizer.AddChunk("\n", 1);
      }
      i = j;
      continue;
    }

    i++;
  }
}

// Best-effort text extraction for legacy Office binaries.
bool ExtractLegacyOfficeText(const std::string& filePath, Statistics* stats) {
  const std::string data = ReadAllText(filePath);
  if (data.empty()) {
    return false;
  }
  StatisticsTokenizer tokenizer(*stats);
  ExtractBinaryText(reinterpret_cast<const unsigned char*>(data.data()), data.size(), tokenizer);
  tokenizer.Finish();
  return !stats->IsEmpty();
}

bool ReadRawFileToStatistics(const std::string& filePath, Statistics* stats) {
  FILE *fpin = fopen(filePath.c_str(), "rb");
  if (fpin == nullptr) {
    fprintf(stderr, "Cannot open file: %s\n", filePath.c_str());
    return false;
  }
  std::array<char, 1 << 16> buf{};
  StatisticsTokenizer tokenizer(*stats);
  size_t read_size = 0;
  while ((read_size = fread(buf.data(), 1, buf.size(), fpin)) > 0) {
    tokenizer.AddChunk(buf.data(), read_size);
  }
  if (ferror(fpin)) {
    fprintf(stderr, "Error reading file: %s\n", filePath.c_str());
    fclose(fpin);
    return false;
  }
  tokenizer.Finish();
  fclose(fpin);
  return true;
}

} // namespace

std::string ReadAllText(const std::string& filePath) {
  FILE *fpin = fopen(filePath.c_str(), "rb");
  if (fpin == nullptr) {
    fprintf(stderr, "Cannot open file: %s\n", filePath.c_str());
    return "";
  }
  std::string result;
  std::error_code ec;
  const auto size = fs::file_size(filePath, ec);
  if (!ec && size > 0) {
    result.resize(static_cast<size_t>(size));
    const size_t read_size = fread(result.data(), 1, result.size(), fpin);
    result.resize(read_size);
  }
  else {
    std::array<char, 1 << 15> buf{};
    size_t read_size = 0;
    while ((read_size = fread(buf.data(), 1, buf.size(), fpin)) > 0) {
      result.append(buf.data(), read_size);
    }
  }
  fclose(fpin);
  return result;
}

// Read and tokenize a file, using format-specific extractors when available.
bool ReadFileToStatistics(const std::string& filePath, Statistics* stats) {
  return ReadFileToStatistics(filePath, stats, false, false);
}

bool ReadFileToStatistics(const std::string& filePath,
                          Statistics* stats,
                          bool safe_mode,
                          bool no_convert) {
  if (stats == nullptr) {
    return false;
  }
  stats->Clear();
  if (no_convert) {
    return ReadRawFileToStatistics(filePath, stats);
  }
  const std::string ext = GetLowerExtension(filePath);
  if (ext == ".pdf") {
    if (LooksLikePdf(filePath) && ExtractPdfText(filePath, stats, safe_mode)) {
      return true;
    }
    stats->Clear();
  }
  if (ext == ".rtf") {
    if (ExtractRtfText(filePath, stats)) {
      return true;
    }
    stats->Clear();
  }
  if (IsZipOfficeExtension(ext)) {
    if (LooksLikeZip(filePath) && ExtractZipXmlText(filePath, ext, stats)) {
      return true;
    }
    stats->Clear();
  }
  if (IsLegacyOfficeExtension(ext)) {
    if (ExtractLegacyOfficeText(filePath, stats)) {
      return true;
    }
    stats->Clear();
  }
  return ReadRawFileToStatistics(filePath, stats);
}

bool IsAllowedTextFile(const std::string& filePath) {
  static const std::unordered_set<std::string> kAllowedExtensions = {
    ".1", ".1p", ".3", ".3p", ".adoc", ".ads", ".adb", ".ada", ".ahk", ".as",
    ".asm", ".asciidoc", ".awk", ".bash", ".bas", ".bat", ".bib", ".c", ".c++",
    ".cc", ".cfg", ".cl", ".clj", ".cljc", ".cljs", ".cmake", ".cmd", ".cob",
    ".cbl", ".coffee", ".conf", ".cp", ".cpp", ".cppm", ".cs", ".csproj",
    ".csx", ".css", ".csv", ".cxx", ".d", ".dart", ".diff", ".doc", ".docm",
    ".docx", ".dot", ".dotm", ".dotx",
    ".dpr", ".dts", ".dtsi", ".edn", ".el", ".elm", ".erl", ".ex", ".exs",
    ".f", ".f03", ".f08", ".f77", ".f90", ".f95", ".fish", ".for", ".fs",
    ".fsi", ".fsproj", ".fsx", ".fpp", ".go", ".gql", ".gradle", ".groovy",
    ".gvy", ".gyp", ".gypi", ".h", ".h++", ".hxx", ".hh", ".hpp", ".hrl",
    ".hs", ".htm", ".html", ".idl", ".inc", ".inl", ".ini", ".ipp", ".ipynb",
    ".ixx", ".java", ".jl", ".js", ".json", ".jsx", ".kt", ".kts", ".less",
    ".lhs", ".lisp", ".log", ".lua", ".m", ".make", ".markdown", ".md", ".mk",
    ".mm", ".mjs", ".cjs", ".ml", ".mli", ".mll", ".mly", ".mpp", ".nim",
    ".odin", ".odp", ".ods", ".odt", ".pas", ".p", ".php", ".phtml", ".phps",
    ".pl", ".pm", ".pod", ".pp", ".proto", ".ps1", ".psd1", ".psm1", ".py",
    ".pyi", ".pyw", ".pyx", ".pxd", ".qml", ".qbs", ".r", ".rake", ".rmd",
    ".rb", ".rei", ".res", ".rst", ".rs", ".rtf", ".s", ".S", ".scala", ".sc",
    ".scm", ".scss", ".sh", ".sql", ".ss", ".sld", ".sty", ".sv", ".svh",
    ".svg", ".swift", ".t", ".tex", ".thrift", ".toml", ".ts", ".tsv", ".tsx",
    ".txt", ".vala", ".vapi", ".vb", ".vba", ".vbs", ".v", ".vh", ".vhd",
    ".vhdl", ".vue", ".xaml", ".xsd", ".xsl", ".xslt", ".xml", ".yaml",
    ".yml", ".zsh", ".zig", ".pdf", ".pot", ".potm", ".potx", ".pps",
    ".ppsm", ".ppsx", ".ppt", ".pptm", ".pptx", ".xls", ".xlsm", ".xlsx",
    ".xlt", ".xltm", ".xltx"
  };
  const fs::path path(filePath);
  std::string ext = path.extension().string();
  if (ext.empty()) {
    return false;
  }
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return kAllowedExtensions.find(ext) != kAllowedExtensions.end();
}
