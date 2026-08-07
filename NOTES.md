BM25 indexing should be one of many tools in the future. Right now bm25 indexing is the default tool, but it doesn't work well for broad questions like "what is this document about?", which are not suited for RAG.
- Question: What would be a better tool? Just give the LLM text from the first few pages of the corpus - a direct_read tool?

Qwen should be in charge of deciding which tool to run, and llama should be the tool for intermediary steps (or smaller models that are not thinking models). Model combination will be an important topic for how this gets architected moving forward.

Fun features that would require tool calling:
- Triggering the bm25 search engine.
- Triggering the direct reading tool for broader questions.
- Generating an output file onto users computer.