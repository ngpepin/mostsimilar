#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "Statistics.h"

namespace {

constexpr char32_t kReplacementChar = 0xFFFD;

bool DecodeUtf8(const std::string& data, size_t& index, char32_t& out, bool final_chunk) {
  const size_t len = data.size();
  if (index >= len) {
    return false;
  }
  const unsigned char c0 = static_cast<unsigned char>(data[index]);
  if (c0 < 0x80) {
    out = c0;
    index += 1;
    return true;
  }
  if (c0 < 0xC2) {
    out = kReplacementChar;
    index += 1;
    return true;
  }
  if (c0 < 0xE0) {
    if (index + 1 >= len) {
      if (final_chunk) {
        out = kReplacementChar;
        index += 1;
        return true;
      }
      return false;
    }
    const unsigned char c1 = static_cast<unsigned char>(data[index + 1]);
    if ((c1 & 0xC0) != 0x80) {
      out = kReplacementChar;
      index += 1;
      return true;
    }
    out = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
    index += 2;
    return true;
  }
  if (c0 < 0xF0) {
    if (index + 2 >= len) {
      if (final_chunk) {
        out = kReplacementChar;
        index += 1;
        return true;
      }
      return false;
    }
    const unsigned char c1 = static_cast<unsigned char>(data[index + 1]);
    const unsigned char c2 = static_cast<unsigned char>(data[index + 2]);
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
      out = kReplacementChar;
      index += 1;
      return true;
    }
    if (c0 == 0xE0 && c1 < 0xA0) {
      out = kReplacementChar;
      index += 1;
      return true;
    }
    if (c0 == 0xED && c1 >= 0xA0) {
      out = kReplacementChar;
      index += 1;
      return true;
    }
    out = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
    index += 3;
    return true;
  }
  if (c0 < 0xF5) {
    if (index + 3 >= len) {
      if (final_chunk) {
        out = kReplacementChar;
        index += 1;
        return true;
      }
      return false;
    }
    const unsigned char c1 = static_cast<unsigned char>(data[index + 1]);
    const unsigned char c2 = static_cast<unsigned char>(data[index + 2]);
    const unsigned char c3 = static_cast<unsigned char>(data[index + 3]);
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
      out = kReplacementChar;
      index += 1;
      return true;
    }
    if (c0 == 0xF0 && c1 < 0x90) {
      out = kReplacementChar;
      index += 1;
      return true;
    }
    if (c0 == 0xF4 && c1 >= 0x90) {
      out = kReplacementChar;
      index += 1;
      return true;
    }
    out = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
    index += 4;
    return true;
  }
  out = kReplacementChar;
  index += 1;
  return true;
}

void AppendUtf8(std::string& out, char32_t codepoint) {
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
  else {
    out.push_back(static_cast<char>(0xEF));
    out.push_back(static_cast<char>(0xBF));
    out.push_back(static_cast<char>(0xBD));
  }
}

std::locale MakeLocale() {
  try {
    return std::locale("");
  }
  catch (...) {
    return std::locale::classic();
  }
}

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint64_t kSimHashSeedHigh = 0x9E3779B185EBCA87ULL;

