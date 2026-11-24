/* tl.h - translations.
 *
 * Ren'Py keeps a game's translations in `tl/<language>/`, one .rpy per source
 * file, holding two unrelated kinds of entry:
 *
 *   - dialogue, one block per say statement, keyed by a translation
 *     identifier;
 *   - strings, `old`/`new` pairs, which is what `_("...")` resolves through.
 *
 * The identifier is `<label>_<digest>`, the digest being the first eight hex
 * characters of the MD5 of the statement's source form, and it has to be
 * reproduced rather than worked around: the same line of dialogue can occur
 * many times in a script with a different translation each time, so the
 * original text is not a key. The digest is computed by the parser, which is
 * the only place that sees a statement's source; see mr_tl_identifier.
 *
 * Nothing is substituted at load time. The script itself branches on
 * `_preferences.language` (this game does so for 36 chapter titles), and the
 * player can switch language at any moment, so the original text stays the
 * thing that is stored -- in the statement, in a save -- and the translation is
 * looked up every time it is displayed. A lookup is two hashes and a binary
 * search, cheap enough to do per frame.
 */

#ifndef MR_TL_H
#define MR_TL_H

/* --- identifiers, used at parse time --- */

/* Ren'Py's identifier for a say statement: `label_digest`, where the digest
 * covers the statement as it was written. `label` is the enclosing label, or
 * NULL for a statement before any (the digest then stands alone). `who` is the
 * speaker as written -- the character variable, not its displayed name -- or
 * NULL for narration; `what` is the dialogue with escapes already resolved;
 * `with_` is the text of a trailing `with` clause, or NULL.
 *
 * `nointeract` is for a say that does not wait to be dismissed, which in
 * practice means a menu's caption. Ren'Py writes the word into the digested
 * code -- `if not self.interact: rv.append("nointeract")`, ast.py:932 -- so a
 * caption's identifier differs from the same line written as ordinary
 * dialogue, and getting this wrong means the translation is simply never
 * found. Note the position: Ren'Py appends it *before* the `with` clause,
 * which get_code puts last. The two cannot currently co-occur -- parse_menu
 * requires end-of-line after a say menuitem (parser.py:267), so a caption
 * has no `with` -- so the order is unverifiable against a real identifier
 * and is written to match ast.py rather than tested.
 *
 * `dup` is how many earlier statements in the same file already produced this
 * identifier, which Ren'Py disambiguates with a `_1`, `_2` suffix. */
void mr_tl_identifier(const char *label, const char *who, const char *what,
                      const char *with_, int nointeract, int dup,
                      char *out, int outsz);

/* --- the catalogue --- */

/* Select a language. NULL (or "None", or the empty string) means the language
 * the script is written in, for which there is no catalogue. Reloads from the
 * VFS; safe to call with the language already in force, which does nothing. */
void mr_tl_set_language(const char *lang);

/* The language in force, or NULL for the script's own. */
const char *mr_tl_language(void);

/* Follow `_preferences.language`, which is where the language actually lives:
 * the flag buttons write it, and loading a save can change it. Called once per
 * frame; a no-op unless it has changed. Returns 1 if it reloaded. */
int mr_tl_sync(void);

/* The translation of the say statement with this identifier, or `orig` when
 * there is none -- an untranslated line, or the original language. Valid until
 * the language changes. */
const char *mr_tl_dialogue(const char *tlid, const char *orig);

/* Translate a `_("...")` string: character names, button labels, menu
 * choices. Keyed on the text, which is how Ren'Py keys these. */
const char *mr_tl_string(const char *orig);

void mr_tl_shutdown(void);

#endif /* MR_TL_H */
