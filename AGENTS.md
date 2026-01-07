# AGENTS

## Build and run
- Use CMake (C++17). No Visual Studio/MSBuild project files are kept in this repo.
- Configure: `cmake -S . -B build`
- Build: `cmake --build build -j`
- Tests: `ctest --test-dir build`
- Debian packages: builds emit `.deb` files to `build/packages/` (or run `cmake --build build --target deb`).

## CLI entry points
- matchtext: `./build/matchtext <Sample File> <Repository Directory> [--recursive] [--hash] [--threads N] [--safe] [--no-convert]`
  - Compares a single sample file to each candidate file in the repository directory.
  - `--recursive` (or `-r`) scans subdirectories.
  - `--hash` uses SimHash instead of TF-IDF.
  - `--threads N` overrides the worker count used for file parsing.
  - `--safe` serializes PDF extraction to avoid poppler threading issues.
  - `--no-convert` skips format-specific extractors (PDF/Office/RTF) and reads raw bytes only.
- mostsimilar: `./build/mostsimilar <Directory> [--hash] [--dedup] [--threads N] [--safe] [--no-convert]`
  - Compares every eligible file to every other file in the directory tree (O(n^2)).
  - Writes `<Directory>_mostsimilar.csv` to the current working directory (`_hash` suffix when using `--hash`).
  - CSV columns: `file`, `most_similar`, `score`, `pair_id` (reciprocal pair identifier).
  - Masks the input directory prefix in stdout/CSV with `.../`.
  - Prints a console table with line-drawing characters (max width 132) and wraps long paths.
  - Prints the full CSV path after writing.
  - `--dedup` (optional threshold, e.g., `--dedup 0.98`) moves any `most_similar` file with
    score >= threshold (default 1.0) into `<Directory>/Duplicates` after the CSV is written,
    preserving the relative subdirectory path when possible. For mutual matches above the
    threshold, only one of the pair is moved, preferring the older one based on filename
    version/date markers (v2, v.2, _v_2, _1b, revA, r2024.1, final2, build123, -revised, _new,
    date stamps like 20240318/2024-03-18/2024Q1), then modification time, then scan order.
  - `--threads N` overrides the worker count used for file parsing.
  - `--safe` serializes PDF extraction to avoid poppler threading issues.
  - `--no-convert` skips format-specific extractors (PDF/Office/RTF) and reads raw bytes only.

## Data and output
- Sample data lives in `Data/`; README output is produced from `Data/Sample.txt` against `Data/Repo`.
- `Data/RepoWithSample` adds `FAIRY TALES By The Brothers Grimm.txt` for `mostsimilar` examples.
- Results are sorted by descending similarity, normalized to [0, 1] where 1.0 means identical.
- Scores are truncated to 8 decimal places (both console and CSV output).
- mostsimilar collapses reciprocal best-match pairs into a single row (left = preferred file,
  right = duplicate candidate under threshold 0.00000001).

## Scoring and text processing
- Tokenization treats Unicode whitespace/punctuation as delimiters; UTF-8 sequences are preserved.
- Case folding uses the current locale; non-ASCII letters can fold differently per environment.
- Stop words (English/French/Spanish) are removed before counting tokens; list lives in `MatchText/Statistics.cpp`.
- Default similarity: pairwise TF-IDF cosine similarity (IDF computed from only the two documents being compared).
- SimHash (`--hash`): 128-bit signature using FNV-1a hashes with two seeds; similarity is
  `1 - (HammingDistance / 128)`, so scores are discrete multiples of 1/128.

## File ingestion
- A whitelist of extensions controls candidates: update `IsAllowedTextFile` in `MatchText/Utils.cpp`.
- PDF uses poppler-cpp when available.
- Poppler error output is suppressed so non-fatal font/linearization warnings do not spam logs.
- DOCX/DOCM/DOTX/DOTM, PPTX/PPTM/POTX/POTM/PPSX/PPSM, XLSX/XLSM/XLTX/XLTM, and ODT/ODS/ODP
  use built-in ZIP/XML parsing.
- RTF uses a lightweight extractor; legacy DOC/DOT/XLS/XLT/PPT/PPS/POT use heuristic string extraction.
- Failures fall back to raw bytes (best-effort extraction).

## Concurrency and performance
- matchtext uses a blocking queue; mostsimilar uses an index-based worker pool to parse files in parallel.
- Thread count defaults to `max(1, hardware_concurrency())`; mostsimilar includes it in progress lines and both tools
  report it at the end when `--threads` is not supplied.
- O(n^2) comparisons dominate runtime for large trees; skip or chunk inputs when testing.
- `--dedup` skips scanning any existing `Duplicates/` subtree under the target directory.

## Key files to orient
- `MatchText/MatchText.cpp`: CLI for sample-vs-repo comparisons and output formatting.
- `MatchText/MostSimilar.cpp`: CLI for all-pairs comparisons, CSV/table output, progress updates.
- `MatchText/Statistics.cpp`: tokenization, stop words, TF-IDF, SimHash.
- `MatchText/Utils.cpp`: file filtering and text extraction helpers.
