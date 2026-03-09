#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright(c) 2025: Mauro Carvalho Chehab <mchehab@kernel.org>.

"""
Regular expression ancillary classes.

Those help caching regular expressions and do matching for kernel-doc.
"""

import re

# Local cache for regular expressions
re_cache = {}


class KernRe:
    """
    Helper class to simplify regex declaration and usage.

    It calls re.compile for a given pattern. It also allows adding
    regular expressions and define sub at class init time.

    Regular expressions can be cached via an argument, helping to speedup
    searches.
    """

    def _add_regex(self, string, flags):
        """
        Adds a new regex or reuses it from the cache.
        """
        self.regex = re_cache.get(string, None)
        if not self.regex:
            self.regex = re.compile(string, flags=flags)
            if self.cache:
                re_cache[string] = self.regex

    def __init__(self, string, cache=True, flags=0):
        """
        Compile a regular expression and initialize internal vars.
        """

        self.cache = cache
        self.last_match = None

        self._add_regex(string, flags)

    def __str__(self):
        """
        Return the regular expression pattern.
        """
        return self.regex.pattern

    def __repr__(self):
        """
        Returns a displayable version of the class init.
        """

        flag_map = {
            re.IGNORECASE: "re.I",
            re.MULTILINE: "re.M",
            re.DOTALL: "re.S",
            re.VERBOSE: "re.X",
        }

        flags = []
        for flag, name in flag_map.items():
            if self.regex.flags & flag:
                flags.append(name)

        flags_name = " | ".join(flags)

        if flags_name:
            return f'KernRe("{self.regex.pattern}", {flags_name})'
        else:
            return f'KernRe("{self.regex.pattern}")'

    def __add__(self, other):
        """
        Allows adding two regular expressions into one.
        """

        return KernRe(str(self) + str(other), cache=self.cache or other.cache,
                  flags=self.regex.flags | other.regex.flags)

    def match(self, string):
        """
        Handles a re.match storing its results.
        """

        self.last_match = self.regex.match(string)
        return self.last_match

    def search(self, string):
        """
        Handles a re.search storing its results.
        """

        self.last_match = self.regex.search(string)
        return self.last_match

    def finditer(self,  string):
        """
        Alias to re.finditer.
        """

        return self.regex.finditer(string)

    def findall(self, string):
        """
        Alias to re.findall.
        """

        return self.regex.findall(string)

    def split(self, string):
        """
        Alias to re.split.
        """

        return self.regex.split(string)

    def sub(self, sub, string, count=0):
        """
        Alias to re.sub.
        """

        return self.regex.sub(sub, string, count=count)

    def group(self, num):
        """
        Returns the group results of the last match.
        """

        return self.last_match.group(num)

    def groups(self):
        """
        Returns the group results of the last match
        """

        return self.last_match.groups()

class TokType():

    @staticmethod
    def __str__(val):
        """Return the name of an enum value"""
        return TokType._name_by_val.get(val, f"UNKNOWN({val})")

class CToken():
    """
    Data class to define a C token.
    """

    # Tokens that can be used by the parser. Works like an C enum.

    COMMENT = 0     #: A standard C or C99 comment, including delimiter.
    STRING = 1      #: A string, including quotation marks.
    CHAR = 2        #: A character, including apostophes.
    NUMBER = 3      #: A number.
    PUNC = 4        #: A puntuation mark: ``;`` / ``,`` / ``.``.
    BEGIN = 5       #: A begin character: ``{`` / ``[`` / ``(``.
    END = 6         #: A end character: ``}`` / ``]`` / ``)``.
    CPP = 7         #: A preprocessor macro.
    HASH = 8        #: The hash character - useful to handle other macros.
    OP = 9          #: A C operator (add, subtract, ...).
    STRUCT = 10     #: A ``struct`` keyword.
    UNION = 11      #: An ``union`` keyword.
    ENUM = 12       #: A ``struct`` keyword.
    TYPEDEF = 13    #: A ``typedef`` keyword.
    NAME = 14       #: A name. Can be an ID or a type.
    SPACE = 15      #: Any space characters, including new lines

    MISMATCH = 255  #: an error indicator: should never happen in practice.

    # Dict to convert from an enum interger into a string.
    _name_by_val = {v: k for k, v in dict(vars()).items() if isinstance(v, int)}

    # Dict to convert from string to an enum-like integer value.
    _name_to_val = {k: v for v, k in _name_by_val.items()}

    @staticmethod
    def to_name(val):
        """Convert from an integer value from CToken enum into a string"""

        return CToken._name_by_val.get(val, f"UNKNOWN({val})")

    @staticmethod
    def from_name(name):
        """Convert a string into a CToken enum value"""
        if name in CToken._name_to_val:
            return CToken._name_to_val[name]

        return CToken.MISMATCH

    def __init__(self, kind, value, pos,
                 brace_level, paren_level, bracket_level):
        self.kind = kind
        self.value = value
        self.pos = pos
        self.brace_level = brace_level
        self.paren_level = paren_level
        self.bracket_level = bracket_level

    def __repr__(self):
        name = self.to_name(self.kind)
        if isinstance(self.value, str):
            value = '"' + self.value + '"'
        else:
            value = self.value

        return f"CToken({name}, {value}, {self.pos}, " \
               f"{self.brace_level}, {self.paren_level}, {self.bracket_level})"

