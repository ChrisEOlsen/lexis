"""
Cross-encoder reranker (spec 5.2.6, build order Stage 9, optional). Reads
each (query, passage) pair from the BM25 top-K together through a
cross-encoder model, reorders by refined relevance score, and drops
low-confidence results before the top-N reach the generation step. Adds
latency — enable for precision-sensitive use cases, disable where speed
matters more.
"""
