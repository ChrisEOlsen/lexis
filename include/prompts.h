/*
 * Every instruction this project sends to a language model, in one place,
 * with the reasoning for each.
 *
 * Why centralize. These strings are the highest-churn, least-testable part
 * of the system: nothing compiles differently when a prompt is wrong, and
 * nothing fails until a model answers oddly. Before this file, the same
 * behavioral rules had been written three different ways across
 * generation.c and corpus_summary.c, and the reasoning-skip prefill was
 * defined four separate times (three C files plus QueryWorker.cpp). The
 * failure that invites is fixing a model behavior in one prompt and
 * silently not fixing it in its sibling.
 *
 * Macros, not `const char *const`. Adjacent string literals concatenate at
 * compile time, which is what lets a prompt be composed from the shared
 * LEXIS_PROMPT_RULE_* fragments below with no runtime work and no extra
 * string_builder_append() calls at each site. A const array cannot be
 * composed this way. This also matches the existing idiom for long
 * literals in this codebase -- see LEXIS_CHAT_TABLES_SQL in pg_store.c.
 *
 * Header-only, no prompts.c: nothing here needs storage, so there is no
 * new translation unit for either build system to learn about. It is
 * included from C and from C++ (QueryWorker.cpp) -- macros are
 * language-agnostic, so no extern "C" wrapper is needed.
 *
 * Convention. A _HEAD macro is the static instruction block that opens a
 * prompt, ending exactly where the caller starts appending dynamic
 * content (retrieved passages, the question, candidate terms). The builder
 * functions keep their assembly logic, so reading one still shows the
 * prompt's shape: instruction, then context, then question.
 */

#ifndef LEXIS_PROMPTS_H
#define LEXIS_PROMPTS_H

/* -- Reasoning-skip prefill -------------------------------------------- */

/* Appended after the chat template's own assistant-turn opening so a
 * thinking model sees an already-closed, empty reasoning block as context
 * it does not need to regenerate -- continuation then goes straight to the
 * real answer. See local_llm_chat_completion_multi()'s doc comment.
 *
 * Not only about output cleanliness (that function already strips a leaked
 * <think> block): it is a real latency win, since every generated token
 * goes to the answer instead of a discarded internal monologue first.
 * Verified in testing that answer quality held up without the reasoning
 * pass on these tasks. */
#define LEXIS_PREFILL_NO_THINK "<think>\n\n</think>\n\n"

/* -- Shared behavioral rules ------------------------------------------- */

/* Composed into every prompt that answers from retrieved material. Each
 * of these existed in two or three divergent phrasings before this file;
 * the wording here is the superset, so no instruction that was previously
 * given to the model was dropped in consolidating them. */

/* The failure this prevents was observed verbatim: asked "What are the
 * following documents about?", with the corpus text already in its prompt,
 * the model replied "Please provide the documents you are referring to."
 * It followed the conversational framing of the request instead of the
 * context attached to it -- the session had opened with small talk. */
#define LEXIS_PROMPT_RULE_NO_ASK_FOR_DOCS                                    \
    "The documents have already been provided and indexed. Never ask the "   \
    "user to provide, attach or upload documents, and never say that no "    \
    "documents are available. "

/* Also observed verbatim, from a passage-grounded answer: "the minimum age
 * limit for Class E is 18. (This is found in ... chunk ...)". Passages are
 * labelled "[Source: <document>, chunk <n>]" so the model can tell them
 * apart and stay grounded, but it then quotes those labels back as if they
 * were part of the answer. Provenance is the UI's job -- it already has
 * the ids, scores and text -- so retrieval bookkeeping in the prose just
 * makes the answer read like debug output. */
#define LEXIS_PROMPT_RULE_PLAIN_PROSE                                        \
    "Write the answer as plain prose for the reader. Do not mention the "    \
    "material you were given, do not refer to \"the context\" or \"the "     \
    "summary\", do not cite source labels or chunk numbers, and do not "     \
    "describe how you know what you know -- just answer. The one "           \
    "exception is saying what the documents do not state, which the rule "   \
    "above requires when they leave the question open: say that in the "     \
    "reader's own terms (\"the documents don't give a figure for X\"), "     \
    "never as \"the context\" or \"chunk 4\". Prose is the default shape, "  \
    "but when you are laying out several separate findings, a short list "   \
    "of them is clearer and is fine. "

