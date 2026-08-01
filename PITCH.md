# LEXIS, in plain terms

LEXIS is a search-and-answer engine. You give it a pile of documents.
Later, someone asks it a question in plain English, and it finds the
right passages and writes a real, grounded answer — not a list of blue
links, an actual paragraph that answers the question, built only from
what's actually in your documents.

No vector databases, no external AI API bills per search, and nothing
leaves the machine it runs on. That last part isn't a minor detail —
it's most of the pitch.

## How it works, in three steps

**1. Ingest — reading everything in and building an index.**

Feed LEXIS a pile of documents (a folder of files, or one big file with
one document per line). It splits each document into bite-sized
"passages" — a paragraph or two at a time — and builds an index of
which words show up in which passages. Think of the index at the back
of a textbook, except LEXIS builds one automatically, for every word,
across millions of documents, in minutes.

The clever part is *how* it builds that index. Rather than looking up
and updating one shared word-list one document at a time (which gets
slower the more documents pile up, because everyone's fighting over the
same list), LEXIS lets many workers process documents fully in parallel,
completely independently, and only does the "merge everyone's words
into one shared list" step once, at the very end, in a single efficient
pass. It's the difference between ten people writing on ten separate
notepads and combining them once at the end, versus ten people trying to
write in the same notebook at the same time.

**2. Ask — turning a question into the right search.**

When someone asks a question, LEXIS doesn't just look for the exact
words they typed. It uses a small AI model plus a thesaurus-like
reference (WordNet) to also consider related words and synonyms — so a
question about "car problems" can still find a passage that only ever
said "automobile issues." It then searches the index for passages
containing those words and ranks them using a scoring method that's been
the backbone of search engines for decades: passages where the search
terms show up often, in a reasonably sized passage, rank higher than
ones where the terms are buried or diluted. This step is close to
instant — milliseconds — no matter how many millions of documents are
indexed, because it's a direct index lookup, not a search through
everything.

**3. Answer — an AI that has to show its work.**

LEXIS hands the best few matching passages to a locally-running AI
model and asks it to write an answer *using only those passages* — not
its own general knowledge. This one small design choice does a lot:
because the AI is told exactly what it's allowed to draw from, and
every answer traces back to specific retrieved text, it's far less
likely to confidently make something up. You can always go look at
which passages it was given.

## Performance, with real numbers

Ingesting 200,000 real documents (from the same benchmark researchers
use to evaluate search engines) took **25.7 seconds** on a single
laptop — roughly **7,800 documents per second**. Scaled up, the full
8.84-million-document version of that same benchmark is projected at
**under 19 minutes**, down from an original, unoptimized build of the
same pipeline that took over two and a half hours. That full run is
happening live as this document is being written.

Searching, once the index is built, is effectively instant regardless
of corpus size. Writing the final answer takes a few seconds, because
an actual AI model is composing real prose, not just returning a list
of matches.

## Why this is built on a real database, not a specialized search index

Most search engines (Elasticsearch, Lucene-based tools, and the like)
store their index in their own custom file format — fast, but a closed
box. You can't easily ask it questions, connect it to anything else, or
build features on top of it without bolting on a second system. LEXIS's
index lives in an ordinary Postgres database instead — the same kind of
database that already runs behind most business software. That trade
gives up a bit of raw indexing speed (search itself is unaffected — it's
still effectively instant) in exchange for a real database sitting
underneath a search engine, not just a search engine. In practice, that
unlocks real product features almost for free, not developer conveniences:

- **Search analytics, out of the box.** What are people actually
  searching for? Which questions come up constantly? Which searches
  come up empty? That's a report against real data, not a project.

- **"Why did this show up?"** Every match can be explained — which
  words it matched on, and how strongly — instead of a black-box
  relevance score nobody can account for.

- **Access control that's actually enforced.** Different users or teams
  seeing different documents isn't a bolted-on filter layer; it's a
  standard database permission, the same tool every other part of the
  business already trusts for exactly this job.

- **A real audit trail.** Every question asked, every passage retrieved,
  every answer given, tied together and queryable. For anything
  touching compliance, legal, or healthcare — "show me exactly what was
  searched and answered, for this user, on this date" is a real
  requirement this architecture answers for free, not a feature
  someone has to build.

- **It plugs into what you already have.** Because it's just database
  tables, this search index can be joined against your actual business
  data — customers, orders, tickets, whatever else already lives in a
  database — instead of living in a separate silo that only speaks its
  own query language.

## The business problem this actually solves

Most "AI search" products today mean sending your documents and your
users' questions to a third-party API, paying per query, and hoping
that company's pricing, terms of service, and data-handling policies
never change in a way that hurts you. LEXIS's answer-writing step runs
an AI model *on the same machine*, not in the cloud. Nothing about a
question, a document, or an answer ever has to leave infrastructure you
control. That's not just a cost story (though it is one — no per-query
API bill, ever) — for any organization that can't put its own data or
its customers' data in front of an external AI vendor at all, it's the
difference between being allowed to use AI search and not.
