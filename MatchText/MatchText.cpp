#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

#include "Statistics.h"
#include "Utils.h"
#include "BlockingQueue.h"
namespace fs = std::filesystem;

struct RepoEntry {
  std::string _filePath;
  double _score;

  RepoEntry(const std::string& filePath, const double score) : _filePath(filePath), _score(score) { }
};

int main(int argc, char *argv[])
{
  if (argc < 3) {
    fprintf(stderr,
            "Match a sample file against a repository and list results by similarity.\n"
            "Usage: %s <Sample File> <Repository Directory> [--recursive] [--hash] [--threads N] [--safe] [--no-convert] [--verbose]\n",
            argv[0]);
    return 1;
  }
  bool recursive = false;
  bool use_hash = false;
  bool threads_specified = false;
  unsigned int requested_threads = 0;
  bool safe_mode = false;
  bool no_convert = false;
  bool verbose = false;
  for (int i = 3; i < argc; i++) {
    if (strcmp(argv[i], "--recursive") == 0 || strcmp(argv[i], "-r") == 0) {
      recursive = true;
      continue;
    }
    if (strcmp(argv[i], "--hash") == 0) {
      use_hash = true;
      continue;
    }
    if (strcmp(argv[i], "--threads") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--threads requires a value.\n");
        return 1;
      }
      char* end = nullptr;
      const unsigned long value = std::strtoul(argv[i + 1], &end, 10);
      if (end == argv[i + 1] || *end != '\0' || value == 0 ||
          value > std::numeric_limits<unsigned int>::max()) {
        fprintf(stderr, "Invalid thread count: %s\n", argv[i + 1]);
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
      fprintf(stderr,
              "Match a sample file against a repository and list results by similarity.\n"
              "Usage: %s <Sample File> <Repository Directory> [--recursive] [--hash] [--threads N] [--safe] [--no-convert] [--verbose]\n"
              "  --hash  Use SimHash to compare files instead of TF-IDF cosine similarity.\n"
              "  --threads N  Override the worker thread count used for file parsing.\n"
              "  --safe  Serialize PDF extraction to avoid poppler threading issues.\n"
              "  --no-convert  Skip format-specific extractors and read raw bytes only.\n"
              "  --verbose  Print files as they are read and comparisons as they are scored.\n"
              "Scores are normalized to [0, 1], where 1.0 means identical.\n",
              argv[0]);
      return 0;
    }
    fprintf(stderr, "Unknown option: %s\n", argv[i]);
    return 1;
  }

  // Load and tokenize the sample document once.
  const std::string sample_path = argv[1];
  Statistics sampleStat;
  if (verbose) {
    fprintf(stderr, "Reading file: %s\n", sample_path.c_str());
  }
  if (!ReadFileToStatistics(sample_path, &sampleStat, safe_mode, no_convert)) {
    return 2;
  }
  if (sampleStat.IsEmpty()) {
    fprintf(stderr, "Sample file must be non-empty: this corner case is not supported.\n");
    return 2;
  }
  Statistics::SimHash128 sample_hash{};
  if (use_hash) {
    sample_hash = sampleStat.SimHash128Signature();
  }
  struct RepoDoc {
    std::string path;
    Statistics stats;
  };
  std::vector<RepoDoc> repo_docs;
  BlockingQueue<std::string> bq;
  std::vector<std::thread> workers;
  std::vector<std::vector<RepoDoc>> workerDocs;
  const unsigned int max_threads = std::max(1u, std::thread::hardware_concurrency());
  const unsigned int workerCount = threads_specified ? requested_threads : max_threads;
  workerDocs.resize(workerCount);
  // Parse repository files in parallel and collect token stats.
  for (unsigned int i = 0; i < workerCount; i++) {
    workers.push_back(std::thread([&, i] {
      std::string repoFilePath;
      while (bq.Pop(repoFilePath)) {
        Statistics repoStat;
        if (verbose) {
          fprintf(stderr, "Reading file: %s\n", repoFilePath.c_str());
        }
        if (!ReadFileToStatistics(repoFilePath, &repoStat, safe_mode, no_convert)) {
          continue;
        }
        if (repoStat.IsEmpty()) {
          fprintf(stderr, "Skipping empty file %s\n", repoFilePath.c_str());
          continue;
        }
        workerDocs[i].push_back({repoFilePath, std::move(repoStat)});
      }
    }));
  }
  const fs::path repoRoot(argv[2]);
  std::error_code ec;
  const fs::directory_options options = fs::directory_options::skip_permission_denied;
  // Enumerate repository files and feed the worker queue.
  if (recursive) {
    fs::recursive_directory_iterator it(repoRoot, options, ec);
    if (ec) {
      fprintf(stderr, "Cannot open repository directory: %s\n", ec.message().c_str());
      return 2;
    }
    for (fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
      if (ec) {
        fprintf(stderr, "Skipping path due to error: %s\n", ec.message().c_str());
        ec.clear();
        continue;
      }
      if (!it->is_regular_file(ec) || ec) {
        ec.clear();
        continue;
      }
      const std::string repoFilePath = it->path().string();
      if (!IsAllowedTextFile(repoFilePath)) {
        continue;
      }
      bq.Push(repoFilePath);
    }
  }
  else {
    fs::directory_iterator it(repoRoot, options, ec);
    if (ec) {
      fprintf(stderr, "Cannot open repository directory: %s\n", ec.message().c_str());
      return 2;
    }
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
      if (ec) {
        fprintf(stderr, "Skipping path due to error: %s\n", ec.message().c_str());
        ec.clear();
        continue;
      }
      if (!it->is_regular_file(ec) || ec) {
        ec.clear();
        continue;
      }
      const std::string repoFilePath = it->path().string();
      if (!IsAllowedTextFile(repoFilePath)) {
        continue;
      }
      bq.Push(repoFilePath);
    }
  }
  bq.RequestShutdown();
  for (size_t i = 0; i < workers.size(); i++) {
    workers[i].join();
  }
  size_t totalEntries = 0;
  for (const auto& workerDoc : workerDocs) {
    totalEntries += workerDoc.size();
  }
  repo_docs.reserve(totalEntries);
  for (auto& workerDoc : workerDocs) {
    for (auto& doc : workerDoc) {
      repo_docs.emplace_back(std::move(doc));
    }
  }

  // Compute similarity scores for each repository document.
  std::vector<RepoEntry> entries;
  entries.reserve(repo_docs.size());
  for (const auto& doc : repo_docs) {
    if (verbose) {
      fprintf(stderr, "Comparing: %s <> %s\n", sample_path.c_str(), doc.path.c_str());
    }
    const double score = use_hash
      ? Statistics::SimHashSimilarity(sample_hash, doc.stats.SimHash128Signature())
      : Statistics::TfIdfCosineSimilarity(sampleStat, doc.stats);
    entries.emplace_back(doc.path, score);
  }
  // Sort by descending similarity with a stable tie-break.
  std::sort(entries.begin(), entries.end(), [](const RepoEntry& left, const RepoEntry& right) {
    if (left._score != right._score) {
      return left._score > right._score;
    }
    return left._filePath < right._filePath;
  });
  for (size_t i = 0; i < entries.size(); i++) {
    fprintf(stdout, "%.8f %s\n", entries[i]._score, entries[i]._filePath.c_str());
  }
  if (!threads_specified) {
    fprintf(stderr, "Threads used (max): %u\n", workerCount);
  }
  return 0;
}