/* The anti-hallucination floor for every grounded answer.
 *
 * This used to be a single line -- "if the material does not contain
 * enough information to answer, say so rather than guessing" -- which
 * gave the model only two moves: a confident answer, or a refusal. Real
 * retrieved material is very often neither. It half-answers: the right
 * topic with the specific number missing, two figures that disagree, a
 * spec given for the trim level next to the one asked about. Faced with
 * that and only two moves available, a model does not refuse -- refusing
 * feels wrong when it is plainly holding relevant text -- so it picks the
 * nearest plausible value and states it flatly. The user cannot tell that
 * answer apart from one the documents actually support.
 *
 * So the rule is now a decision with three outcomes, and the middle one
 * is the point of the change: settled -> answer; partial or conflicting
 * -> report what was actually found, with its conditions attached, and
 * name what is still open; nothing relevant at all -> say it is not
 * covered. Split across two macros only so the ANSWER_FROM_SUMMARY path
 * can take the first without the passage-shaped second. */
#define LEXIS_PROMPT_RULE_GROUNDED                                           \
    "Answer only from the material provided, and work out first whether "    \
    "it actually settles the question. If it states the answer outright, "   \
    "give that answer directly and plainly. "

/* The middle outcome. "Do not choose the most likely answer and state it
 * as fact" is the load-bearing sentence: without it a model treats a
 * near-miss passage as close enough and rounds it up to certainty.
 *
 * The specific prohibitions at the end are the three ways that rounding
 * up actually shows up in this pipeline -- filling the missing half from
 * what the model knows about the subject generally, quietly picking one
 * of two conflicting figures (or splitting the difference), and carrying
 * a value across from the neighbouring case that happened to be in the
 * same passage. Each is a plausible-looking answer with nothing behind
 * it, which is exactly the failure the retrieved text was meant to
 * prevent. */
#define LEXIS_PROMPT_RULE_UNCERTAINTY                                        \
    "If it does not -- the material is partial, ambiguous, gives "           \
    "conflicting figures, or covers a case close to but not the same as "    \
    "the one asked about -- then do not choose the most likely answer and "  \
    "state it as fact. Set out instead what the material does give: the "    \
    "specific figures, conditions, names, steps or statements bearing on "   \
    "the question, each with the conditions that came with it, and say "     \
    "plainly which part of the question they leave open. Several separate "  \
    "findings are clearer as a short list. Never fill a gap from general "   \
    "knowledge, never reconcile or average values that disagree, and "       \
    "never give a figure stated for one case as though it applied to "      \
    "another. Only when nothing in the material bears on the question at "   \
    "all should you say it is not covered. "

/* Passage-path only. The retrieved set comes from BM25 -- keyword
 * scoring, not comprehension -- so a passage can rank highly on shared
 * words while being about something else entirely, and the reranker
 * reorders that set rather than filtering it. The model is never told
 * this, and the natural reading of "here is your context" is that
 * everything in it was chosen because it belongs. Saying where the
 * passages came from is what makes the rule above usable: to judge
 * whether the material settles the question, the model first has to know
 * it is allowed to conclude that a passage in front of it does not
 * count. */
#define LEXIS_PROMPT_RULE_KEYWORD_MATCHES                                    \
    "The passages below were found by matching words in the question, so "   \
    "some of them may share wording with it without being about it. "       \
    "Shared wording is not evidence that a passage answers the question: "   \
    "check that a passage really concerns what was asked before relying "    \
    "on it, and disregard the ones that do not. "

/* -- Tool routing ------------------------------------------------------ */

/* One-shot classification into SEARCH / SUMMARY / CHAT. See tool_router.h
 * for what each choice means and why SUMMARY replaced a READ tool.
 *
 * The closing rules are load-bearing and each came from an observed
 * misroute. "Refers to the documents in any way" fixes a bare fragment
 * ("The documents in the corpus.") being classified CHAT and answered with
 * "you haven't provided any documents". "Earlier small talk does not make
 * the current message conversational" fixes history contamination, where
 * corpus questions after two chatty turns were pulled into CHAT. The
 * follow-up rule fixes the reverse of that second one: an earlier version
 * said simply "judge the latest message on its own", which made an
 * elliptical continuation ("Is that all? What about the other buttons?")
 * look like filler, since judged alone it names no subject at all.
 *
 * Verified against the live model, 8/8 intended routes with no history and
 * 3/3 with chatty history ahead of the question. Reword with care, and
 * re-run that check if you do. */
