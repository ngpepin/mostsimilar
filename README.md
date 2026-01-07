# mostsimilar & matchtext
Directory-scale and sample-based text similarity tools from the Linux command line, built on top of https://github.com/srogatch/TextMatching C++ .NET repo.

`mostsimilar` provides directory-wide best matches via pairwise comparisons.

`matchtext` performs single sample vs repository match scoring.

## Overview
- Both tools process a whitelist of extensions (see `MatchText/Utils.cpp`).
- Common document formats are parsed on a best-effort basis: PDF (if poppler-cpp is
  available), DOCX/DOCM/DOTX/DOTM, PPTX/PPTM/POTX/POTM/PPSX/PPSM,
  XLSX/XLSM/XLTX/XLTM, ODT/ODS/ODP (via built-in ZIP/XML parsing), RTF, and
  legacy DOC/DOT/XLS/XLT/PPT/PPS/POT (heuristic string extraction). If extraction
  fails or a library is missing, raw bytes are used instead.
- Default scoring uses pairwise TF-IDF cosine similarity with English/French/Spanish stop words removed.
- `--hash` switches to SimHash similarity (128-bit signatures).
- `--threads N` overrides the worker thread count for file parsing.
- `--safe` serializes PDF extraction if poppler is unstable with threads.
- `--no-convert` skips format-specific extractors and reads raw bytes only.

## Build (Ubuntu)
```
cmake -S . -B build
cmake --build build -j
```

## Packaging (Ubuntu)
Builds `.deb` packages automatically when you build (output under `build/packages/`).
You can also run:
```
cmake --build build --target deb
```

## mostsimilar
```
./build/mostsimilar <Directory> [--hash] [--dedup] [--threads N] [--safe] [--no-convert]
```

Behavior
- Recursively scans the directory, prints a console table, and writes a CSV to the current working directory.
- CSV name: `<Directory>_mostsimilar.csv` (or `<Directory>_mostsimilar_hash.csv` with `--hash`).
- CSV columns: `file`, `most_similar`, `score` (normalized to [0, 1]), `pair_id`.
- Reciprocal best matches are shown once in the table/CSV: the left column is the preferred file and the right
  column is the duplicate candidate under a very low threshold (0.00000001).
- `--dedup` (optionally `--dedup 0.98`) moves any `most_similar` file with score >= threshold into
  `<Directory>/Duplicates` after the CSV is written.
- For mutual matches above the threshold, only one of the pair is moved, preferring the older file based on
  filename version/date markers (v2, v.2, _v_2, revA, r2024.1, final2, build123, date stamps like
  20240318/2024-03-18/2024Q1), then modification time, then scan order.
- Progress lines include the active thread count during file loading and match computation.
- Paths in the CSV/output are masked by replacing the parent directory of the input directory with `.../`.

Test dataset: `Data/RepoWithSample` contains the original repo plus
`FAIRY TALES By The Brothers Grimm.txt` so its row can be compared to the
`matchtext` output.

Example output (`./build/mostsimilar Data/RepoWithSample`):

Rows are sorted by score (descending).

| File | Most similar | Score |
| --- | --- | --- |
| `.../A TALE OF TWO CITIES - A STORY OF THE FRENCH REVOLUTION.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.74802288` |
| `.../FAIRY TALES By The Brothers Grimm.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.66202666` |
| `.../PRIDE AND PREJUDICE.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.61328044` |
| `.../ALICE’S ADVENTURES IN WONDERLAND.txt` | `.../FAIRY TALES By The Brothers Grimm.txt` | `0.60094929` |
| `.../Frankenstein; or, the Modern Prometheus.txt` | `.../A TALE OF TWO CITIES - A STORY OF THE FRENCH REVOLUTION.txt` | `0.59321520` |
| `.../The Romance of Lust.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.59126618` |
| `.../MOBY-DICK or, THE WHALE.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.59032847` |
| `.../THE ADVENTURES OF TOM SAWYER.txt` | `.../FAIRY TALES By The Brothers Grimm.txt` | `0.57515195` |
| `.../The Iliad of Homer.txt` | `.../Frankenstein; or, the Modern Prometheus.txt` | `0.39058293` |
| `.../BEOWULF - AN ANGLO-SAXON EPIC POEM.txt` | `.../Frankenstein; or, the Modern Prometheus.txt` | `0.35827436` |