uint64_t Fnv1aHash64(const std::string& text, uint64_t seed) {
  uint64_t hash = kFnvOffset ^ seed;
  for (unsigned char ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

int Popcount64(uint64_t value) {
  int count = 0;
  while (value != 0) {
    value &= (value - 1);
    count++;
  }
  return count;
}

const std::unordered_set<std::string>& StopWords() {
  // English, French, and Spanish stop words (ASCII-only list to match tokenizer output).
  static const std::unordered_set<std::string> kStopWords = {
    "a", "about", "above", "after", "again", "against", "all", "am", "an",
    "and", "any", "are", "aren", "as", "at", "be", "because", "been", "before",
    "being", "below", "between", "both", "but", "by", "can", "could", "couldn",
    "did", "didn", "do", "does", "doesn", "doing", "don", "down", "during",
    "each", "few", "for", "from", "further", "had", "hadn", "has", "hasn",
    "have", "haven", "having", "he", "her", "here", "hers", "herself", "him",
    "himself", "his", "how", "i", "if", "in", "into", "is", "isn", "it",
    "its", "itself", "just", "let", "ll", "me", "more", "most", "mustn", "my",
    "myself", "no", "nor", "not", "now", "o", "of", "off", "on", "once",
    "only", "or", "other", "our", "ours", "ourselves", "out", "over", "own",
    "re", "s", "same", "shan", "she", "should", "shouldn", "so", "some",
    "such", "t", "than", "that", "the", "their", "theirs", "them",
    "themselves", "then", "there", "these", "they", "this", "those", "through",
    "to", "too", "under", "until", "up", "very", "was", "wasn", "we", "were",
    "weren", "what", "when", "where", "which", "while", "who", "whom", "why",
    "will", "with", "won", "would", "wouldn", "y", "you", "your", "yours",
    "yourself", "yourselves",
    // French (ASCII-only forms).
    "au", "aux", "ce", "ces", "cet", "cette", "dans", "de", "des", "donc",
    "du", "elle", "elles", "en", "et", "il", "ils", "je", "la", "le", "les",
    "leur", "leurs", "l", "mais", "ne", "ni", "nous", "on", "or", "ou", "pas",
    "plus", "pour", "qu", "que", "qui", "quoi", "sa", "sans", "se", "ses",
    "son", "sur", "tu", "un", "une", "vous",
    // Spanish (ASCII-only forms).
    "al", "como", "con", "cuando", "de", "del", "donde", "el", "ella",
    "ellas", "ellos", "en", "es", "esa", "esas", "ese", "esos", "esta",
    "estas", "este", "estos", "la", "las", "lo", "los", "mas", "me", "mi",
    "mis", "mucho", "muy", "no", "nos", "o", "para", "pero", "por", "porque",
    "que", "quien", "quienes", "se", "si", "sin", "su", "sus", "te", "tu",
    "tus", "una", "unas", "uno", "unos", "ya", "y"
  };
  return kStopWords;
}

bool IsStopWord(const std::string& token) {
  return StopWords().find(token) != StopWords().end();
}

} // namespace

Statistics::Statistics(const std::string& text) {
  AddText(text);
}

void Statistics::AddToken(std::string& token) {
  if (token.empty()) {
    return;
  }
  // Drop common stop words so they don't dominate the scoring.
  if (IsStopWord(token)) {
    token.clear();
    return;
  }
  auto it = _counts.find(token);
  if (it == _counts.end()) {
    _counts.emplace(token, 1);
  }
  else {
    it->second++;
  }
  _totWords++;
  token.clear();
}

void Statistics::AddText(const std::string& text) {
  StatisticsTokenizer tokenizer(*this);
  tokenizer.AddChunk(text.data(), text.size());
  tokenizer.Finish();
}

void Statistics::Clear() {
  _counts.clear();
  _totWords = 0;
}

double Statistics::Dist(const Statistics& fellow) const {
  // L2 distance between normalized term-frequency vectors.
  double sum = 0;
  for (const auto& wordInfo : _counts) {
    const std::string& wordText = wordInfo.first;
    const double freq = double(wordInfo.second) / _totWords;
    auto it = fellow._counts.find(wordText);
    const double fellowFreq = (it == fellow._counts.end())
      ? 0.0
      : double(it->second) / fellow._totWords;
    const double d = freq - fellowFreq;
    sum += d * d;
  }
  for (const auto& wordInfo : fellow._counts) {
    if (_counts.find(wordInfo.first) != _counts.end()) {
      continue;
    }
    const double fellowFreq = double(wordInfo.second) / fellow._totWords;
    sum += fellowFreq * fellowFreq;
  }
  return std::sqrt(sum);
}

Statistics::SimHash128 Statistics::SimHash128Signature() const {
  // SimHash: hash tokens, weight by count, and take the sign per bit.
  std::array<int64_t, 128> weights{};
  for (const auto& wordInfo : _counts) {
    const uint64_t hash_low = Fnv1aHash64(wordInfo.first, 0);
    const uint64_t hash_high = Fnv1aHash64(wordInfo.first, kSimHashSeedHigh);
    const int64_t weight = wordInfo.second;
    for (int bit = 0; bit < 64; bit++) {
      if (hash_low & (1ULL << bit)) {
        weights[bit] += weight;
      }
      else {
        weights[bit] -= weight;
      }
    }
    for (int bit = 0; bit < 64; bit++) {
      if (hash_high & (1ULL << bit)) {
        weights[bit + 64] += weight;
      }
      else {
        weights[bit + 64] -= weight;
      }
    }
  }
  SimHash128 out;
  for (int bit = 0; bit < 64; bit++) {
    if (weights[bit] >= 0) {
      out.low |= (1ULL << bit);
    }
  }
  for (int bit = 0; bit < 64; bit++) {
    if (weights[bit + 64] >= 0) {
      out.high |= (1ULL << bit);
    }
  }
  return out;
}

double Statistics::SimHashDistance(const SimHash128& left, const SimHash128& right) {
  const uint64_t diff_low = left.low ^ right.low;
  const uint64_t diff_high = left.high ^ right.high;
  const int distance = Popcount64(diff_low) + Popcount64(diff_high);
  return static_cast<double>(distance) / 128.0;
}

double Statistics::SimHashSimilarity(const SimHash128& left, const SimHash128& right) {
  return 1.0 - SimHashDistance(left, right);
}

double Statistics::DistanceToSimilarity(double distance) {
  const double max_dist = std::sqrt(2.0);
  if (max_dist <= 0.0) {
    return 0.0;
  }
  double score = 1.0 - (distance / max_dist);
  if (score < 0.0) {
    score = 0.0;
  }
  if (score > 1.0) {
    score = 1.0;
  }
  return score;
}

double Statistics::TfIdfCosineSimilarity(const Statistics& left, const Statistics& right) {
  if (left._totWords == 0 || right._totWords == 0) {
    return 0.0;
  }
  // Pairwise IDF: compute weights from the two documents only.
  const double total_terms = static_cast<double>(left._totWords + right._totWords);
  if (total_terms <= 0.0) {
    return 0.0;
  }
  double dot = 0.0;
  double norm_left = 0.0;
  double norm_right = 0.0;
  for (const auto& wordInfo : left._counts) {
    const auto right_it = right._counts.find(wordInfo.first);
    const int64_t right_count = (right_it == right._counts.end()) ? 0 : right_it->second;
    const double combined = static_cast<double>(wordInfo.second + right_count);
    const double idf = std::log((total_terms + 1.0) / (combined + 1.0)) + 1.0;
    const double left_tf = static_cast<double>(wordInfo.second) / left._totWords;
    const double left_weight = left_tf * idf;
    norm_left += left_weight * left_weight;
    if (right_count > 0) {
      const double right_tf = static_cast<double>(right_count) / right._totWords;
      const double right_weight = right_tf * idf;
      dot += left_weight * right_weight;
    }
  }
  for (const auto& wordInfo : right._counts) {
    const auto left_it = left._counts.find(wordInfo.first);
    const int64_t left_count = (left_it == left._counts.end()) ? 0 : left_it->second;
    const double combined = static_cast<double>(wordInfo.second + left_count);
    const double idf = std::log((total_terms + 1.0) / (combined + 1.0)) + 1.0;
    const double right_tf = static_cast<double>(wordInfo.second) / right._totWords;
    const double right_weight = right_tf * idf;
    norm_right += right_weight * right_weight;
  }
  if (norm_left <= 0.0 || norm_right <= 0.0) {
    return 0.0;
  }
  const double denom = std::sqrt(norm_left) * std::sqrt(norm_right);
  if (denom <= 0.0) {
    return 0.0;
  }
  // Cosine similarity of the weighted vectors.
  double score = dot / denom;
  if (score < 0.0) {
    score = 0.0;
  }
  if (score > 1.0) {
    score = 1.0;
  }
  return score;
}

StatisticsTokenizer::StatisticsTokenizer(Statistics& stats)
  : _stats(stats),
    _locale(MakeLocale()),
    _ctype(std::use_facet<std::ctype<wchar_t>>(_locale)) {}

void StatisticsTokenizer::AddChunk(const char* data, size_t len) {
  _pending.append(data, len);
  ProcessBuffer(false);
}

void StatisticsTokenizer::Finish() {
  ProcessBuffer(true);
  _stats.AddToken(_token);
}

void StatisticsTokenizer::ProcessBuffer(bool final_chunk) {
  size_t index = 0;
  while (index < _pending.size()) {
    char32_t codepoint = 0;
    if (!DecodeUtf8(_pending, index, codepoint, final_chunk)) {
      break;
    }
    if (codepoint == kReplacementChar) {
      _stats.AddToken(_token);
      continue;
    }
    if (codepoint <= WCHAR_MAX) {
      const wchar_t wch = static_cast<wchar_t>(codepoint);
      const std::ctype_base::mask mask = std::ctype_base::space | std::ctype_base::punct | std::ctype_base::cntrl;
      if (_ctype.is(mask, wch)) {
        // Whitespace/punctuation ends the current token.
        _stats.AddToken(_token);
        continue;
      }
      const wchar_t lowered = _ctype.tolower(wch);
      codepoint = static_cast<char32_t>(lowered);
    }
    // Accumulate the current token in UTF-8.
    AppendUtf8(_token, codepoint);
  }
  if (index > 0) {
    _pending.erase(0, index);
  }
  if (final_chunk && !_pending.empty()) {
    _pending.clear();
  }
}
