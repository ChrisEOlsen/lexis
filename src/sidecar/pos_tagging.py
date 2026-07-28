"""
Stopword removal + POS tagging (spec 5.2.2) -- DEFERRED, not built.

Originally planned to run spaCy POS tagging on every query, filtering to
nouns/verbs/adjectives before synonym expansion. Deferred once synonym
expansion (src/core/wordnet.c) turned out to be a cheap, local, in-memory
hash table lookup rather than the expensive Python/NLTK call the original
design assumed -- the cost justification for pre-filtering terms before
expansion mostly evaporated once expansion itself stopped being expensive.
See LIMITATIONS.md for the full reasoning and the concrete trigger for
revisiting this (real benchmark evidence, spec section 8, that precision
suffers without it -- not a guess made in advance of measuring).
"""