#define LEXIS_PROMPT_TOOL_ROUTER_HEAD                                          \
    "A collection of documents has already been provided and indexed. Choose " \
    "exactly one tool to handle the user's message. Respond with ONLY one "    \
    "word: SEARCH, SUMMARY, or CHAT.\n\n"                                      \
    "- SEARCH: asks for a specific fact, number, definition or detail that "   \
    "could be answered by finding one relevant passage (e.g. \"what is the "   \
    "minimum age for X?\", \"how many pounds is the weight limit for Y?\", "   \
    "\"what does it say about Z?\"). A request for tips, hints, advice, "      \
    "recommendations or instructions about a named topic is also SEARCH -- "   \
    "the topic is the specific detail, however casually the request is "       \
    "worded (e.g. \"any hints for using voice commands?\", \"any tips "        \
    "about how to keep the air inside fresh?\", \"advice on towing?\").\n"     \
    "- SUMMARY: asks what the collection is, what it covers, or what it is "   \
    "for -- anything about the documents as a whole rather than one detail "   \
    "in them (e.g. \"what is this about?\", \"what are these documents?\", "   \
    "\"summarize this\", \"what topics are covered?\"). Choose SUMMARY for "   \
    "any question about the CONTENT of the documents that "                    \
    "names no specific detail to look up.\n"                                   \
    "- CHAT: asks nothing about the content of the documents. Greetings, "     \
    "thanks, apologies, acknowledgements, small talk, and questions about "    \
    "this conversation itself (e.g. \"thank you!\", \"that was helpful\", "     \
    "\"hello\", \"what did I just ask you?\"). ALSO questions about YOU "       \
    "rather than about the documents: what you are, what you can do, what "    \
    "you have access to, or whether you can reach anything outside the "       \
    "documents (e.g. \"what are you?\", \"what can you do?\", \"can you "       \
    "search the web?\", \"can you check online for X?\", \"can you email "      \
    "this?\"). BUT \"can I ...\" or \"how can I ...\" about using, "           \
    "operating or adjusting something the documents describe is the USER "     \
    "asking about the subject matter, not about you -- that is SEARCH "        \
    "(e.g. \"can I make a phone call using the Uconnect system?\", \"how "     \
    "can I adjust the volume?\", \"can I zoom the camera image?\").\n\n"       \
    "Rules. A message that refers to the documents in any way -- \"the "       \
    "documents\", \"the corpus\", \"this file\", a filename -- is SEARCH or "  \
    "SUMMARY, never CHAT: the documents exist and are available, so such a "   \
    "message is always a real request about them. Reserve CHAT for messages "  \
    "that would make just as much sense with no documents present at all.\n"   \
    "Exception: a question about whether you can reach something OUTSIDE "     \
    "the documents -- the web, the internet, email, other files, another "     \
    "group, the user's computer -- is CHAT even when it names a document "     \
    "topic. \"Can you check online whether there is a recall on this "         \
    "vehicle?\" is CHAT: what is being asked for (the internet) is outside "   \
    "the documents. A request to look something up INSIDE the documents "      \
    "stays SEARCH or SUMMARY no matter how it is phrased -- \"can you find "   \
    "the towing limit?\", \"could you look up the minimum age?\", \"are you "  \
    "able to tell me the tire pressure?\" are all ordinary document "          \
    "requests.\n"                                                              \
    "Earlier small talk does not make the current message conversational -- "  \
    "judge what THIS message is asking for.\n"                                 \
    "But a message that continues the previous request -- \"is that all?\", "   \
    "\"what about the others?\", \"tell me more\", \"and X?\" -- is asking for " \
    "more of whatever was just answered, and inherits that request's subject " \
    "even though it names none of its own. Such a follow-up is SEARCH or "     \
    "SUMMARY, never CHAT.\n\n"                                                 \
    "Message: \""

/* Appended to the router prompt only when the previous answer in this
 * conversation came from a retrieval tool. Stating it as a fact beats
 * hoping the model infers it from the transcript, and it is what makes the
 * follow-up rule above actionable: "is that all?" is only unambiguously a
 * document request when the thing it follows was one. */
#define LEXIS_PROMPT_TOOL_ROUTER_PRIOR_RETRIEVAL                              \
    "\n\nNote: the previous answer in this conversation was drawn from the "  \
    "documents, so a follow-up here is very likely asking for more of that."

/* -- Conversation (no retrieval) --------------------------------------- */

