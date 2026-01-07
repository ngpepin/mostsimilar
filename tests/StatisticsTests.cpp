#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "Statistics.h"
#include "Utils.h"

namespace fs = std::filesystem;

namespace {

struct Entry {
  fs::path path;
  double score;
};

struct ExpectedEntry {
  const char* name;
};

std::string NormalizeFilename(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (size_t i = 0; i < name.size(); ) {
    if (i + 3 <= name.size() &&
        static_cast<unsigned char>(name[i]) == 0xE2 &&
        static_cast<unsigned char>(name[i + 1]) == 0x80 &&
        static_cast<unsigned char>(name[i + 2]) == 0x99) {
      out.push_back('\'');
      i += 3;
      continue;
    }
    out.push_back(name[i]);
    i++;
  }
  return out;
}

std::vector<Entry> ComputeSimilarities(const fs::path& sample_path, const fs::path& repo_dir) {
  Statistics sample_stat;
  REQUIRE_MESSAGE(ReadFileToStatistics(sample_path.string(), &sample_stat), "Failed to read sample file.");
  REQUIRE_MESSAGE(!sample_stat.IsEmpty(), "Sample file must be non-empty.");
  std::vector<Entry> entries;
  std::vector<Statistics> repo_stats;
  std::vector<fs::path> repo_paths;
  for (const fs::directory_entry& repo_file : fs::directory_iterator(repo_dir)) {
    if (!repo_file.is_regular_file()) {
      continue;
    }
    Statistics repo_stat;
    if (!ReadFileToStatistics(repo_file.path().string(), &repo_stat)) {
      continue;
    }
    if (repo_stat.IsEmpty()) {
      continue;
    }
    repo_paths.push_back(repo_file.path());
    repo_stats.push_back(std::move(repo_stat));
  }
  entries.reserve(repo_stats.size());
  for (size_t i = 0; i < repo_stats.size(); i++) {
    const double score = Statistics::TfIdfCosineSimilarity(sample_stat, repo_stats[i]);
    entries.push_back({repo_paths[i], score});
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.path < b.path;
  });
  return entries;
}

} // namespace

TEST_CASE("Statistics treats punctuation and case consistently") {
  const Statistics sample("Hello, world!");
  const Statistics other("hello world");
  CHECK(sample.Dist(other) == doctest::Approx(0.0));
}

TEST_CASE("Statistics distance is stable for simple inputs") {
  const Statistics sample("alpha alpha beta");
  const Statistics other("alpha beta beta");
  CHECK(sample.Dist(other) == doctest::Approx(std::sqrt(2.0) / 3.0));
}

TEST_CASE("SimHash distance is zero for identical content") {
  const Statistics sample("hello world");
  const Statistics other("hello world");
  const auto sample_hash = sample.SimHash128Signature();
  const auto other_hash = other.SimHash128Signature();
  CHECK(Statistics::SimHashDistance(sample_hash, other_hash) == doctest::Approx(0.0));
  CHECK(Statistics::SimHashSimilarity(sample_hash, other_hash) == doctest::Approx(1.0));
}

TEST_CASE("SimHash differs for different content") {
  const Statistics sample("hello world");
  const Statistics other("goodbye world");
  const auto sample_hash = sample.SimHash128Signature();
  const auto other_hash = other.SimHash128Signature();
  CHECK(Statistics::SimHashDistance(sample_hash, other_hash) > 0.0);
}

TEST_CASE("Statistics includes fellow word count in frequency") {
  const Statistics sample("alpha");
  const Statistics other("alpha beta");
  CHECK(sample.Dist(other) == doctest::Approx(std::sqrt(0.5)));
}

TEST_CASE("Sample data matches expected output ordering") {
  const fs::path repo_root(MATCHTEXT_SOURCE_DIR);
  const fs::path sample_path = repo_root / "Data" / "Sample.txt";
  const fs::path repo_dir = repo_root / "Data" / "Repo";

  const std::vector<Entry> entries = ComputeSimilarities(sample_path, repo_dir);
  const ExpectedEntry expected[] = {
    {"THE ADVENTURES OF SHERLOCK HOLMES.txt"},
    {"A TALE OF TWO CITIES - A STORY OF THE FRENCH REVOLUTION.txt"},
    {"ALICE'S ADVENTURES IN WONDERLAND.txt"},
    {"THE ADVENTURES OF TOM SAWYER.txt"},
    {"The Romance of Lust.txt"},
    {"MOBY-DICK or, THE WHALE.txt"},
    {"Frankenstein; or, the Modern Prometheus.txt"},
    {"PRIDE AND PREJUDICE.txt"},
    {"BEOWULF - AN ANGLO-SAXON EPIC POEM.txt"},
    {"The Iliad of Homer.txt"},
  };

  REQUIRE(entries.size() == (sizeof(expected) / sizeof(expected[0])));
  for (size_t i = 0; i < entries.size(); i++) {
    const std::string actual_name = NormalizeFilename(entries[i].path.filename().string());
    CHECK(actual_name == expected[i].name);
  }
}
