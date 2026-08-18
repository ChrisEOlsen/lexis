/*
 * Runtime path resolution -- the one seam between "developer checkout"
 * and "installed app bundle".
 *
 * Every file LEXIS reads falls in one of two buckets:
 *
 *   resources  read-only data shipped with the program: data/wordnet,
 *              data/synonyms, data/stopwords, tessdata. In a checkout
 *              these live under the working directory; in LEXIS.app
 *              they live in Contents/Resources.
 *   config     the mutable per-user config file. In a checkout:
 *              config/lexis.conf. Installed: ~/Library/Application
 *              Support/LEXIS/lexis.conf.
 *
 * Nothing here is loaded from the environment or guessed: the app's
 * main() calls lexis_paths_set() once, before anything else runs, when
 * it detects it is inside a bundle. When it is never called, every
 * default is exactly the relative path the project has always used, so
 * the CLI, the tests, and a dev build of the app behave identically to
 * before this module existed.
 *
 * Model files are NOT resolved here -- their locations come from the
 * config file (model_path / reranker_model_path), which the installed
 * app writes with absolute paths at first run.
 */

#ifndef LEXIS_PATHS_H
#define LEXIS_PATHS_H

/* Both may be NULL to keep that default. Strings are copied. Call at
 * most once, before any module reads config or resources -- the lazy
 * loaders (generation.c, retrieval.c) cache what they find. */
void lexis_paths_set(const char *resource_dir, const char *config_file);

/* The config file path. Default: "config/lexis.conf". Never NULL. */
const char *lexis_paths_config_file(void);

/* resource_dir + "/" + relative, malloc'd (caller frees). With no
 * resource_dir set, a plain copy of `relative` -- the historical
 * working-directory-relative behavior. */
char *lexis_paths_resource(const char *relative);

#endif /* LEXIS_PATHS_H */
