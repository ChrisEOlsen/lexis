// C-callable Jinja chat-template bridge; see jinja_chat_template.h.
#include "jinja_chat_template.h"

#include "chat-template.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

namespace {
// Cache parsed template by source (~11s parse dwarfs inference); one model per process so source rarely changes.
// Not thread-safe; caller already serializes LLM calls (see local_llm_client.h).
std::string g_cached_source;
std::unique_ptr<minja::chat_template> g_cached_template;
} // namespace

char *jinja_render_chat_template(const char *jinja_template_src, const char *bos_token, const char *eos_token,
                                  const LocalLlmTurn *turns, size_t count, int add_generation_prompt,
                                  int enable_thinking, int has_enable_thinking_override) {
    // Never let C++ exceptions cross the extern "C" boundary (UB); NULL on failure.
    try {
        if (g_cached_template == nullptr || g_cached_source != jinja_template_src) {
            g_cached_template = std::make_unique<minja::chat_template>(
                jinja_template_src, bos_token != nullptr ? bos_token : "", eos_token != nullptr ? eos_token : "");
            g_cached_source = jinja_template_src;
        }

        nlohmann::ordered_json messages = nlohmann::ordered_json::array();
        for (size_t i = 0; i < count; i++) {
            nlohmann::ordered_json message;
            message["role"] = turns[i].role;
            message["content"] = turns[i].content;
            messages.push_back(message);
        }

        minja::chat_template_inputs inputs;
        inputs.messages = messages;
        inputs.add_generation_prompt = add_generation_prompt != 0;
        if (has_enable_thinking_override) {
            inputs.extra_context["enable_thinking"] = enable_thinking != 0;
        }

        // Skip template BOS: tokenizer already prepends one with add_special=true; else doubled.
        minja::chat_template_options opts;
        opts.use_bos_token = false;

        std::string rendered = g_cached_template->apply(inputs, opts);

        char *result = static_cast<char *>(std::malloc(rendered.size() + 1));
        if (result == nullptr) {
            return nullptr;
        }
        std::memcpy(result, rendered.c_str(), rendered.size() + 1);
        return result;
    } catch (const std::exception &) {
        return nullptr;
    }
}