#: Tokens to parse C code.
TOKEN_LIST = [
    (CToken.COMMENT, r"//[^\n]*|/\*[\s\S]*?\*/"),

    (CToken.STRING,  r'"(?:\\.|[^"\\])*"'),
    (CToken.CHAR,    r"'(?:\\.|[^'\\])'"),

    (CToken.NUMBER,  r"0[xX][0-9a-fA-F]+[uUlL]*|0[0-7]+[uUlL]*|"
                     r"[0-9]+(\.[0-9]*)?([eE][+-]?[0-9]+)?[fFlL]*"),

    (CToken.PUNC,    r"[;,\.]"),

    (CToken.BEGIN,   r"[\[\(\{]"),

    (CToken.END,     r"[\]\)\}]"),

    (CToken.CPP,     r"#\s*(define|include|ifdef|ifndef|if|else|elif|endif|undef|pragma)\b"),

    (CToken.HASH,    r"#"),

    (CToken.OP,      r"\+\+|\-\-|\->|==|\!=|<=|>=|&&|\|\||<<|>>|\+=|\-=|\*=|/=|%="
                     r"|&=|\|=|\^=|=|\+|\-|\*|/|%|<|>|&|\||\^|~|!|\?|\:"),

    (CToken.STRUCT,  r"\bstruct\b"),
    (CToken.UNION,   r"\bunion\b"),
    (CToken.ENUM,    r"\benum\b"),
    (CToken.TYPEDEF, r"\bkinddef\b"),

    (CToken.NAME,      r"[A-Za-z_][A-Za-z0-9_]*"),

    (CToken.SPACE,   r"[\s]+"),

    (CToken.MISMATCH,r"."),
]

#: Handle C continuation lines.
RE_CONT = KernRe(r"\\\n")

RE_COMMENT_START = KernRe(r'/\*\s*')

#: tokenizer regex. Will be filled at the first CTokenizer usage.
re_scanner = None

class CTokenizer():
    """
    Scan C statements and definitions and produce tokens.

    When converted to string, it drops comments and handle public/private
    values, respecting depth.
    """

    # This class is inspired and follows the basic concepts of:
    #   https://docs.python.org/3/library/re.html#writing-a-tokenizer

    def _tokenize(self, source):
        """
        Interactor that parses ``source``, splitting it into tokens, as defined
        at ``self.TOKEN_LIST``.

        The interactor returns a CToken class object.
        """

        # Handle continuation lines. Note that kdoc_parser already has a
        # logic to do that. Still, let's keep it for completeness, as we might
        # end re-using this tokenizer outsize kernel-doc some day - or we may
        # eventually remove from there as a future cleanup.
        source = RE_CONT.sub("", source)

        brace_level = 0
        paren_level = 0
        bracket_level = 0

        for match in re_scanner.finditer(source):
            kind = CToken.from_name(match.lastgroup)
            pos = match.start()
            value = match.group()

            if kind == CToken.MISMATCH:
                raise RuntimeError(f"Unexpected token '{value}' on {pos}:\n\t{source}")
            elif kind == CToken.BEGIN:
                if value == '(':
                    paren_level += 1
                elif value == '[':
                    bracket_level += 1
                else:  # value == '{'
                    brace_level += 1

            elif kind == CToken.END:
                if value == ')' and paren_level > 0:
                    paren_level -= 1
                elif value == ']' and bracket_level > 0:
                    bracket_level -= 1
                elif brace_level > 0:    # value == '}'
                    brace_level -= 1

            yield CToken(kind, value, pos,
                         brace_level, paren_level, bracket_level)

    def __init__(self, source):
        """
        Create a regular expression to handle TOKEN_LIST.

        While I generally don't like using regex group naming via:
            (?P<name>...)

        in this particular case, it makes sense, as we can pick the name
        when matching a code via re_scanner().
        """
        global re_scanner

        if not re_scanner:
            re_tokens = []

            for kind, pattern in TOKEN_LIST:
                name = CToken.to_name(kind)
                re_tokens.append(f"(?P<{name}>{pattern})")

            re_scanner = KernRe("|".join(re_tokens), re.MULTILINE | re.DOTALL)

        self.tokens = []
        for tok in self._tokenize(source):
            self.tokens.append(tok)

    def __str__(self):
        out=""
        show_stack = [True]

        for tok in self.tokens:
            if tok.kind == CToken.BEGIN:
                show_stack.append(show_stack[-1])

            elif tok.kind == CToken.END:
                prev = show_stack[-1]
                if len(show_stack) > 1:
                    show_stack.pop()

                if not prev and show_stack[-1]:
                    #
                    # Try to preserve indent
                    #
                    out += "\t" * (len(show_stack) - 1)

                    out += str(tok.value)
                    continue

            elif tok.kind == CToken.COMMENT:
                comment = RE_COMMENT_START.sub("", tok.value)

                if comment.startswith("private:"):
                    show_stack[-1] = False
                    show = False
                elif comment.startswith("public:"):
                    show_stack[-1] = True

                continue

            if show_stack[-1]:
                    out += str(tok.value)

        return out


