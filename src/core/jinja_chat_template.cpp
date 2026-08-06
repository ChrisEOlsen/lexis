// Implementation of the C-callable Jinja chat-template bridge.
// See include/jinja_chat_template.h for the module's role.
#include "jinja_chat_template.h"

#include "chat-template.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

namespace {
// Parsing Gemma 4's real template (18,810 characters of Jinja2 --
// macros, loops, dictsort) is not cheap: measured directly at ~11
// seconds per parse on the dev machine, completely dwarfing actual
// model inference for a short prompt (the router's one-word decision
// was taking ~12s total for this reason, not because of thinking or
// generation). The template source is fixed for the lifetime of
// whichever model is loaded (local_llm_client.c loads exactly one
// model process-wide), so there's no reason to re-parse it on every
// call -- cache the parsed template, keyed by its source string, and
// only reconstruct it if the source actually changes (in practice:
// only when a different model gets loaded). Not thread-safe, but
// neither is anything else in this module -- the caller (AppController)
// already guarantees only one LLM call runs at a time (see
// local_llm_client.h's own documented constraint).
std::string g_cached_source;
std::unique_ptr<minja::chat_template> g_cached_template;
} // namespace

char *jinja_render_chat_template(const char *jinja_template_src, const char *bos_token, const char *eos_token,
                                  const LocalLlmTurn *turns, size_t count, int add_generation_prompt,
                                  int enable_thinking, int has_enable_thinking_override) {
    // Never let a C++ exception cross back into the C callers on the
    // other side of this extern "C" boundary -- minja throws on a
    // malformed/unsupported template or a rendering error, and letting
    // that propagate into local_llm_client.c would be undefined
    // behavior. Matches this bridge's "NULL on failure" contract.
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

        // local_llm_chat_completion_multi() tokenizes the returned prompt
        // with add_special=true, which makes llama_tokenize() itself
        // prepend the vocab's BOS token -- if the template ALSO renders
        // its own {{ bos_token }} (as Gemma 4's does), the result is two
        // BOS tokens back to back (llama.cpp warns about exactly this:
        // "the prompt also starts with a BOS token"). Disabling it here
        // makes the tokenizer's own insertion the single source of BOS,
        // matching the plain llama_chat_apply_template() path's
        // behavior (which never renders bos_token into the string
        // itself either).
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