/* The CHAT path had no instruction block at all: it sent the bare question
 * and the model answered as a general-purpose assistant with no idea a
 * document collection was attached. That is how "What about the other
 * buttons on the steering wheel?" produced a generic essay about modern
 * cars ending in "please tell me the make, model, and year of your car" --
 * the rule forbidding exactly that lives in the ANSWER prompts, and this
 * path never used them.
 *
 * This is the ONLY prompt that describes what LEXIS is, deliberately. It
 * is the one path where the model speaks as the application rather than
 * answering from retrieved text, so it is the only place where "what are
 * you?" or "can you check the recall notices?" can land.
 *
 * The router does not get this: it is a one-word classification, and every
 * added sentence is more for it to weigh. The answer prompts do not get it
 * either, and that one is a real conflict rather than a preference --
 * LEXIS_PROMPT_RULE_PLAIN_PROSE tells the model not to describe how it
 * knows what it knows, precisely because it was leaking retrieval
 * bookkeeping into prose ("this is found in chunk 8"). Handing it the
 * vocabulary of the retrieval architecture in the same prompt would push
 * directly against that.
 *
 * What it says is capability-level, never implementation: no mention of
 * BM25, chunks, passages or Postgres, for the same leak-avoidance reason.
 * The limits matter more than the description -- without them the model
 * does not know it cannot reach the web or anything outside the active
 * group, and will make a plausible-looking attempt.
 *
 * Still kept short: this path exists for greetings and acknowledgements,
 * and every sentence is prefill paid on every "thanks". */
#define LEXIS_PROMPT_CONVERSE_HEAD                                            \
    "You are LEXIS, an assistant that answers questions about the user's "    \
    "own documents. The user organises documents into groups and asks "       \
    "questions about one group at a time; you can answer a specific "         \
    "question from that group's documents, or describe what the group "       \
    "covers as a whole. You cannot see anything outside the active group -- " \
    "no other group, no web access, no files on the user's machine -- so if " \
    "asked for something beyond it, say plainly that you only have this "     \
    "group's documents.\n\n"                                                 \
    "The message below is conversational rather than a request for "          \
    "information from those documents, so reply directly and briefly. "       \
    "The conversation so far is visible to you -- questions about it "        \
    "(\"what did I ask earlier?\") are answered by reading it, not "          \
    "declined. "                                                              \
    LEXIS_PROMPT_RULE_NO_ASK_FOR_DOCS                                         \
    "Never ask which product, vehicle, model or version the user means -- "   \
    "the collection is already loaded and is the only subject. If the "       \
    "message does need information from the documents after all, offer to "   \
    "look it up.\n\nMessage: "
/* No verbatim example reply in this prompt on purpose: with one present
 * the model parroted the example word-for-word ("Happy to help -- ask me
 * anything...") instead of engaging with the actual message -- observed
 * live for "It does - look again" and "What was it before that?". */

/* -- Answer generation ------------------------------------------------- */

/* SEARCH path: answer from BM25-retrieved passages. Caller appends the
 * labelled passage blocks, then the question.
 *
 * Rule order is the order the model has to make the decisions in: what
 * the passages are and how far to trust them, then whether they settle
 * the question, then what to do when they do not, then how to write it. */
#define LEXIS_PROMPT_ANSWER_FROM_PASSAGES_HEAD                               \
    "You are answering a question using only the provided context. "         \
    LEXIS_PROMPT_RULE_KEYWORD_MATCHES                                        \
    LEXIS_PROMPT_RULE_GROUNDED                                               \
    LEXIS_PROMPT_RULE_UNCERTAINTY                                            \
    LEXIS_PROMPT_RULE_NO_ASK_FOR_DOCS                                        \
    LEXIS_PROMPT_RULE_PLAIN_PROSE                                            \
    "\n\nContext:\n\n"

/* Retired READ path: answer from whole document text. Still defined
 * because generation_generate_answer_from_documents() still exists as the
 * escalation path a future SUMMARY -> full-read fallback would use; no
 * caller reaches it today. See LIMITATIONS.md. */
#define LEXIS_PROMPT_ANSWER_FROM_DOCUMENTS_HEAD                              \
    "You are answering a question using the full text of the documents "     \
    "below. "                                                                \
    LEXIS_PROMPT_RULE_GROUNDED                                               \
    LEXIS_PROMPT_RULE_UNCERTAINTY                                            \
    LEXIS_PROMPT_RULE_NO_ASK_FOR_DOCS                                        \
    LEXIS_PROMPT_RULE_PLAIN_PROSE                                            \
    "\n\nContext:\n\n"