#: Nested delimited pairs (brackets and parenthesis)
DELIMITER_PAIRS = {
    '{': '}',
    '(': ')',
    '[': ']',
}

#: compiled delimiters
RE_DELIM = KernRe(r'[\{\}\[\]\(\)]')


class NestedMatch:
    """
    Finding nested delimiters is hard with regular expressions. It is
    even harder on Python with its normal re module, as there are several
    advanced regular expressions that are missing.

    This is the case of this pattern::

            '\\bSTRUCT_GROUP(\\(((?:(?>[^)(]+)|(?1))*)\\))[^;]*;'

    which is used to properly match open/close parentheses of the
    string search STRUCT_GROUP(),

    Add a class that counts pairs of delimiters, using it to match and
    replace nested expressions.

    The original approach was suggested by:

        https://stackoverflow.com/questions/5454322/python-how-to-match-nested-parentheses-with-regex

    Although I re-implemented it to make it more generic and match 3 types
    of delimiters. The logic checks if delimiters are paired. If not, it
    will ignore the search string.
    """

    # TODO: make NestedMatch handle multiple match groups
    #
    # Right now, regular expressions to match it are defined only up to
    #       the start delimiter, e.g.:
    #
    #       \bSTRUCT_GROUP\(
    #
    # is similar to: STRUCT_GROUP\((.*)\)
    # except that the content inside the match group is delimiter-aligned.
    #
    # The content inside parentheses is converted into a single replace
    # group (e.g. r`\0').
    #
    # It would be nice to change such definition to support multiple
    # match groups, allowing a regex equivalent to:
    #
    #   FOO\((.*), (.*), (.*)\)
    #
    # it is probably easier to define it not as a regular expression, but
    # with some lexical definition like:
    #
    #   FOO(arg1, arg2, arg3)

    def __init__(self, regex):
        self.regex = KernRe(regex)

    def _search(self, line):
        """
        Finds paired blocks for a regex that ends with a delimiter.

        The suggestion of using finditer to match pairs came from:
        https://stackoverflow.com/questions/5454322/python-how-to-match-nested-parentheses-with-regex
        but I ended using a different implementation to align all three types
        of delimiters and seek for an initial regular expression.

        The algorithm seeks for open/close paired delimiters and places them
        into a stack, yielding a start/stop position of each match when the
        stack is zeroed.

        The algorithm should work fine for properly paired lines, but will
        silently ignore end delimiters that precede a start delimiter.
        This should be OK for kernel-doc parser, as unaligned delimiters
        would cause compilation errors. So, we don't need to raise exceptions
        to cover such issues.
        """

        stack = []

        for match_re in self.regex.finditer(line):
            start = match_re.start()
            offset = match_re.end()
            string_char = None
            escape = False

            d = line[offset - 1]
            if d not in DELIMITER_PAIRS:
                continue

            end = DELIMITER_PAIRS[d]
            stack.append(end)

            for match in RE_DELIM.finditer(line[offset:]):
                pos = match.start() + offset

                d = line[pos]

                if escape:
                    escape = False
                    continue

                if string_char:
                    if d == '\\':
                        escape = True
                    elif d == string_char:
                        string_char = None

                    continue

                if d in ('"', "'"):
                    string_char = d
                    continue

                if d in DELIMITER_PAIRS:
                    end = DELIMITER_PAIRS[d]

                    stack.append(end)
                    continue

                # Does the end delimiter match what is expected?
                if stack and d == stack[-1]:
                    stack.pop()

                    if not stack:
                        yield start, offset, pos + 1
                        break

    def search(self, line):
        """
        This is similar to re.search:

        It matches a regex that it is followed by a delimiter,
        returning occurrences only if all delimiters are paired.
        """

        for t in self._search(line):

            yield line[t[0]:t[2]]

    def sub(self, sub, line, count=0):
        """
        This is similar to re.sub:

        It matches a regex that it is followed by a delimiter,
        replacing occurrences only if all delimiters are paired.

        if the sub argument contains::

            r'\0'

        it will work just like re: it places there the matched paired data
        with the delimiter stripped.

        If count is different than zero, it will replace at most count
        items.
        """
        out = ""

        cur_pos = 0
        n = 0

        for start, end, pos in self._search(line):
            out += line[cur_pos:start]

            # Value, ignoring start/end delimiters
            value = line[end:pos - 1]

            # replaces \0 at the sub string, if \0 is used there
            new_sub = sub
            new_sub = new_sub.replace(r'\0', value)

            out += new_sub

            # Drop end ';' if any
            if pos < len(line) and line[pos] == ';':
                pos += 1

            cur_pos = pos
            n += 1

            if count and count >= n:
                break

        # Append the remaining string
        l = len(line)
        out += line[cur_pos:l]

        return out

    def __repr__(self):
        """
        Returns a displayable version of the class init.
        """

        return f'NestedMatch("{self.regex.regex.pattern}")'
