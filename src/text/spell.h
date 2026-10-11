/* Spelling checker for Edit > Check Spelling (port of crates/text/src/spell.rs).
 *
 * The dictionary is SCOWL size 50 (American English, ~72k words; assets/dict/LICENSE-SCOWL.txt),
 * compiled in gzip-compressed and decoded on first use. Words are grouped by SCOWL level (10 =
 * most common ... 50), which ranks suggestions. Case rules: a lowercase dictionary word may be
 * written capitalised or in all caps; a capitalised entry (proper noun) must keep its capital.
 * A trailing possessive 's is ignored, typographic apostrophes count as ', and tokens with
 * digits are skipped.
 *
 * Threads: a dictionary is immutable once built; pc_spell_english() is built once (pthread_once)
 * and may be used from any thread. Strings are UTF-8 with explicit lengths (invalid bytes count
 * as U+FFFD). */
#ifndef PC_TEXT_SPELL_H
#define PC_TEXT_SPELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

typedef struct pc_spell_dict pc_spell_dict;

/* User words, compared with the lowercase form of a word (HashSet<String> in the reference). */
typedef struct pc_spell_user {
    const char *const *words; /* NUL-terminated UTF-8 */
    size_t n;
} pc_spell_user;

/* The bundled English dictionary (built once; an empty dictionary if that runs out of memory). */
const pc_spell_dict *pc_spell_english(void);
/* A dictionary from one word per line; `#NN` lines set the level of the words after. */
pc_status pc_spell_from_list(const char *list, size_t len, pc_spell_dict **out, pc_error *err);
void pc_spell_free(pc_spell_dict *d); /* NULL-safe; never frees the English dictionary */

size_t pc_spell_len(const pc_spell_dict *d);

/* Whether word is spelled correctly (user may be NULL). Out of memory counts as correct. */
bool pc_spell_check(const pc_spell_dict *d, const char *word, size_t len, const pc_spell_user *user);

/* A list of owned NUL-terminated strings. */
typedef struct pc_spell_list {
    char **v;
    size_t n;
} pc_spell_list;

void pc_spell_list_fini(pc_spell_list *l);

/* Up to max suggestions, nearest first (then most common), with the word's case applied. */
pc_status pc_spell_suggest(const pc_spell_dict *d, const char *word, size_t len, size_t max,
                           pc_spell_list *out, pc_error *err);

/* A word span: byte range in the checked text. */
typedef struct pc_spell_span {
    size_t start, end;
} pc_spell_span;

/* Byte ranges of the words in text: letters/digits with inner apostrophes (hyphens split).
 * *out is freed with free(). */
pc_status pc_spell_words(const char *text, size_t len, pc_spell_span **out, size_t *n, pc_error *err);
/* Misspelled words of text, in order (spans into text). */
pc_status pc_spell_misspellings(const pc_spell_dict *d, const char *text, size_t len,
                                const pc_spell_user *user, pc_spell_span **out, size_t *n, pc_error *err);

/* Damerau-Levenshtein (optimal string alignment) distance of two code point strings, or
 * SIZE_MAX when above max (exposed for tests). */
size_t pc_spell_distance(const uint32_t *a, size_t n, const uint32_t *b, size_t m, size_t max);

#endif
