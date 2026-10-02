/* Runtime path resolution: the seam between developer checkout and installed app bundle. */

#ifndef LEXIS_PATHS_H
#define LEXIS_PATHS_H

/* NULL keeps a default; strings copied. Call at most once, before anything reads config/resources. */
void lexis_paths_set(const char *resource_dir, const char *config_file);

/* The config file path. Default: "config/lexis.conf". Never NULL. */
const char *lexis_paths_config_file(void);

/* resource_dir + "/" + relative, malloc'd (caller frees). Unset resource_dir = copy of relative. */
char *lexis_paths_resource(const char *relative);

#endif /* LEXIS_PATHS_H */
