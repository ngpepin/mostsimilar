#pragma once

#include <cstddef>
#include <cstdint>
#include <locale>
#include <string>
#include <unordered_map>

class Statistics {
  std::unordered_map<std::string, int64_t> _counts;
  int64_t _totWords = 0;
public:
  struct SimHash128 {
    uint64_t high = 0;
    uint64_t low = 0;
  };

  Statistics() = default;
  explicit Statistics(const std::string& text);

  void AddToken(std::string& token);
  void AddText(const std::string& text);
  void Clear();

  double Dist(const Statistics& fellow) const;
  // 128-bit SimHash signature built from token counts.
  SimHash128 SimHash128Signature() const;
  static double SimHashDistance(const SimHash128& left, const SimHash128& right);
  static double SimHashSimilarity(const SimHash128& left, const SimHash128& right);
  static double DistanceToSimilarity(double distance);
  // Pairwise TF-IDF cosine similarity (weights based on this pair only).
  static double TfIdfCosineSimilarity(const Statistics& left, const Statistics& right);

  bool IsEmpty() const { return _totWords == 0; }
};

class StatisticsTokenizer {
public:
  explicit StatisticsTokenizer(Statistics& stats);
  void AddChunk(const char* data, size_t len);
  void Finish();

private:
  void ProcessBuffer(bool final_chunk);

  Statistics& _stats;
  std::string _pending;
  std::string _token;
  std::locale _locale;
  const std::ctype<wchar_t>& _ctype;
};