Example CSV rows (note `pair_id` as the last column):
```csv
file,most_similar,score,pair_id
.../A TALE OF TWO CITIES - A STORY OF THE FRENCH REVOLUTION.txt,.../THE ADVENTURES OF SHERLOCK HOLMES.txt,0.74802288,4
.../FAIRY TALES By The Brothers Grimm.txt,.../THE ADVENTURES OF SHERLOCK HOLMES.txt,0.66202666,5
.../PRIDE AND PREJUDICE.txt,.../THE ADVENTURES OF SHERLOCK HOLMES.txt,0.61328044,2
```

Example output (`./build/mostsimilar Data/RepoWithSample --hash`):
| File | Most similar | Score |
| --- | --- | --- |
| `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `.../MOBY-DICK or, THE WHALE.txt` | `0.82812500` |
| `.../The Romance of Lust.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.82031250` |
| `.../A TALE OF TWO CITIES - A STORY OF THE FRENCH REVOLUTION.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.82031250` |
| `.../PRIDE AND PREJUDICE.txt` | `.../A TALE OF TWO CITIES - A STORY OF THE FRENCH REVOLUTION.txt` | `0.78906250` |
| `.../Frankenstein; or, the Modern Prometheus.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.78906250` |
| `.../FAIRY TALES By The Brothers Grimm.txt` | `.../THE ADVENTURES OF SHERLOCK HOLMES.txt` | `0.78125000` |
| `.../ALICE’S ADVENTURES IN WONDERLAND.txt` | `.../FAIRY TALES By The Brothers Grimm.txt` | `0.77343750` |
| `.../THE ADVENTURES OF TOM SAWYER.txt` | `.../MOBY-DICK or, THE WHALE.txt` | `0.73437500` |
| `.../The Iliad of Homer.txt` | `.../MOBY-DICK or, THE WHALE.txt` | `0.72656250` |
| `.../BEOWULF - AN ANGLO-SAXON EPIC POEM.txt` | `.../The Iliad of Homer.txt` | `0.67968750` |

Scores are normalized to [0, 1] for both methods, where 1.0 means identical. TF-IDF cosine
similarity down-weights common words, while SimHash focuses on token presence rather than
frequency, which makes its scores discrete (multiples of 1/128) and creates more ties. That
tends to reshuffle the ordering compared to TF-IDF similarity, and can elevate different
nearest neighbors even when the corpus is the same. Stop-word removal also shifts the
ordering by reducing common-word noise.

## matchtext
Windows:
```
MatchText.exe <Sample File> <Repository Directory> [--recursive] [--hash] [--threads N] [--safe] [--no-convert]
```

Ubuntu:
```
./build/matchtext <Sample File> <Repository Directory> [--recursive] [--hash] [--threads N] [--safe] [--no-convert]
```

Below is the output for sample "FAIRY TALES By The Brothers Grimm" against a repository of 10 other books
using TF-IDF cosine similarity:
```
0.66242753 Data/Repo/THE ADVENTURES OF SHERLOCK HOLMES.txt
0.64461773 Data/Repo/A TALE OF TWO CITIES - A STORY OF THE FRENCH REVOLUTION.txt
0.59994812 Data/Repo/ALICE’S ADVENTURES IN WONDERLAND.txt
0.57425747 Data/Repo/THE ADVENTURES OF TOM SAWYER.txt
0.52540822 Data/Repo/The Romance of Lust.txt
0.51076155 Data/Repo/MOBY-DICK or, THE WHALE.txt
0.49168939 Data/Repo/Frankenstein; or, the Modern Prometheus.txt
0.48361284 Data/Repo/PRIDE AND PREJUDICE.txt
0.32694161 Data/Repo/BEOWULF - AN ANGLO-SAXON EPIC POEM.txt
0.30531994 Data/Repo/The Iliad of Homer.txt
```

SimHash similarity output (`--hash`) for the same inputs:
```
0.78125000 Data/Repo/THE ADVENTURES OF SHERLOCK HOLMES.txt
0.77343750 Data/Repo/ALICE’S ADVENTURES IN WONDERLAND.txt
0.76562500 Data/Repo/MOBY-DICK or, THE WHALE.txt
0.74218750 Data/Repo/The Romance of Lust.txt
0.73437500 Data/Repo/PRIDE AND PREJUDICE.txt
0.72656250 Data/Repo/A TALE OF TWO CITIES - A STORY OF THE FRENCH REVOLUTION.txt
0.71093750 Data/Repo/Frankenstein; or, the Modern Prometheus.txt
0.67968750 Data/Repo/The Iliad of Homer.txt
0.67187500 Data/Repo/THE ADVENTURES OF TOM SAWYER.txt
0.57812500 Data/Repo/BEOWULF - AN ANGLO-SAXON EPIC POEM.txt
```

Here the whole repository is listed, starting from most similar texts down to least similar.

# Appendix: Scoring Methods

## Tokenization and stop words
Files are decoded as UTF-8 and tokenized by splitting on whitespace, punctuation, and
control characters. Tokens are lowercased using the current locale. Common English, French,
and Spanish stop words are removed before scoring to reduce noise from very frequent terms.

## TF-IDF cosine similarity (default)
For the default method, each document is represented as a TF-IDF vector and compared with
cosine similarity. The IDF weight is computed from just the two documents being compared
(no corpus-wide statistics are used).

Definitions (for a token `t`):
- `tf(t, doc) = count(t, doc) / total_tokens(doc)`
- `idf(t) = log((T + 1) / (count(t, docA) + count(t, docB) + 1)) + 1`
  where `T` is the total token count across both documents
- `weight(t, doc) = tf(t, doc) * idf(t)`

Vector form (for the combined vocabulary `V = tokens(docA) ∪ tokens(docB)`):
$$
\\mathrm{sim}(A, B) = \\frac{\\sum_{t \\in V} w(t, A)\\, w(t, B)}{\\sqrt{\\sum_{t \\in V} w(t, A)^2}\\, \\sqrt{\\sum_{t \\in V} w(t, B)^2}}
$$

This yields a cosine similarity in `[0, 1]` where higher values mean closer matches.
Because IDF is computed from the pair, rare tokens within that pair are emphasized while
shared high-frequency tokens are down-weighted.

Example:
Document A: "the cat sat on the mat"
Document B: "the cat sat on the rug"
After stop-word removal: A = "cat sat mat", B = "cat sat rug"
The shared tokens ("cat", "sat") are down-weighted by IDF, so the similarity is lower than
1.0 (about 0.57 with the current formula).

Worked numeric example (natural log, values rounded):
- Token counts: A = {cat:1, sat:1, mat:1}, B = {cat:1, sat:1, rug:1}
- Total tokens: |A| = |B| = 3, T = 6
- IDF values:
  - cat/sat: idf = ln((T+1)/(2+1)) + 1 = ln(7/3) + 1 ≈ 1.8473
  - mat/rug: idf = ln((T+1)/(1+1)) + 1 = ln(7/2) + 1 ≈ 2.2528
- Weights (tf = 1/3):
  - w(cat,A) = w(sat,A) = 0.6158, w(mat,A) = 0.7509
  - w(cat,B) = w(sat,B) = 0.6158, w(rug,B) = 0.7509
- Dot product: 0.6158^2 + 0.6158^2 = 0.7584
- Norms: ||A|| = ||B|| = sqrt(0.6158^2 + 0.6158^2 + 0.7509^2) ≈ 1.1507
- Cosine similarity: 0.7584 / (1.1507 * 1.1507) ≈ 0.573

## SimHash similarity (--hash)
SimHash turns each document into a 128-bit signature. Each token is hashed (FNV-1a),
weighted by its count, and accumulated into 128 signed totals; the sign of each total
decides the final bit. Similarity is computed as:
`1 - (HammingDistance(sigA, sigB) / 128)`.
This yields discrete steps (multiples of 1/128), so ties are more common.

More explicitly, for each token `t` with count `c(t)` and 128-bit hash `h(t)`:
$$
s_i = \\sum_t c(t) \\cdot \\begin{cases}
1 & \\text{if bit } i \\text{ of } h(t) \\text{ is } 1 \\\\
-1 & \\text{if bit } i \\text{ of } h(t) \\text{ is } 0
\\end{cases}
$$
$$
\\mathrm{sig}_i = \\begin{cases}
1 & \\text{if } s_i \\ge 0 \\\\
0 & \\text{if } s_i < 0
\\end{cases}
$$
$$
\\mathrm{sim}(A, B) = 1 - \\frac{\\mathrm{HammingDistance}(\\mathrm{sig}_A, \\mathrm{sig}_B)}{128}
$$

SimHash reflects token presence patterns rather than full frequency distributions, which is
why its scores are coarse and often tied.

## Score orientation
Both methods report similarity in `[0, 1]`, where `1.0` means identical content according to
the chosen method. No dataset-wide normalization is applied.
