
## CUDA
Given the current design, CUDA is unlikely to be a game‑changer without redesigning the scoring pipeline.

- ```mostsimilar``` spends most compute in the O(n^2) pairwise loop in ```MostSimilar.cpp```, but each comparison uses ```std::unordered_map``` lookups and per‑pair IDF in ```Statistics.cpp```; that’s branchy, irregular memory access that GPUs handle poorly without a major data layout change.
- If you keep the algorithm as‑is and just “offload,” expect small end‑to‑end gains (roughly ~1.2–3x) because parsing/extraction and tokenization stay on CPU and dominate for many corpora.
- If you restructure to GPU‑friendly vectors (e.g., global IDF + sparse/dense TF‑IDF matrices, or hashed vectorization), the comparison phase could be 5–20x faster on large corpora, but end‑to‑end still tends to land ~2–6x because file I/O/extraction remains.
- For ```matchtext```, the compute is O(n) not O(n^2), and parsing tends to dominate; GPU speedups are likely marginal (often ~1.1–2x).
- SimHash comparisons are already just XOR+popcount; GPU won’t help much there.
