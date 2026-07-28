/*
 * Unix socket client to the Python sidecar (spec 5.2.2, 6, build order
 * Stage 6). Sends the user query for stopword removal/POS tagging and
 * synonym expansion, and optionally for cross-encoder reranking. Unix
 * sockets keep sidecar IPC low-latency on the same host.
 */
