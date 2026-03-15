// SPDX-License-Identifier: (GPL-2.0 OR MIT)
#include <linux/ctype.h>
#include <linux/module.h>
#include <linux/glob.h>
#include <linux/export.h>

/*
 * The only reason this code can be compiled as a module is because the
 * ATA code that depends on it can be as well.  In practice, they're
 * both usually compiled in and the module overhead goes away.
 */
MODULE_DESCRIPTION("glob(7) matching");
MODULE_LICENSE("Dual MIT/GPL");

/*
 * __glob_match - Shell-style pattern matching, like !fnmatch(pat, str, 0)
 * @pat: Shell-style pattern to match, e.g. "*.[ch]".
 * @str: String to match.  The pattern must match the entire string.
 *
 * Perform shell-style glob matching, returning true (1) if the match
 * succeeds, or false (0) if it fails.  Equivalent to !fnmatch(@pat, @str, 0).
 *
 * Pattern metacharacters are ?, *, [ and \.
 * (And, inside character classes, !, ^ - and ].)
 *
 * This is a small and simple implementation intended for device denylists
 * where a string is matched against a number of patterns.  Thus, it
 * does not preprocess the patterns.  It is non-recursive, and run-time
 * is at most quadratic: strlen(@str)*strlen(@pat).
 *
 * An example of the worst case is glob_match("*aaaaa", "aaaaaaaaaa");
 * it takes 6 passes over the pattern before matching the string.
 *
 * Like !fnmatch(@pat, @str, 0) and unlike the shell, this does NOT
 * treat / or leading . specially; it isn't actually used for pathnames.
 *
 * Note that according to glob(7), character classes are complemented by
 * a leading !.  The regex-style [^a-z] syntax is also accepted as an
 * alias.
 *
 * An opening bracket without a matching close is matched literally.
 */
static bool __pure __glob_match(char const *pat, char const *str, bool nocase)
{
	/*
	 * Backtrack to previous * on mismatch and retry starting one
	 * character later in the string.  Because * matches all characters
	 * (no exception for /), it can be easily proved that there's
	 * never a need to backtrack multiple levels.
	 */
	char const *back_pat = NULL, *back_str = NULL;

	/*
	 * Loop over each token (character or class) in pat, matching
	 * it against the remaining unmatched tail of str.  Return false
	 * on mismatch, or true after matching the trailing nul bytes.
	 */
	for (;;) {
		unsigned char c = *str++;
		unsigned char d = *pat++;

		if (nocase) {
			c = tolower(c);
			d = tolower(d);
		}

		switch (d) {
		case '?':	/* Wildcard: anything but nul */
			if (c == '\0')
				return false;
			break;
		case '*':	/* Any-length wildcard */
			if (*pat == '\0')	/* Optimize trailing * case */
				return true;
			back_pat = pat;
			back_str = --str;	/* Allow zero-length match */
			break;
		case '[': {	/* Character class */
			if (c == '\0')	/* No possible match */
				return false;
			bool match = false, inverted = (*pat == '!' || *pat == '^');
			char const *class = inverted ? pat + 1 : pat;
			unsigned char a = *class++;

			/*
			 * Iterate over each span in the character class.
			 * A span is either a single character a, or a
			 * range a-b.  The first span may begin with ']'.
			 */
			do {
				unsigned char b = a;

				if (a == '\0')	/* Malformed */
					goto literal;

				if (class[0] == '-' && class[1] != ']') {
					b = class[1];

					if (b == '\0')
						goto literal;

					class += 2;
				}
				if (nocase) {
					a = tolower(a);
					b = tolower(b);
				}
				/* Normalize inverted ranges like [z-a] */
				if (a > b) {
					unsigned char tmp = a;

					a = b;
					b = tmp;
				}
				if (a <= c && c <= b)
					match = true;
			} while ((a = *class++) != ']');

			if (match == inverted)
				goto backtrack;
			pat = class;
			}
			break;
		case '\\':
			if (*pat != '\0') {
				d = *pat++;
				if (nocase)
					d = tolower(d);
			}
			fallthrough;
		default:	/* Literal character */
literal:
			if (c == d) {
				if (d == '\0')
					return true;
				break;
			}
backtrack:
			if (c == '\0' || !back_pat)
				return false;	/* No point continuing */
			/* Try again from last *, one character later in str. */
			pat = back_pat;
			str = ++back_str;
			break;
		}
	}
}

/**
 * glob_match - Shell-style pattern matching, like !fnmatch(pat, str, 0)
 * @pat: Shell-style pattern to match, e.g. "*.[ch]".
 * @str: String to match.  The pattern must match the entire string.
 *
 * Perform shell-style glob matching, returning true (1) if the match
 * succeeds, or false (0) if it fails.  Equivalent to !fnmatch(@pat, @str, 0).
 *
 * Pattern metacharacters are ?, *, [ and \.
 * (And, inside character classes, !, ^ - and ].)
 *
 * This is small and simple and non-recursive, with run-time at most
 * quadratic: strlen(@str)*strlen(@pat).  It does not preprocess the
 * patterns.  An opening bracket without a matching close is matched
 * literally.
 *
 * Return: true if @str matches @pat, false otherwise.
 */
bool __pure glob_match(char const *pat, char const *str)
{
	return __glob_match(pat, str, false);
}
EXPORT_SYMBOL(glob_match);

/**
 * glob_match_nocase - Case-insensitive shell-style pattern matching
 * @pat: Shell-style pattern to match.
 * @str: String to match.  The pattern must match the entire string.
 *
 * Identical to glob_match(), but performs case-insensitive comparisons
 * for literal characters and character class contents using tolower().
 * Metacharacters (?, *, [, ]) are not affected by case folding.
 *
 * Return: true if @str matches @pat (case-insensitive), false otherwise.
 */
bool __pure glob_match_nocase(char const *pat, char const *str)
{
	return __glob_match(pat, str, true);
}
EXPORT_SYMBOL(glob_match_nocase);

/**
 * glob_validate - Check whether a glob pattern is well-formed
 * @pat: Shell-style pattern to validate.
 *
 * Return: true if @pat is a syntactically valid glob pattern, false
 * if it contains malformed constructs.  The following are considered
 * invalid:
 *
 *  - An opening '[' with no matching ']' (unclosed character class).
 *  - A trailing '\' with no character following it.
 *
 * Note that glob_match() handles these gracefully (an unclosed bracket
 * is matched literally, a trailing backslash matches itself), but
 * callers that accept patterns from user input may wish to reject
 * malformed patterns early with a clear error.
 */
bool __pure glob_validate(char const *pat)
{
	while (*pat) {
		switch (*pat++) {
		case '\\':
			if (*pat == '\0')
				return false;
			pat++;
			break;
		case '[':
			if (*pat == '!' || *pat == '^')
				pat++;
			/* ] as first character is literal, not end of class */
			if (*pat == ']')
				pat++;
			while (*pat && *pat != ']')
				pat++;
			if (*pat == '\0')
				return false;
			pat++;	/* skip ']' */
			break;
		}
	}
	return true;
}
EXPORT_SYMBOL(glob_validate);
