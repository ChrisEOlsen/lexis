/*
 * libcurl + cJSON wrapper for OpenRouter API calls (spec 5.2.4, 5.2.7, 6,
 * build order Stage 4). Single client used by both query formulation
 * (small model) and generation (large model) — OpenRouter decouples the
 * pipeline from any single model provider.
 */

#ifndef LEXIS_OPENROUTER_CLIENT_H
#define LEXIS_OPENROUTER_CLIENT_H

/* Must be called exactly once, before any other function in this module
 * and before any thread that might also call it is started -- libcurl's
 * global init is documented as not thread-safe. Returns 0 on success, -1
 * on failure. */
int openrouter_client_init(void);

/* Must be called exactly once, after every other call into this module
 * (from every thread) has finished and no more will be made. */
void openrouter_client_cleanup(void);

/* Sends `json_body` as an authenticated POST request to `url` with
 * Authorization: Bearer `api_key` and Content-Type: application/json
 * headers set. Returns the raw response body as a heap string (caller
 * must free()) on a 2xx HTTP status, or NULL on a network failure,
 * non-2xx status, or allocation failure. */
char *openrouter_post(const char *url, const char *api_key, const char *json_body);

/* Sends a single-message chat completion request to `model` via
 * OpenRouter (one prompt in, one reply out -- not a multi-turn
 * conversation), reading the API key from the OPENROUTER_API_KEY
 * environment variable. Returns just the assistant's reply text (caller
 * must free()), or NULL if the API key is missing, the request fails, or
 * the response can't be parsed into the expected shape. */
char *openrouter_chat_completion(const char *model, const char *user_message);

#endif /* LEXIS_OPENROUTER_CLIENT_H */
