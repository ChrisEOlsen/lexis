---
tags: [language]
---

# Lemmatizer

**Reduces a word to its WordNet base form** ("called" → "call", "went" → "go") so index and query vocabulary match. Runs at **both** index time and query time — the two sides must lemmatize identically, so any change here requires re-ingesting every index.

Source: `src/core/lemmatizer.c`; exception files (`*.exc`) from the committed WordNet data.

Algorithm (WordNet morphy):
1. Irregular-exception lists first (`saw` → `see`, `went` → `go`).
2. **Return the word unchanged if it is already in [[WordNet]]** — the guard real morphy always had, added 2026-08-13 after its absence caused the king→k bug (below).
3. Otherwise morphy's suffix-detachment rules, each candidate validated against the real WordNet table.

History worth remembering: without step 2, base forms that merely *look* inflected were mangled at index and query time — `king`→`k`, `ring`→`re`, `sing`→`se` (the stripped stems are all real WordNet nouns: potassium, the musical note, selenium). 1,719 corrupted postings in the 200K slice; surfaced only when gemma-4-E4B followed the corrupted candidate list more faithfully than E2B had. The fix costs one extra WordNet lookup per token: ingest dropped 13,595 → 9,062 passages/sec.

**Used by:** [[Query Formulation]], [[Ingest Primitives]]. **Depends on:** [[WordNet]].