/* SUMMARY path: answer a broad question from the group's cached overview
 * (corpus_summary.h) instead of from document text.
 *
 * No _UNCERTAINTY here, and the added rule is its narrower cousin. This
 * path is reached for questions about the collection as a whole, which
 * the overview is by construction able to answer -- hedging over a broad
 * question it does answer would be a regression. The one real gap is a
 * misroute: a question naming a specific detail lands here, the overview
 * has the topic but not the number, and the model supplies a number. So
 * the instruction is scoped to exactly that -- do not invent a specific
 * the overview does not hold; say what it does cover and that the detail
 * itself is a lookup. */
#define LEXIS_PROMPT_ANSWER_FROM_SUMMARY_HEAD                                \
    "You are answering a question about a collection of documents, using "   \
    "the overview of that collection given below. The overview describes "   \
    "documents that have already been provided and indexed. It describes "   \
    "the collection in general terms and does not hold the details inside "  \
    "the documents, so if the question asks for a specific fact, figure "    \
    "or instruction, never produce one that is not in the overview: say "    \
    "what the overview does show about that topic, and that the detail "     \
    "itself would have to be looked up in the documents. "                   \
    LEXIS_PROMPT_RULE_NO_ASK_FOR_DOCS                                        \
    LEXIS_PROMPT_RULE_PLAIN_PROSE                                            \
    "\n\nCollection overview:\n\n"

/* -- Summary construction ---------------------------------------------- */

/* Builds the cached group overview from sampled document excerpts.
 *
 * "These excerpts are the entire input" is the same defence as
 * LEXIS_PROMPT_RULE_NO_ASK_FOR_DOCS but stated for a different situation:
 * here the model genuinely is seeing excerpts rather than whole documents,
 * so telling it the documents are "provided and indexed" would invite it
 * to ask for the rest. It must summarize what is in front of it. */
#define LEXIS_PROMPT_BUILD_SUMMARY_HEAD                                        \
    "Below are excerpts from every document in a collection. Write a single "   \
    "overview of what this collection contains: the kind of documents it "     \
    "holds, the main subjects they cover, and the sorts of questions it "      \
    "could answer. Aim for 120-200 words of plain prose.\n\n"                  \
    "These excerpts are the entire input. Do not ask for more documents and "  \
    "do not say that documents are missing -- summarize what is here. Do not " \
    "mention excerpts, sources, or that you were given context; write the "    \
    "overview as a description of the collection itself.\n\n"                  \
    "Excerpts:\n\n"

/* -- Query formulation ------------------------------------------------- */

/* Sense-filters WordNet-derived expansion candidates for a BM25 query.
 * The model's job is VETO, not selection: the caller searches the
 * original question terms unconditionally (enforced in
 * query_formulation_parse_selected_terms(), not just prompted), so the
 * only live decision here is which related words match the sense the
 * question intends. The previous "select every word likely to appear in
 * a relevant document" objective was measured hurting retrieval --
 * generic sense-correct expansions outscored rare question terms, and
 * hyponyms (since removed from the prompt entirely) enumerated the
 * answer space -- see LIMITATIONS.md. Caller appends the original
 * question, then the _CANDIDATES block, then the per-term lists. */
#define LEXIS_PROMPT_QUERY_TERMS_HEAD                                        \
    "You are refining optional expansion words for a keyword-based (BM25) " \
    "search query. The question's own terms will always be searched; your " \
    "only job is deciding which related words genuinely fit the question." \
    "\n\n"

#define LEXIS_PROMPT_QUERY_TERMS_CANDIDATES                                    \
    "\"\n\nFor each query term below, related words are listed: synonyms "     \
    "(same meaning), hypernyms (broader terms), and words used in "           \
    "similar contexts. Select ONLY related "       \
    "words that (1) match the meaning the question uses -- discard words "     \
    "that belong to a different sense of the term -- and (2) would likely "    \
    "appear in a passage answering this exact question. Prefer few, "          \
    "precise words; selecting none is often the right answer. Respond "        \
    "with ONLY a JSON array of strings -- no other text.\n\n"

/* Rewrites a follow-up into a standalone question so BM25 has real terms to
 * work with -- "what about that instead?" retrieves nothing on its own. The
 * rewritten form is used only for retrieval; generation always sees the
 * user's original wording (see generation.h). */
#define LEXIS_PROMPT_CONTEXTUALIZE_HEAD                                      \
    "Given the conversation so far, rewrite the following question as a "    \
    "standalone question that makes sense with no prior context -- resolve " \
    "any pronouns or references to what was discussed earlier. Respond "     \
    "with ONLY the rewritten question, no other text.\n\nQuestion: \""

#endif /* LEXIS_PROMPTS_H */
