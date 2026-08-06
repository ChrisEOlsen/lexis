// Implementation of the C-callable Jinja chat-template bridge.
// See include/jinja_chat_template.h for the module's role.
#include "jinja_chat_template.h"

#include "chat-template.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>

char *jinja_render_chat_template(const char *jinja_template_src, const char *bos_token, const char *eos_token,
                                  const LocalLlmTurn *turns, size_t count, int add_generation_prompt,
                                  int enable_thinking, int has_enable_thinking_override) {
    // Never let a C++ exception cross back into the C callers on the
    // other side of this extern "C" boundary -- minja throws on a
    // malformed/unsupported template or a rendering error, and letting
    // that propagate into local_llm_client.c would be undefined
    // behavior. Matches this bridge's "NULL on failure" contract.
    try {
        minja::chat_template tmpl(jinja_template_src, bos_token != nullptr ? bos_token : "",
                                   eos_token != nullptr ? eos_token : "");

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

        std::string rendered = tmpl.apply(inputs, opts);

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
