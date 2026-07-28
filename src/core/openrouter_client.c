/*
 * Implementation of the OpenRouter HTTP client.
 * See include/openrouter_client.h for the module's role (spec 5.2.4, Stage 4).
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "openrouter_client.h"

#include <cJSON.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPENROUTER_CHAT_COMPLETIONS_URL "https://openrouter.ai/api/v1/chat/completions"

/* Growable buffer the write callback accumulates the response body into
 * as libcurl delivers it, possibly across multiple calls. */
typedef struct {
    char *data;
    size_t size;
} OpenRouterResponse;

/* Must match libcurl's required CURLOPT_WRITEFUNCTION signature exactly.
 * `size * nmemb` is the number of bytes in this chunk; libcurl may call
 * this multiple times per request as data arrives. Returning anything
 * other than the full chunk size signals an error and aborts the
 * transfer -- that's what the realloc-failure path relies on. */
static size_t openrouter_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t chunk_size = size * nmemb;
    OpenRouterResponse *response = (OpenRouterResponse *)userdata;

    /* realloc(NULL, n) behaves like malloc(n), so this works uniformly
     * whether this is the first chunk or a later one. */
    char *new_data = realloc(response->data, response->size + chunk_size + 1);
    if (new_data == NULL) {
        return 0;
    }
    response->data = new_data;

    memcpy(response->data + response->size, ptr, chunk_size);
    response->size += chunk_size;
    response->data[response->size] = '\0';

    return chunk_size;
}

int openrouter_client_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void openrouter_client_cleanup(void) {
    curl_global_cleanup();
}

char *openrouter_post(const char *url, const char *api_key, const char *json_body) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "openrouter_post: curl_easy_init failed\n");
        return NULL;
    }

    OpenRouterResponse response = {NULL, 0};

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, openrouter_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode result = curl_easy_perform(curl);

    char *return_value = NULL;
    if (result != CURLE_OK) {
        fprintf(stderr, "openrouter_post: request failed: %s\n", curl_easy_strerror(result));
        free(response.data);
    } else {
        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        if (status_code < 200 || status_code >= 300) {
            fprintf(stderr, "openrouter_post: HTTP %ld: %s\n", status_code,
                    response.data != NULL ? response.data : "(empty response)");
            free(response.data);
        } else if (response.data == NULL) {
            /* A 2xx with a genuinely empty body is still success -- don't
             * let that collapse into the NULL-means-failure contract. */
            return_value = strdup("");
        } else {
            return_value = response.data;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return return_value;
}

char *openrouter_chat_completion(const char *model, const char *user_message) {
    const char *api_key = getenv("OPENROUTER_API_KEY");
    if (api_key == NULL || api_key[0] == '\0') {
        fprintf(stderr, "openrouter_chat_completion: OPENROUTER_API_KEY not set\n");
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", model);

    cJSON *message = cJSON_CreateObject();
    if (message == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", user_message);

    cJSON *messages = cJSON_CreateArray();
    if (messages == NULL) {
        cJSON_Delete(message);
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToArray(messages, message);
    cJSON_AddItemToObject(root, "messages", messages);

    char *request_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (request_body == NULL) {
        return NULL;
    }

    char *response_body = openrouter_post(OPENROUTER_CHAT_COMPLETIONS_URL, api_key, request_body);
    free(request_body);
    if (response_body == NULL) {
        return NULL;
    }

    cJSON *response_json = cJSON_Parse(response_body);
    free(response_body);
    if (response_json == NULL) {
        fprintf(stderr, "openrouter_chat_completion: failed to parse response JSON\n");
        return NULL;
    }

    cJSON *choices = cJSON_GetObjectItem(response_json, "choices");
    cJSON *first_choice = (choices != NULL) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message_obj = (first_choice != NULL) ? cJSON_GetObjectItem(first_choice, "message") : NULL;
    cJSON *content = (message_obj != NULL) ? cJSON_GetObjectItem(message_obj, "content") : NULL;

    if (content == NULL || !cJSON_IsString(content)) {
        fprintf(stderr, "openrouter_chat_completion: unexpected response shape\n");
        cJSON_Delete(response_json);
        return NULL;
    }

    char *reply = strdup(content->valuestring);
    cJSON_Delete(response_json);
    return reply;
}
