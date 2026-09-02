# The question pipeline

What happens between hitting Enter and seeing an answer.

## In the app

1. **Routing.** A quick model call classifies the message: SEARCH (a
   question the documents can answer), SUMMARY (a question about the
   collection as a whole), or CHAT (greetings, questions about the
   conversation). SUMMARY answers from a cached group overview; CHAT
   answers directly. Everything else continues below.

2. **Rewriting.** In an ongoing conversation, a model call resolves
   references: "what about the rear ones?" becomes "how do I adjust
   the rear headrests?". The search uses the union of the original
   and rewritten questions' words, so the rewrite can only add, never
   lose, a term.

3. **Terms.** The question is tokenized, filler words are dropped, and
   each word is reduced to its base form. Result: the core search
   terms, e.g. "tip apply parking brake".

4. **Synonym expansion.** WordNet and the learned synonym table offer
   related words for each term. The model keeps only the ones that fit
   the question's meaning -- for "any tips on the parking brake" it
   might add "guidance", and reject "peak" (a different sense of
   "tip"). Kept synonyms search at 40% of a normal term's weight, so
   they can help a passage but never outweigh the user's actual words.

5. **Search.** BM25 scores every passage sharing any query term and
   ranks the top 40. Passages matching more distinct terms get a
   bonus, which stops one over-represented topic from drowning the one
   passage that matches the whole question.

6. **Re-ranking.** The embedding model scores how close each of the 40
   candidates is to the question in meaning, and that ordering is
   fused with the BM25 ordering. This rescues right-answer passages
   that matched fewer exact words.

7. **Trimming.** The top of the re-ranked list is cut to at most 12
   passages within a token budget -- what actually reaches the model.

8. **Answering.** The chat model gets the passages, the original
   question, and recent conversation history, with strict rules:
   answer only from the passages, plain prose, and -- because the
   passages were picked by keyword match -- don't assume a passage is
   relevant just because it shares words with the question. When the
   passages settle the question it answers directly. When they only
   half-settle it (the topic is there but the number isn't, two
   figures disagree, the spec is for the neighbouring case), it lays
   out what the passages actually say, with the conditions attached,
   and names what's still open, instead of picking the likeliest
   answer and stating it as fact. Only when nothing relevant is there
   at all does it say the question isn't covered.

9. **Retry on refusal.** If the answer is a refusal ("the material
   doesn't contain..."), the pipeline tries once more with a wider
   passage cut and the model's reasoning mode on. Only then does the
   user see a refusal.

10. **Record.** The answer is saved with its provenance: which tool
    ran, the exact search terms, the rewritten question if any, and the
    full text of every passage used. The Source panel shows all
    of it.

Steps 1-8 are visible while they happen: the app's footer reports each
stage as it starts ("Refining the question...", "Searching the
group...", "Reading 12 passages...", "Writing..."), and the answer
streams in token by token as it is written rather than appearing all at
once. The reasoning pass never streams -- if the model thinks, the
stream holds until the thinking block closes. A refusal retry announces
itself ("Trying again with a deeper search...") and restarts the
answer.

**On demand.** The newest SEARCH answer also carries a "Try harder"
action: the same deeper retrieval and forced reasoning the automatic
refusal retry uses, run on request. The replacement answer replaces the
original in the conversation and in history -- but only on success. A
retry that fails, or that finds nothing the second time, leaves the
original answer exactly as it was.

## In the CLI

`./lexis query "..."` runs steps 3-8 identically (same code), minus
routing and conversation history. It also logs every intermediate
step -- prompts, raw model responses, timings -- to the database when
`mode=testing` is set, which is how pipeline problems get diagnosed
from records instead of guesswork.

## Time budget

Typical question: about 8-9 seconds. Almost all of it is the chat
model writing the answer; routing, rewriting, expansion, search, and
re-ranking together take about 2 seconds.
