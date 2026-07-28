# WordNet Loader — How It Fits Together

## The problem this solves

BM25 matches exact words. If a document says "high blood pressure" and someone searches "hypertension," plain keyword search finds nothing — even though they mean the same thing. This module closes that gap: given a word, it hands back its synonyms (same meaning), hypernyms (broader terms), and hyponyms (narrower terms), so a search for one can be expanded to also catch the others.

## Hypernym vs. hyponym

Broader vs. narrower, in an "is-a" hierarchy:

```
       animal          ← hypernym of "dog" (broader -- "a dog IS-A animal")
         │
        dog
         │
       poodle          ← hyponym of "dog" (narrower -- "a poodle IS-A dog")
```

*Hyper-* means "above" (hyperactive = above-normal activity) → a hypernym sits **above** the word, more general. *Hypo-* means "under" (hypothermia = below-normal temperature) → a hyponym sits **below** the word, more specific. Synonyms are neither above nor below — they're words in the *same* spot in the hierarchy, meaning the same thing.

## Why there are so many data structures

WordNet doesn't ship as a clean "word → related words" table. It ships as two plain-text files per part of speech, and — for compactness, since this format predates modern databases by decades — relationships between words are stored as **byte offsets**, not the actual words. Saying "dog's hypernym is at position 1234567 in the file" is far cheaper to store than repeating the word "animal" every time it's referenced. But an offset is meaningless by itself; you have to go look up what's actually sitting at that position to find out it says "animal."

That's the entire reason this module has so many pieces — each one is a stepping stone in a single pipeline, and most of them are discarded the moment their one job is done:

```
data.<pos> / index.<pos>          raw text files, offsets everywhere,
   (on disk)                       nothing resolved to real words yet
        │
        ▼
WordNetSynset                     one parsed line: "this synset's words
WordNetIndexEntry                  are X, Y" / "this word belongs to
                                    synsets at these offsets" -- still
                                    just numbers, not cross-referenced
        │
        ▼
WordNetSynsetIndex                 sorted, fast-searchable collections --
WordNetWordIndex                   exist ONLY so "what's at offset N?"
                                    and "what synsets does word W belong
                                    to?" can be answered quickly while
                                    resolving. Thrown away right after.
        │
        ▼
   RESOLUTION                      walk every offset, look up what's
                                    actually there, collect the real words
        │
        ▼
WordNetTable                       the only thing that survives. word →
WordNetLookupResult                 {synonyms, hypernyms, hyponyms} as
                                    actual strings. This is what the rest
                                    of the program ever touches.
```

So: it isn't that this module needs five permanent data structures — it needs **one** (`WordNetTable`). The other four exist purely to get from "a file full of numbers" to that one table, and disappear once loading finishes. If WordNet shipped as a simple word→word text file to begin with, none of the scaffolding above the "RESOLUTION" line would need to exist at all.

One thing intentionally *not* preserved: which specific *sense* of a word each result came from. "Object" the physical thing and "object" the verb (to protest) get merged into one flat, deduplicated result — because picking the right sense for a given query needs the query's context, which is a job for the small-model disambiguation step described in the spec (5.2.3), not this module.

## Function reference, grouped by pipeline stage

**Parsing one line** (raw text → struct)
- `wordnet_parse_data_line` — one `data.<pos>` line → a `WordNetSynset`
- `wordnet_parse_index_line` — one `index.<pos>` line → a `WordNetIndexEntry`
- `wordnet_synset_free` / `wordnet_index_entry_free` — release one parsed struct

**Loading a whole file** (many lines → a searchable collection)
- `wordnet_load_data_file` — every synset in one `data.<pos>` file, sorted by offset
- `wordnet_load_index_file` — every word in one `index.<pos>` file, sorted alphabetically
- `wordnet_synset_index_find` / `wordnet_word_index_find` — binary search by offset / by word
- `wordnet_synset_index_free` / `wordnet_word_index_free` — release a loaded file's data

**The final table** (resolved answers, what the rest of the program uses)
- `wordnet_table_create` / `wordnet_table_free` — allocate/release the table itself
- `wordnet_lookup` — the actual entry point: word in, synonyms/hypernyms/hyponyms out
- `wordnet_table_load` — the top-level call: loads all four parts of speech, resolves every word, discards all the scaffolding above, returns the one table that matters
