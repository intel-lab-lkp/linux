#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Linux kernel patch/source style checker.
# Complete re-implementation of scripts/checkpatch.pl.
#
# Style: kernel-flavoured, maximum simplicity — no AST, no complex
# state machines.  C statement blocks are located by paren/brace
# depth; structural checks use single-line lookahead.

from __future__ import annotations

import os
import re
import subprocess
import sys
from argparse import ArgumentParser
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Optional

# ---------------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------------

VERSION              = '4.00'
DEFAULT_MAX_LINE     = 100
MAX_SUBJECT          = 75
MAX_COMMIT_BODY_LINE = 75
DEFAULT_TABSIZE      = 8
MIN_SHA1_LEN         = 12
MIN_CONF_DESC_LEN    = 4

ERROR, WARN, CHECK   = 0, 1, 2
LEVEL_NAMES          = ('ERROR', 'WARNING', 'CHECK')
LEVEL_COLORS         = ('\x1b[31m', '\x1b[33m', '\x1b[32m')
RESET, BLUE          = '\x1b[0m', '\x1b[34m'

SCRIPT_DIR           = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONF_DIRS    = ('.', os.path.expanduser('~'),
                        os.path.join(SCRIPT_DIR, '.scripts'))
ENV_CONFIG_DIR       = 'CHECKPATCH_CONFIG_DIR'

SPELLING_FILE        = os.path.join(SCRIPT_DIR, 'spelling.txt')
CONST_STRUCTS_FILE   = os.path.join(SCRIPT_DIR, 'const_structs.checkpatch')
CODESPELL_DEFAULT    = '/usr/share/codespell/dictionary.txt'

# ---------------------------------------------------------------------------
# Regex fragments
# ---------------------------------------------------------------------------

IDENT      = r'[A-Za-z_]\w*'
INT_SUFFIX = r'(?i)(?:llu|ull|ll|lu|ul|l|u)?'
HEX_CONST  = rf'0[xX][0-9a-fA-F]+{INT_SUFFIX}'
INT_CONST  = rf'(?:0[0-7]+|0[xX][0-9a-fA-F]+|[0-9]+){INT_SUFFIX}'
STRING     = r'"(?:\\.|[^"\\])*"'
CHAR       = r"'(?:\\.|[^'\\])*'"

TYPE_BASE = (
    r'(?:'
    r'(?:unsigned|signed)\s+(?:char|short|int|long)'
    r'|unsigned|signed'
    r'|void|char|short|int|long|long\s+long|float|double|bool|_Bool'
    r'|(?:struct|union|enum)\s+\w+'
    r'|__[us](?:8|16|32|64)'
    r'|[us](?:8|16|32|64)'
    r'|\w+_t'
    r')'
)
TYPE = rf'{TYPE_BASE}(?:\s+{TYPE_BASE})*'

RE_SUBJECT    = re.compile(r'^Subject:\s*(.*?)\s*$', re.I)
RE_FROM       = re.compile(r'^From:\s*(.*?)\s*$', re.I)
RE_SOB        = re.compile(r'^\s*Signed-off-by:\s*(.*?)\s*$', re.I)
RE_TAG        = re.compile(r'^\s*([A-Za-z][A-Za-z-]+):\s*(.*)$')
RE_FIXES      = re.compile(
    r'^Fixes:\s*([0-9a-fA-F]{5,40})\s*(?:\(\s*"([^"]*)"\s*\))?', re.I)
RE_LINK       = re.compile(r'^(Link|Closes):\s*(\S+)', re.I)
RE_SPDX       = re.compile(r'SPDX-License-Identifier:\s*(\S[^\n]*)')
RE_C99        = re.compile(r'//')
RE_BLOCK      = re.compile(r'/\*.*?\*/', re.DOTALL)
RE_FALLTHRU   = re.compile(r'/\*\s*(?:fall ?through|fallthrough)\s*\*/', re.I)
RE_TRAIL_WS   = re.compile(r'[ \t]+$')
RE_URL        = re.compile(r'https?://\S+')
RE_LINUX_V    = re.compile(r'\bLINUX_VERSION_CODE\b')
RE_CVS_KW     = re.compile(r'\$(?:Revision|Log|Id|Date|Author)\$')
RE_HUNK_HDR   = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$')
RE_DEFINE     = re.compile(r'^\s*#\s*define\s+(\w+)(\([^)]*\))?\s*(.*)$')
RE_KCONFIG    = re.compile(r'^\s*(config|menuconfig|choice|endchoice|if|'
                           r'endif|menu|endmenu|source)\b')
RE_KCONFIG_SYM = re.compile(r'^\s*(config|menuconfig|choice)\s+(\w+)')
RE_HELP_START = re.compile(r'^\s*help\b')
RE_HELP_TRIG  = re.compile(r'^\s*(?:bool|tristate|prompt|def_bool|'
                           r'def_tristate)\s*["\']')
RE_MAINT_TAG  = re.compile(r'^([A-Z]):\s*(.*)$')
RE_MAINT_ORDER = 'MRLSWQBCPTFXNK'
RE_COMPAT     = re.compile(r'\bcompatible\s*=\s*"([^"]+)"')

RE_DEPRECATED = re.compile(r'\b(' + '|'.join((
    r'kmap', r'kunmap', r'kmap_atomic', r'kunmap_atomic',
    r'DEFINE_IDR', r'idr_init(?:_base)?',
    r'simple_strto[lul]+', r'strict_strto[lul]+')) + r')\s*\(')

# ---------------------------------------------------------------------------
# Reference tables
# ---------------------------------------------------------------------------

SIG_TAGS = {
    'signed-off-by': 'Signed-off-by:', 'co-developed-by': 'Co-developed-by:',
    'acked-by': 'Acked-by:', 'tested-by': 'Tested-by:',
    'reviewed-by': 'Reviewed-by:', 'reported-by': 'Reported-by:',
    'suggested-by': 'Suggested-by:', 'assisted-by': 'Assisted-by:',
}

DEPRECATED_APIS = {
    'kmap': 'kmap_local_page', 'kunmap': 'kunmap_local',
    'kmap_atomic': 'kmap_local_page', 'kunmap_atomic': 'kunmap_local',
    'DEFINE_IDR': 'DEFINE_XARRAY', 'idr_init': 'xa_init',
    'idr_init_base': 'xa_init_flags',
    'simple_strtol': 'kstrtol', 'simple_strtoll': 'kstrtoll',
    'simple_strtoul': 'kstrtoul', 'simple_strtoull': 'kstrtoull',
    'strict_strtol': 'kstrtol', 'strict_strtoll': 'kstrtoll',
    'strict_strtoul': 'kstrtoul', 'strict_strtoull': 'kstrtoull',
}

OBSOLETE_ARCHIVES = (
    'lists.infradead.org', 'lkml.org', 'mail-archive.com',
    'mailman.alsa-project.org', 'marc.info', 'ozlabs.org',
    'spinics.net', 'freedesktop.org/archives/dri-devel',
)

COMMON_TYPOS = {
    'adress': 'address', 'adresses': 'addresses', 'alot': 'a lot',
    'ambiguos': 'ambiguous', 'aquire': 'acquire', 'aquired': 'acquired',
    'becuase': 'because', 'comit': 'commit', 'commited': 'committed',
    'commiting': 'committing', 'configration': 'configuration',
    'enviroment': 'environment', 'existance': 'existence',
    'funtion': 'function', 'funtions': 'functions',
    'implmentation': 'implementation', 'intialize': 'initialize',
    'intialized': 'initialized', 'interupt': 'interrupt',
    'interupts': 'interrupts', 'lenght': 'length', 'mulitple': 'multiple',
    'occured': 'occurred', 'occurence': 'occurrence',
    'paramter': 'parameter', 'paramters': 'parameters',
    'recieve': 'receive', 'recieved': 'received',
    'registred': 'registered', 'retreive': 'retrieve',
    'succesful': 'successful', 'succesfully': 'successfully',
    'sucess': 'success', 'thier': 'their', 'transfered': 'transferred',
    'unecessary': 'unnecessary', 'usefull': 'useful',
    'wich': 'which', 'writeable': 'writable',
}

CAMELCASE_EXCEPTIONS = {
    '_Generic', 'PageSlab', 'PageReserved', 'TestSetPage',
    'TestClearPage', 'ClearPage', 'SetPage',
}

WORLD_WRITABLE_RE = re.compile(
    r'\b(S_IWUGO|S_IWOTH|S_IRWXUGO|S_IALLUGO|0[0-7][0-7][2367])\b')

KNOWN_TYPES = sorted({
    # whitespace
    'TRAILING_WHITESPACE', 'DOS_LINE_ENDINGS', 'CODE_INDENT',
    'SPACE_BEFORE_TAB', 'TABSTOP', 'LONG_LINE', 'C99_COMMENTS',
    'PREFER_FALLTHROUGH', 'LINE_CONTINUATIONS', 'SPLIT_STRING',
    'MISSING_EOF_NEWLINE', 'DEEP_INDENTATION',
    # spacing
    'SPACING', 'POINTER_LOCATION', 'OPEN_BRACE', 'ELSE_AFTER_BRACE',
    'ASSIGNMENT_CONTINUATIONS', 'LOGICAL_CONTINUATIONS',
    'UNNECESSARY_PARENTHESES', 'RETURN_PARENTHESES',
    'TYPECAST_INT_CONSTANT', 'SIZEOF_ADDRESS', 'SIZEOF_PARENTHESIS',
    'MULTIPLE_ASSIGNMENTS', 'COMPARISON_TO_NULL', 'BOOL_COMPARISON',
    'UNNECESSARY_CASTS', 'PARENTHESIS_ALIGNMENT',
    # structure
    'LINE_SPACING', 'UNNECESSARY_ELSE', 'UNNECESSARY_BREAK',
    'SUSPECT_CODE_INDENT', 'TRAILING_STATEMENTS', 'BRACES',
    'RETURN_VOID', 'MULTILINE_DEREFERENCE', 'STORAGE_CLASS',
    'INLINE_LOCATION', 'MISORDERED_TYPE', 'CONST_CONST',
    # macros
    'MULTISTATEMENT_MACRO_USE_DO_WHILE',
    'SINGLE_STATEMENT_DO_WHILE_MACRO', 'TRAILING_SEMICOLON',
    'COMPLEX_MACRO', 'MACRO_ARG_REUSE', 'MACRO_ARG_PRECEDENCE',
    'MACRO_ARG_UNUSED',
    # kernel APIs
    'PRINTK_WITHOUT_KERN_LEVEL', 'PREFER_PR_LEVEL', 'PREFER_DEV_LEVEL',
    'PRINTK_RATELIMITED', 'UNNECESSARY_KERN_LEVEL', 'AVOID_BUG',
    'DEPRECATED_API', 'STRCPY', 'STRNCPY', 'STRLCPY', 'CONSIDER_KSTRTO',
    'USLEEP_RANGE', 'LONG_UDELAY', 'MSLEEP', 'NEW_TYPEDEFS',
    'VOLATILE', 'INLINE', 'UNSPECIFIED_INT', 'UNNECESSARY_INT',
    'JIFFIES_COMPARISON', 'ALLOC_SIZEOF_STRUCT', 'ARRAY_SIZE',
    'BIT_MACRO', 'IS_ENABLED_CONFIG', 'PREFER_IS_ENABLED',
    'USE_NEGATIVE_ERRNO', 'ASSIGNMENT_IN_IF', 'LINUX_VERSION_CODE',
    'DATE_TIME', 'USE_FUNC', 'YIELD', 'PREFER_SEQ_PUTS', 'ENOTSUPP',
    'STATIC_CONST_CHAR_ARRAY', 'STATIC_CONST', 'LIKELY_MISUSE',
    'CONST_STRUCT', 'NR_CPUS', 'IN_ATOMIC', 'CVS_KEYWORD',
    'CAMELCASE', 'TYPO_SPELLING', 'HEXADECIMAL_BOOLEAN_TEST',
    'FUNCTION_WITHOUT_ARGS', 'UAPI_INCLUDE', 'EMBEDDED_FILENAME',
    'DEFAULT_NO_BREAK', 'IF_0', 'IF_1',
    # commit
    'MISSING_SUBJECT', 'LONG_SUBJECT', 'COMMIT_LOG_LONG_LINE',
    'COMMIT_COMMENT_SYMBOL', 'COMMIT_LOG_WRONG_LINK',
    'COMMIT_LOG_VERSIONING', 'DIFF_IN_COMMIT_MSG',
    'MISSING_SIGN_OFF', 'NO_AUTHOR_SIGN_OFF', 'BAD_SIGN_OFF',
    'BAD_FIXES_TAG', 'MISSING_FIXES_TAG', 'BAD_REPORTED_BY_LINK',
    'PREFER_LORE_ARCHIVE', 'PLACEHOLDER_USE', 'GERRIT_CHANGE_ID',
    # structure
    'NOT_UNIFIED_DIFF', 'EXECUTE_PERMISSIONS', 'FILE_PATH_CHANGES',
    'DT_SPLIT_BINDING_PATCH', 'DT_SCHEMA_BINDING_PATCH',
    'SPDX_LICENSE_TAG', 'EMBEDDED_FILENAME',
    # domain
    'CONFIG_DESCRIPTION', 'MAINTAINERS_STYLE',
    'UNDOCUMENTED_DT_STRING', 'UNCOMMENTED_RGMII_MODE',
    'EXPORTED_WORLD_WRITABLE',
})

# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class ParsedLine:
    idx: int
    kind: str
    raw: str
    text: str = ''
    real: int = 0
    patch_linenr: int = 0
    filename: str = ''


@dataclass
class Finding:
    level: int
    msg_type: str
    message: str
    line: int = 0
    patch_line: int = 0
    filename: str = ''
    context: str = ''

    def render(self, *, show_types=False, color=False, terse=False,
               showfile=False):
        parts = []
        if color:
            parts.append(LEVEL_COLORS[self.level])
        parts.append(LEVEL_NAMES[self.level])
        parts.append(':')
        if show_types:
            if color:
                parts.append(BLUE)
            parts.append(f'{self.msg_type}:')
        if color:
            parts.append(RESET)
        parts.append(' ')
        if showfile and self.filename:
            parts.append(f'{self.filename}:{self.line}:')
        else:
            if self.patch_line:
                parts.append(f'#{self.patch_line}: ')
            if self.filename:
                parts.append(f'FILE: {self.filename}:{self.line}:')
        out = ''.join(parts).rstrip()
        if terse:
            return f'{out} {self.message.splitlines()[0]}'
        text = f'{out} {self.message}'
        if self.context and not terse:
            text += f'\n{self.context}'
        return text


@dataclass
class FileContext:
    filename: str
    lines: list[ParsedLine] = field(default_factory=list)
    by_real: dict[int, ParsedLine] = field(default_factory=dict)

    def add(self, line: ParsedLine) -> None:
        self.lines.append(line)
        if line.real > 0 and line.kind in ('added', 'context'):
            self.by_real[line.real] = line

    def prev(self, line: ParsedLine) -> Optional[ParsedLine]:
        return self.by_real.get(line.real - 1) if line.real > 0 else None

    def next(self, line: ParsedLine) -> Optional[ParsedLine]:
        return self.by_real.get(line.real + 1) if line.real > 0 else None


# ---------------------------------------------------------------------------
# New: C statement block extraction
# ---------------------------------------------------------------------------

def _sanitize(line: str) -> str:
    """Replace comments and string/char literals with placeholders."""
    s = RE_BLOCK.sub(lambda m: ' ' * len(m.group(0)), line)
    s = re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), s)
    s = re.sub(STRING, lambda m: '"' + 'X' * (len(m.group(0)) - 2) + '"', s)
    s = re.sub(CHAR, lambda m: "'" + 'X' * (len(m.group(0)) - 2) + "'", s)
    return s


def statement_block(lines: list[str], start_idx: int,
                    start_off: int = 0):
    """Extract a complete C statement starting at (idx, off).

    Tracks parenthesis and brace depth; terminates on `;` at level 0
    or an unbalanced `}`.  Returns (text, end_idx, end_off, level)
    where level < 0 signals an unbalanced closing brace was reached.
    """
    depth_p = depth_b = 0
    parts: list[str] = []
    for i in range(start_idx, len(lines)):
        raw = lines[i]
        s = _sanitize(raw)
        off = start_off if i == start_idx else 0
        while off < len(s):
            c = s[off]
            if c == '(':
                depth_p += 1
            elif c == ')':
                depth_p = max(0, depth_p - 1)
            elif c == '{':
                depth_b += 1
            elif c == '}':
                depth_b -= 1
                if depth_b < 0:
                    return ''.join(parts) + raw[:off + 1], i, off, -1
            elif c == ';' and depth_p == 0 and depth_b == 0:
                return ''.join(parts) + raw[:off + 1], i, off, 0
            off += 1
        parts.append(raw + '\n')
    return ''.join(parts), len(lines) - 1, 0, 0


def _is_simple_macro_body(body: str) -> bool:
    """True if the macro body is a single simple expression."""
    b = body.strip()
    if not b:
        return True
    if re.fullmatch(r'\w+', b):
        return True
    if re.fullmatch(r'-?\d+\w*', b):
        return True
    if re.fullmatch(STRING, b):
        return True
    if b.startswith('(') and b.rstrip().endswith(')'):
        return True
    if b.startswith('{') and b.rstrip().endswith('}'):
        return True
    if re.match(r'do\s*\{.*\}\s*while\s*\(\s*0\s*\)', b, re.DOTALL):
        return True
    if re.fullmatch(rf'{IDENT}\s*\(.*\)', b, re.DOTALL):
        return True
    return False


# ---------------------------------------------------------------------------
# Fixer
# ---------------------------------------------------------------------------

class Fixer:
    def __init__(self) -> None:
        self.mods: dict[int, str] = {}
        self.deletes: set[int] = set()
        self.inserts: dict[int, list[str]] = defaultdict(list)

    def modify(self, idx, raw): self.mods[idx] = raw
    def current(self, idx, default): return self.mods.get(idx, default)
    def delete(self, idx): self.deletes.add(idx)
    def insert_before(self, idx, raw): self.inserts[idx].append(raw)
    def has_changes(self): return bool(self.mods or self.deletes or self.inserts)

    def apply(self, lines):
        out = []
        for i, line in enumerate(lines):
            for ins in self.inserts.get(i, []):
                out.append(ins)
            if i in self.deletes:
                continue
            out.append(self.mods.get(i, line.raw))
        for ins in self.inserts.get(len(lines), []):
            out.append(ins)
        return out


def recompute_hunk_counts(lines):
    result = list(lines)
    hunk_idxs = [i for i, ln in enumerate(result) if ln.startswith('@@ ')]
    for k, hi in enumerate(hunk_idxs):
        end = hunk_idxs[k + 1] if k + 1 < len(hunk_idxs) else len(result)
        plus = minus = ctx = 0
        for j in range(hi + 1, end):
            ln = result[j]
            if ln.startswith(('diff --git', '--- ', '+++ ')):
                break
            if ln.startswith('+'):
                plus += 1
            elif ln.startswith('-'):
                minus += 1
            elif ln.startswith(' ') or ln == '':
                ctx += 1
        m = RE_HUNK_HDR.match(result[hi])
        if m:
            result[hi] = (f'@@ -{m.group(1)},{minus + ctx} '
                          f'+{m.group(3)},{plus + ctx} @@{m.group(5)}')
    return result


# ---------------------------------------------------------------------------
# Patch parser
# ---------------------------------------------------------------------------

class PatchParser:
    def __init__(self):
        self.lines: list[ParsedLine] = []
        self.commit_author = ''
        self.commit_subject = ''
        self.commit_body: list[str] = []
        self.has_patch = False
        self.has_separator = False
        self.new_file_mode = ''
        self.file_changed = False

    def _add(self, pl, kind, raw, **kw):
        self.lines.append(ParsedLine(
            idx=len(self.lines), kind=kind, raw=raw,
            patch_linenr=pl, **kw))

    def parse(self, data):
        raw_lines = data.splitlines()
        seen = False
        in_hunk = False
        cur_file = ''
        real = 0

        for i, raw in enumerate(raw_lines):
            pl = i + 1
            if not seen:
                if raw.startswith('diff --git '):
                    seen = True
                    self.has_patch = True
                    m = re.search(r'\bb/(\S+)$', raw)
                    if m:
                        cur_file = m.group(1)
                    self._add(pl, 'diff-meta', raw)
                    continue
                if (raw.startswith('--- ')
                        and i + 1 < len(raw_lines)
                        and raw_lines[i + 1].startswith('+++ ')):
                    seen = True
                    self.has_patch = True
                    self._add(pl, 'file-old', raw)
                    continue
                self._absorb_commit(raw)
                self._add(pl, 'commit', raw)
                continue

            if raw.startswith('diff --git '):
                m = re.search(r'\bb/(\S+)$', raw)
                if m:
                    cur_file = m.group(1)
                self._add(pl, 'diff-meta', raw)
                in_hunk = False
                continue
            if raw.startswith('new file mode '):
                self.new_file_mode = raw.split()[-1]
                self.file_changed = True
                self._add(pl, 'diff-meta', raw)
                continue
            if raw.startswith(('deleted file ', 'rename ', 'similarity ',
                               'dissimilarity ', 'old mode ', 'new mode ',
                               'index ')):
                if raw.startswith(('deleted file', 'rename ')):
                    self.file_changed = True
                self._add(pl, 'diff-meta', raw)
                continue
            if raw.startswith('--- '):
                self._add(pl, 'file-old', raw)
                continue
            if raw.startswith('+++ '):
                m = re.match(r'^\+\+\+\s+(?:b/)?(\S+)', raw)
                if m and m.group(1) != '/dev/null':
                    cur_file = m.group(1)
                self._add(pl, 'file-new', raw)
                continue
            if raw.startswith('@@ '):
                m = re.match(r'^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@', raw)
                if m:
                    real = int(m.group(1))
                in_hunk = True
                self._add(pl, 'hunk', raw)
                continue
            if not in_hunk:
                self._add(pl, 'other', raw)
                continue

            if raw.startswith('+'):
                self._add(pl, 'added', raw, text=raw[1:], real=real,
                          filename=cur_file)
                real += 1
            elif raw.startswith('-'):
                self._add(pl, 'removed', raw, text=raw[1:], real=real,
                          filename=cur_file)
            elif raw.startswith(' '):
                self._add(pl, 'context', raw, text=raw[1:], real=real,
                          filename=cur_file)
                real += 1
            elif raw.startswith('\\'):
                self._add(pl, 'no-newline', raw, real=real,
                          filename=cur_file)
            elif raw == '':
                self._add(pl, 'blank', raw, real=real, filename=cur_file)
                real += 1
            else:
                self._add(pl, 'other', raw)

    def _absorb_commit(self, raw):
        if raw.strip() == '---':
            self.has_separator = True
            return
        if self.has_separator:
            return
        m = RE_SUBJECT.match(raw)
        if m:
            self.commit_subject = m.group(1)
            return
        m = RE_FROM.match(raw)
        if m:
            self.commit_author = m.group(1)
            return
        if raw.startswith(('From ', 'Date:', 'commit ')):
            return
        if raw.strip():
            self.commit_body.append(raw)


# ---------------------------------------------------------------------------
# Config loaders
# ---------------------------------------------------------------------------

def _load_dict_file(path, sep):
    out = {}
    if not path or not os.path.isfile(path):
        return out
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith('#') or sep not in line:
                    continue
                k, v = line.split(sep, 1)
                out[k.strip().lower()] = v.strip()
    except OSError:
        pass
    return out


def load_spelling_file(path):
    fixes = _load_dict_file(path, '||')
    fixes.update(_load_dict_file(path, '->'))
    return fixes


def load_word_set(path):
    words = set()
    if not path or not os.path.isfile(path):
        return words
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith('#'):
                    words.add(line)
    except OSError:
        pass
    return words


# ---------------------------------------------------------------------------
# Checker
# ---------------------------------------------------------------------------

class Checker:
    def __init__(self, opts):
        self.opts = opts
        self.opts.signoff = not getattr(opts, "no_signoff", False)
        self.opts.fixes_tag = not getattr(opts, "no_fixes_tag", False)
        self.findings: list[Finding] = []
        self.counts = [0, 0, 0]
        self.lines_checked = 0
        self.fixer: Optional[Fixer] = None

        self.types_ignored = set()
        self.types_wanted = set()
        for t in opts.ignore or ():
            for x in t.split(','):
                if x.strip():
                    self.types_ignored.add(x.strip().upper())
        for t in opts.types or ():
            for x in t.split(','):
                if x.strip():
                    self.types_wanted.add(x.strip().upper())

        self.fix_enabled = bool(opts.fix or opts.fix_inplace)

        self.spelling = dict(COMMON_TYPOS)
        if opts.spelling_file:
            self.spelling.update(load_spelling_file(opts.spelling_file))
        if opts.codespell:
            self.spelling.update(load_spelling_file(
                opts.codespellfile or CODESPELL_DEFAULT))

        self.const_structs = set()
        cf = opts.const_structs_file or CONST_STRUCTS_FILE
        if os.path.isfile(cf):
            self.const_structs = load_word_set(cf)

        self.extra_typedefs = set()
        if opts.typedefsfile:
            self.extra_typedefs = load_word_set(opts.typedefsfile)

        self._dt_compat_cache = None
        self._dt_root = opts.root or '.'
        self._last_is_binding: Optional[bool] = None

    # ---- Reporting -------------------------------------------------

    def _want(self, msg_type):
        t = msg_type.upper()
        if self.types_wanted:
            return t in self.types_wanted
        return t not in self.types_ignored

    def _emit(self, level, msg_type, msg, *, line=0, patch_line=0,
              fname='', ctx=''):
        if not self._want(msg_type):
            return False
        if self.opts.test_only and self.opts.test_only not in msg:
            return False
        self.findings.append(Finding(level, msg_type, msg, line=line,
                                     patch_line=patch_line,
                                     filename=fname, context=ctx))
        self.counts[level] += 1
        return True

    def error(self, t, m, **kw): return self._emit(ERROR, t, m, **kw)
    def warn(self, t, m, **kw): return self._emit(WARN, t, m, **kw)
    def check(self, t, m, **kw): return self._emit(CHECK, t, m, **kw)

    def _fix(self, idx, raw, transform):
        if not self.fixer:
            return
        base = self.fixer.mods.get(idx, raw)
        self.fixer.mods[idx] = transform(base)

    # ---- Entry -----------------------------------------------------

    def run(self, path, data):
        self.fixer = Fixer() if self.fix_enabled else None
        if self.opts.file:
            self._check_plain_source(path, data)
        else:
            self._check_patch(path, data)

    def _check_patch(self, path, data):
        parser = PatchParser()
        parser.parse(data)
        if not parser.has_patch:
            self.error('NOT_UNIFIED_DIFF',
                       'Does not appear to be a unified-diff format patch',
                       fname=path)
            return

        if parser.new_file_mode in ('100755', '100775', '100777') \
                and not self._is_script_file(parser.lines):
            self.error('EXECUTE_PERMISSIONS',
                       'do not set execute permissions for source files',
                       fname=path)

        files = defaultdict(list)
        for line in parser.lines:
            if line.filename:
                files[line.filename].append(line)

        if parser.file_changed and 'MAINTAINERS' not in files:
            self.warn('FILE_PATH_CHANGES',
                      'added, moved or deleted file(s), does MAINTAINERS '
                      'need updating?', fname=path)

        for fname, flines in files.items():
            ctx = FileContext(fname)
            for line in flines:
                ctx.add(line)
            self._run_file_checks(ctx)

            is_binding = self._is_dt_binding(fname)
            if self._last_is_binding is not None \
                    and self._last_is_binding != is_binding:
                self.warn('DT_SPLIT_BINDING_PATCH',
                          'DT binding docs and includes should be a '
                          'separate patch', fname=path)
            self._last_is_binding = is_binding

        self._check_commit(parser, path)
        self._apply_patch_fixes(path, parser)

    @staticmethod
    def _is_script_file(lines):
        for line in lines:
            if line.kind == 'file-new' and line.raw.startswith('+++ '):
                name = line.raw.split()[-1]
                return name.endswith(('.py', '.pl', '.sh', '.awk'))
        return False

    @staticmethod
    def _is_dt_binding(fname):
        return ('/devicetree/bindings/' in fname
                or fname.startswith('Documentation/devicetree/')
                or '/dt-bindings/' in fname
                or fname.startswith('include/dt-bindings/'))

    def _check_plain_source(self, path, data):
        raw_lines = data.splitlines()
        ctx = FileContext(path)
        for i, raw in enumerate(raw_lines):
            ctx.add(ParsedLine(idx=i, kind='added', raw=raw, text=raw,
                               real=i + 1, patch_linenr=i + 1,
                               filename=path))
        head = '\n'.join(raw_lines[:5])
        if not RE_SPDX.search(head):
            self.warn('SPDX_LICENSE_TAG',
                      'Missing SPDX-License-Identifier tag',
                      line=1, fname=path)
        self._run_file_checks(ctx)
        if data and not data.endswith('\n'):
            self.warn('MISSING_EOF_NEWLINE',
                      'missing newline at end of file',
                      line=len(raw_lines), fname=path)
        self._apply_source_fixes(path, raw_lines)

    # ---- Orchestration --------------------------------------------

    def _run_file_checks(self, ctx):
        for line in ctx.lines:
            if line.kind not in ('added', 'context') and not self.opts.file:
                continue
            self._check_source_line(line, ctx)
            if line.kind == 'added' or self.opts.file:
                self.lines_checked += 1

        self._check_line_relationships(ctx)
        self._check_macros(ctx)
        self._check_line_continuations(ctx)
        self._check_split_strings(ctx)
        self._check_structure(ctx)       # NEW: braces, indent, parens
        self._check_function_args(ctx)   # NEW: FUNCTION_WITHOUT_ARGS
        self._check_trailing_statements(ctx)  # NEW

        if 'Kconfig' in ctx.filename:
            self._check_kconfig(ctx)
        if ctx.filename.endswith('MAINTAINERS'):
            self._check_maintainers(ctx)
        if self._is_dt_binding(ctx.filename) or ctx.filename.endswith(
                ('.dts', '.dtsi', '.dtso')):
            self._check_dt(ctx)

    # ---- Fix application ------------------------------------------

    def _apply_patch_fixes(self, path, parser):
        if not self.fix_enabled or not self.fixer \
                or not self.fixer.has_changes():
            return
        self._write_fixed_output(
            path, recompute_hunk_counts(self.fixer.apply(parser.lines)))

    def _apply_source_fixes(self, path, raw_lines):
        if not self.fix_enabled or not self.fixer \
                or not self.fixer.has_changes():
            return
        plines = [ParsedLine(idx=i, kind='added', raw=raw, text=raw)
                  for i, raw in enumerate(raw_lines)]
        self._write_fixed_output(path, self.fixer.apply(plines))

    def _write_fixed_output(self, path, lines):
        if path == '-':
            sys.stdout.write('\n'.join(lines))
            if lines:
                sys.stdout.write('\n')
            return
        outpath = path if self.opts.fix_inplace else path + '.checkpatch-fixes'
        try:
            with open(outpath, 'w', encoding='utf-8') as f:
                for ln in lines:
                    f.write(ln + '\n')
        except OSError as exc:
            print(f'{path}: cannot write {outpath}: {exc}',
                  file=sys.stderr)
            return
        if not self.opts.quiet:
            print(f'\nWrote corrected output to {outpath}')
            print('Please inspect the output before submitting.')

    # ---- Commit ---------------------------------------------------

    def _check_commit(self, parser, path):
        subject, body = parser.commit_subject, parser.commit_body
        if not subject.strip():
            self.error('MISSING_SUBJECT', 'Missing patch subject',
                       fname=path)
        elif len(subject) > MAX_SUBJECT:
            self.warn('LONG_SUBJECT',
                      f'Subject is {len(subject)} characters, '
                      f'prefer <= {MAX_SUBJECT}', fname=path)
        for line in body:
            if not line.strip():
                continue
            if line.startswith(('diff --git', '+++ ', 'index ')):
                continue
            if len(line) > MAX_COMMIT_BODY_LINE and not RE_URL.search(line):
                self.warn('COMMIT_LOG_LONG_LINE',
                          f'commit body line is {len(line)} chars, '
                          f'prefer <= {MAX_COMMIT_BODY_LINE}', fname=path)
            if line.startswith('#'):
                self.warn('COMMIT_COMMENT_SYMBOL',
                          "commit log line starts with '#'", fname=path)
            if re.search(r'\bgerrit\b', line, re.I) \
                    and re.search(r'\bchange-id\b', line, re.I):
                self.error('GERRIT_CHANGE_ID',
                           "Remove Gerrit Change-Id's before submitting "
                           "upstream", fname=path)
            if re.search(r'(?:SUBJECT|BLURB) HERE', line):
                self.error('PLACEHOLDER_USE',
                           'Placeholder text detected', fname=path)
        self._check_commit_signatures(parser, path)
        self._check_commit_fixes(body, path)
        self._check_commit_links(body, path)
        if self.opts.signoff and not any(RE_SOB.match(l) for l in body):
            self.error('MISSING_SIGN_OFF',
                       'Missing Signed-off-by: line(s)', fname=path)
        if self.opts.fixes_tag:
            marker = self._commit_needs_fixes(body)
            if marker and not any(RE_FIXES.match(l) for l in body):
                self.warn('MISSING_FIXES_TAG',
                          f"Commit message has '{marker}', maybe it "
                          f"also needs a 'Fixes:' tag?", fname=path)

    def _check_commit_signatures(self, parser, path):
        seen, author_sob = {}, False
        for idx, line in enumerate(parser.commit_body):
            m = RE_TAG.match(line)
            if not m:
                continue
            tag, val = m.group(1), m.group(2).strip()
            tag_lc = tag.lower()
            if tag_lc not in SIG_TAGS:
                best = min(SIG_TAGS, key=lambda k: _edit_distance(tag_lc, k))
                if _edit_distance(tag_lc, best) <= 2:
                    self.warn('BAD_SIGN_OFF',
                              f"Non-standard signature '{tag}' - "
                              f"perhaps '{SIG_TAGS[best]}'?", fname=path)
                elif tag_lc.endswith('-by'):
                    self.warn('BAD_SIGN_OFF',
                              f'Non-standard signature: {tag}', fname=path)
                continue
            canonical = SIG_TAGS[tag_lc]
            if tag + ':' != canonical:
                self.warn('BAD_SIGN_OFF',
                          f"Use '{canonical}' not '{tag}:'", fname=path)
            nxt = (parser.commit_body[idx + 1]
                   if idx + 1 < len(parser.commit_body) else '')
            if tag_lc == 'co-developed-by' and not RE_SOB.match(nxt):
                self.warn('BAD_SIGN_OFF',
                          'Co-developed-by: must be immediately '
                          'followed by Signed-off-by:', fname=path)
            if tag_lc in ('reported-by', 'reported-and-tested-by') \
                    and not RE_LINK.match(nxt):
                self.warn('BAD_REPORTED_BY_LINK',
                          'Reported-by: should be immediately followed '
                          'by Closes: or Link: with a URL', fname=path)
            key = f'{tag_lc}:{val.lower()}'
            if key in seen:
                self.warn('BAD_SIGN_OFF',
                          f'Duplicate signature: {line.strip()}', fname=path)
            seen[key] = True
            if tag_lc == 'signed-off-by' and parser.commit_author \
                    and _same_email(val, parser.commit_author):
                author_sob = True
        if self.opts.signoff and parser.commit_author and not author_sob:
            self.error('NO_AUTHOR_SIGN_OFF',
                       f"Missing Signed-off-by: line by nominal patch "
                       f"author '{parser.commit_author}'", fname=path)

    def _check_commit_fixes(self, body, path):
        for line in body:
            m = RE_FIXES.match(line)
            if not m:
                continue
            sha1, title = m.group(1), m.group(2)
            issues = []
            if len(sha1) < MIN_SHA1_LEN:
                issues.append(f'SHA1 is {len(sha1)} chars')
            if sha1.lower() != sha1:
                issues.append('SHA1 must be lowercase')
            if title is None:
                issues.append('missing ("<subject>") after SHA1')
            if issues:
                self.warn('BAD_FIXES_TAG',
                          'Fixes: tag issues: ' + '; '.join(issues),
                          fname=path)

    def _check_commit_links(self, body, path):
        for line in body:
            m = RE_LINK.match(line)
            if m and not m.group(2).startswith(('http://', 'https://')):
                self.warn('COMMIT_LOG_WRONG_LINK',
                          f'{m.group(1)} should be followed by an '
                          f'http(s) URL', fname=path)
            for host in OBSOLETE_ARCHIVES:
                if host in line:
                    self.warn('PREFER_LORE_ARCHIVE',
                              'Use lore.kernel.org archive links when '
                              'possible', fname=path)
                    break

    @staticmethod
    def _commit_needs_fixes(body):
        text = '\n'.join(body)
        for marker in ('BUG:', 'KASAN:', 'KMSAN:', 'Call Trace:',
                       'syzkaller', 'stable@'):
            if marker in text:
                return marker
        return ''

    # ---- Per-line checks ------------------------------------------

    def _check_source_line(self, line, ctx):
        kw = dict(line=line.real, patch_line=line.patch_linenr,
                  fname=line.filename, ctx=line.raw)
        idx = line.idx
        text = line.text
        raw = line.raw

        if line.kind == 'context':
            return

        if '\r' in text:
            self.error('DOS_LINE_ENDINGS', 'DOS line endings', **kw)
            if self.fixer:
                self.fixer.modify(idx, raw.replace('\r', ''))
            text = text.replace('\r', '')

        if RE_TRAIL_WS.search(text):
            self.error('TRAILING_WHITESPACE', 'trailing whitespace', **kw)
            if self.fixer:
                self._fix(idx, raw, lambda s: s.rstrip())

        self._check_indent(text, raw, idx, kw)
        self._check_length(text, kw)

        code = self._strip_comments(text)
        code = re.sub(STRING, '""', code)
        code = re.sub(CHAR, "''", code)
        stripped = code.strip()
        if not stripped:
            return

        if RE_C99.search(code):
            self.error('C99_COMMENTS', 'do not use C99 // comments', **kw)
            if self.fixer:
                m = re.match(r'^(.*?)//\s*(.*?)\s*$', raw)
                if m:
                    self.fixer.modify(idx, f'{m.group(1)}/* {m.group(2)} */')

        if RE_FALLTHRU.search(text):
            self.warn('PREFER_FALLTHROUGH',
                      "Prefer 'fallthrough;' over fallthrough comment", **kw)

        self._check_spacing(stripped, raw, idx, kw)
        self._check_pointer_location(stripped, raw, idx, kw)
        self._check_braces(stripped, raw, idx, kw)
        self._check_parens(stripped, **kw)
        self._check_control_flow(stripped, **kw)
        self._check_kernel_api(stripped, raw, idx, kw)
        self._check_misc(stripped, raw, idx, kw)
        self._check_spdx_line(stripped, line, **kw)
        self._check_content(stripped, **kw)

    def _check_indent(self, text, raw, idx, kw):
        head = text.lstrip()
        if head.startswith(('*', '/*', '//', '#')):
            return
        if re.match(r'^ +\t', text):
            self.error('CODE_INDENT', 'space before tab in indent', **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(r'^ +\t', '\t', raw))
            return
        m = re.match(r'^( +)\S', text)
        if m:
            spaces = len(m.group(1))
            self.error('CODE_INDENT', 'code indent should use tabs', **kw)
            if self.fixer:
                tabs = max(1, spaces // DEFAULT_TABSIZE)
                rem = spaces % DEFAULT_TABSIZE if spaces >= DEFAULT_TABSIZE else 0
                prefix = '\t' * tabs + ' ' * rem
                self._fix(idx, raw, lambda s: ('+' if s.startswith('+') else '') + prefix + s.lstrip('+').lstrip().rstrip())
            return
        if re.search(r' \t', text):
            self.warn('SPACE_BEFORE_TAB',
                      'please, no space before tabs', **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(r' +\t', '\t', raw))
        if re.match(r'^\t{6,}', raw) and not head.startswith('#'):
            self.warn('DEEP_INDENTATION',
                      'Too many leading tabs - consider code refactoring',
                      **kw)

    def _check_length(self, text, kw):
        expanded = text.expandtabs(self.opts.tab_size)
        if len(expanded) <= self.opts.max_line_length:
            return
        if self._long_line_ok(text):
            return
        self.warn('LONG_LINE',
                  f'line length {len(expanded)} exceeds '
                  f'{self.opts.max_line_length}', **kw)

    @staticmethod
    def _long_line_ok(text):
        s = text.strip()
        if RE_URL.search(text):
            return True
        if re.match(r'^(?:pr_|dev_|netdev_|printk|WARN|panic)', s):
            return True
        if re.match(r'^#\s*define\s+\w+\s+"', s):
            return True
        if s.startswith('"') and s.rstrip(',;)]').endswith('"'):
            return True
        return False

    def _check_spacing(self, code, raw, idx, kw):
        for kw_name in ('if', 'for', 'while', 'switch'):
            if re.search(rf'\b{kw_name}\(', code):
                self.error('SPACING',
                           f"space required after '{kw_name}' before '('",
                           **kw)
                if self.fixer:
                    self._fix(idx, raw, lambda s, _k=kw_name: re.sub(
                        rf'\b{_k}\(', f'{_k} (', s))
                break
        m = re.search(rf'\b({IDENT})\s+\(', code)
        if m:
            name = m.group(1)
            ok = {'if', 'for', 'while', 'switch', 'return',
                  'sizeof', 'typeof', '__typeof__', '__attribute__',
                  'asm', '__asm__'}
            if (name not in ok
                    and not code.lstrip().startswith('#')):
                self.error('SPACING',
                           f"space prohibited between '{name}' and '('",
                           **kw)
                if self.fixer:
                    self._fix(idx, raw, lambda s, _n=name: re.sub(
                        rf'\b{_n}\s+\(', f'{_n}(', s))
        for m in re.finditer(r',(\S)', code):
            prev = code[m.start() - 1] if m.start() > 0 else ''
            if m.group(1) in ')]}>;':
                continue
            if prev.isdigit() and m.group(1).isdigit():
                continue
            self.error('SPACING', 'space required after that comma', **kw)
            if self.fixer:
                self._fix(idx, raw, lambda s: re.sub(r',(\S)', r', \1', s))
            break
        for op in ('==', '!=', '<=', '>=', '&&', '||', '<<', '>>'):
            if re.search(rf'\w{re.escape(op)}\w', code):
                self.error('SPACING',
                           f"spaces required around '{op}'", **kw)
                if self.fixer:
                    self._fix(idx, raw, lambda s, _o=op: re.sub(
                        rf'(\w){re.escape(_o)}(\w)',
                        rf'\1 {_o} \2', s))
                break

        # Simple binary arithmetic: x+1, a*b (conservative)
        for arith_op in ('+', '*', '/', '%'):
            if re.search(rf'[A-Za-z0-9_\)\]]{re.escape(arith_op)}[A-Za-z0-9_\(]', code):
                self.error('SPACING',
                           f"spaces required around '{arith_op}'", **kw)
                if self.fixer:
                    self._fix(idx, raw, lambda s, _o=arith_op: re.sub(
                        rf'([A-Za-z0-9_\)\]]){re.escape(_o)}([A-Za-z0-9_\(])',
                        rf'\1 {_o} \2', s))
                break
        if re.search(r'\s+->|->\s+', code):
            self.error('SPACING', "spaces prohibited around '->'", **kw)
            if self.fixer:
                self._fix(idx, raw, lambda s: re.sub(r'\s*->\s*', '->', s))

    def _check_pointer_location(self, code, raw, idx, kw):
        if re.search(rf'\b(?:{TYPE_BASE})\s+\*\s*{IDENT}', code):
            self.error('POINTER_LOCATION',
                       'pointer should be attached to the identifier, '
                       'not the type', **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    rf'(\b{TYPE_BASE})\s+\*\s*({IDENT})', r'\1 *\2', raw))

    def _check_braces(self, code, raw, idx, kw):
        if re.search(rf'^{TYPE}\s+{IDENT}\s*\([^)]*\)\s*\{{', code) \
                and not code.lstrip().startswith('#'):
            self.error('OPEN_BRACE',
                       "function opening brace '{' should be on its "
                       f"own line", **kw)
        if re.search(r'(?:\)|\bdo|\belse)\{', code):
            self.error('SPACING',
                       "space required before the open brace '{'", **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    r'(\)|\bdo|\belse)\{', r'\1 {', raw))

    def _check_parens(self, code, **kw):
        m = re.match(r'^return\s*\(([^()]*)\)\s*;', code)
        if m and re.match(r'^[\w.>\[\]\->]+\s*$', m.group(1)):
            self.error('RETURN_PARENTHESES',
                       'return is not a function, parentheses are not '
                       'required', **kw)
        if re.search(r'\bif\s*\(\s*\(', code):
            self.check('UNNECESSARY_PARENTHESES',
                       'unnecessary parentheses around if condition', **kw)
        if re.search(r'\(\s+\w', code) and 'for (' not in code:
            self.error('SPACING', "space prohibited after '('", **kw)
        if re.search(r'\w\s+\)', code):
            self.error('SPACING', "space prohibited before ')'", **kw)

    def _check_control_flow(self, code, **kw):
        if re.search(r'\}\s*else\s*\{', code):
            self.error('ELSE_AFTER_BRACE',
                       "else should follow close brace '}'", **kw)
        m = re.search(r'\breturn\s*\(?\s*(E[A-Z]+)\b', code)
        if m and m.group(1) not in ('EOF', 'ERROR') \
                and not m.group(1).startswith('EPOLL'):
            self.warn('USE_NEGATIVE_ERRNO',
                      f'return of an errno should typically be negative '
                      f'(return -{m.group(1)})', **kw)
        if re.search(r'\bif\s*\([^)]*=[^=]', code) \
                and not re.search(r'==|!=|<=|>=', code):
            self.error('ASSIGNMENT_IN_IF',
                       'do not use assignment in if condition', **kw)
        if re.search(r'\bif\s*\(\s*(\w+)\s*==\s*(true|false|TRUE|FALSE)\b',
                     code):
            self.check('BOOL_COMPARISON',
                       'Using comparison to true/false is error prone', **kw)

    def _check_kernel_api(self, code, raw, idx, kw):
        if re.search(r'\bprintk\s*\(\s*(?!KERN_)', code):
            self.warn('PRINTK_WITHOUT_KERN_LEVEL',
                      'printk() should include KERN_<LEVEL>', **kw)
        m = re.search(r'\bprintk(?:_once|_ratelimited)?\s*\(\s*'
                      r'KERN_([A-Z]+)', code)
        if m:
            lvl = m.group(1).lower()
            if lvl == 'warning':
                lvl = 'warn'
            self.warn('PREFER_PR_LEVEL',
                      f'Prefer pr_{lvl}(...) over printk(KERN_{m.group(1)}'
                      f'...)', **kw)
        m = re.search(r'\bdev_printk\s*\(\s*KERN_([A-Z]+)', code)
        if m:
            lvl = m.group(1).lower()
            if lvl == 'warning':
                lvl = 'warn'
            self.warn('PREFER_DEV_LEVEL',
                      f'Prefer dev_{lvl}(...) over dev_printk(KERN_'
                      f'{m.group(1)}...)', **kw)
        if re.search(r'\bprintk_ratelimit\s*\(', code):
            self.warn('PRINTK_RATELIMITED',
                      'Prefer printk_ratelimited or pr_<level>_ratelimited',
                      **kw)
        if re.search(r'\b(?:pr|dev|netdev)_\w+\s*\(.*KERN_[A-Z]+', code):
            self.warn('UNNECESSARY_KERN_LEVEL',
                      'Possible unnecessary KERN_<LEVEL> in pr_*()', **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    r'\s*KERN_[A-Z]+\s*', '', raw))
        if re.search(r'(?<!AA_)(?<!BUILD_)(?<!IDA_)(?<!KVM_)'
                     r'\bBUG(?:_ON)?\s*\(', code):
            self.warn('AVOID_BUG',
                      'Do not crash the kernel; use WARN_ON_ONCE() plus '
                      'recovery code instead of BUG()', **kw)
        m = RE_DEPRECATED.search(code)
        if m:
            old = m.group(1)
            repl = DEPRECATED_APIS.get(old, '')
            self.warn('DEPRECATED_API',
                      f"Deprecated use of '{old}'"
                      + (f", prefer '{repl}' instead" if repl else ''), **kw)
        for fn, repl in (('strcpy', 'strscpy'), ('strncpy', 'strscpy'),
                         ('strlcpy', 'strscpy')):
            if re.search(rf'\b{fn}\s*\(', code):
                self.warn(fn.upper(), f'Prefer {repl} over {fn}', **kw)
        m = re.search(r'\budelay\s*\(\s*(\d+)\s*\)', code)
        if m:
            val = int(m.group(1))
            if val >= 10:
                self.warn('USLEEP_RANGE',
                          'usleep_range is preferred over udelay', **kw)
            if val > 2000:
                self.warn('LONG_UDELAY',
                          'long udelay - prefer mdelay', **kw)
        m = re.search(r'\bmsleep\s*\(\s*(\d+)\s*\)', code)
        if m and int(m.group(1)) < 20:
            self.warn('MSLEEP',
                      'msleep < 20ms can sleep up to 20ms; use '
                      'usleep_range', **kw)
        if 'typedef' in code \
                and not re.search(rf'\btypedef\s+{TYPE}\s*\(\s*\*\s*{IDENT}'
                                  rf'\s*\)', code) \
                and not re.search(r'\b__bitwise\b', code) \
                and not re.search(rf'\btypedef\s+{TYPE}\s+{IDENT}\s*\(',
                                  code):
            self.warn('NEW_TYPEDEFS', 'do not add new typedefs', **kw)
        if re.search(r'\bvolatile\b', code) \
                and not re.search(r'\b(?:asm|__asm__)\s+'
                                  r'(?:volatile|__volatile__)', code):
            self.warn('VOLATILE',
                      'Use of volatile is usually wrong: see '
                      'Documentation/process/volatile-considered-'
                      'harmful.rst', **kw)
        if re.search(r'\b__inline__?\b', code):
            self.warn('INLINE', 'plain "inline" is preferred', **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    r'\b__inline__?\b', 'inline', raw))
        m = re.search(r'\b(unsigned|signed)\s+(?!char\b|short\b|int\b'
                      r'|long\b)[A-Za-z_]\w*', code)
        if m:
            self.warn('UNSPECIFIED_INT',
                      f"Prefer '{m.group(1)} int' over bare "
                      f"'{m.group(1)}'", **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    r'\b(unsigned|signed)\s+(?!char\b|short\b|int\b'
                    r'|long\b)', r'\1 int ', raw))
        if re.search(r'\bjiffies\s*(?:[<>]=?|==|!=)', code) \
                or re.search(r'(?:[<>]=?|==|!=)\s*jiffies\b', code):
            self.warn('JIFFIES_COMPARISON',
                      'Comparing jiffies is almost always wrong; use '
                      'time_after/time_before', **kw)
        m = re.search(rf'sizeof\s*\(\s*({IDENT})\s*\)\s*/\s*'
                      rf'sizeof\s*\(\s*\1\s*\[\s*0\s*\]\s*\)', code)
        if m:
            self.warn('ARRAY_SIZE', f'Prefer ARRAY_SIZE({m.group(1)})',
                      **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    rf'sizeof\s*\(\s*{m.group(1)}\s*\)\s*/\s*'
                    rf'sizeof\s*\(\s*{m.group(1)}\s*\[\s*0\s*\]\s*\)',
                    f'ARRAY_SIZE({m.group(1)})', raw))
        m = re.search(rf'\b(\w+)\s*=\s*\w*alloc\s*\(\s*'
                      rf'sizeof\s*\(\s*struct\s+(\w+)\s*\)', code)
        if m:
            self.check('ALLOC_SIZEOF_STRUCT',
                       f'Prefer sizeof(*{m.group(1)}) over '
                       f'sizeof(struct {m.group(2)})', **kw)
        m = re.search(r'\b(\w+)\s*==\s*NULL\b', code)
        if m:
            self.check('COMPARISON_TO_NULL',
                       f'Comparison to NULL could be written '
                       f'"!{m.group(1)}"', **kw)
        m = re.search(r'\b(\w+)\s*!=\s*NULL\b', code)
        if m:
            self.check('COMPARISON_TO_NULL',
                       f'Comparison to NULL could be written '
                       f'"{m.group(1)}"', **kw)
        if re.search(r'\b\w+\s*=\s*\w+\s*=\s*[^=]', code):
            self.check('MULTIPLE_ASSIGNMENTS',
                       'multiple assignments should be avoided', **kw)
        if RE_LINUX_V.search(code):
            self.warn('LINUX_VERSION_CODE',
                      'LINUX_VERSION_CODE should be avoided', **kw)
        if RE_CVS_KW.search(raw):
            self.warn('CVS_KEYWORD',
                      'CVS style keyword markers will _not_ be updated',
                      **kw)
        if re.search(r'\b__FUNCTION__\b', code):
            self.warn('USE_FUNC',
                      '__func__ should be used instead of __FUNCTION__',
                      **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    r'\b__FUNCTION__\b', '__func__', raw))
        if re.search(r'\byield\s*\(\s*\)', code):
            self.warn('YIELD',
                      'Using yield() is generally wrong', **kw)
        m = re.search(rf'\bseq_printf\s*\(\s*\w+\s*,\s*({STRING})\s*\)',
                      code)
        if m and '%' not in m.group(1):
            self.warn('PREFER_SEQ_PUTS',
                      'Prefer seq_puts to seq_printf', **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    r'\bseq_printf\b', 'seq_puts', raw))
        if re.search(r'\bENOTSUPP\b', code):
            self.warn('ENOTSUPP',
                      'ENOTSUPP is not a SUSV4 error code, prefer '
                      'EOPNOTSUPP', **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    r'\bENOTSUPP\b', 'EOPNOTSUPP', raw))
        if re.search(r'\b(?:simple|strict)_strto[lul]+\s*\(', code):
            self.warn('CONSIDER_KSTRTO',
                      'simple_strto* and strict_strto* are obsolete, '
                      'use kstrto* instead', **kw)
        if re.search(r'\b__DATE__\b|\b__TIME__\b|\b__TIMESTAMP__\b', code):
            self.warn('DATE_TIME',
                      'Use of __DATE__/__TIME__/__TIMESTAMP__ makes the '
                      'build non-deterministic', **kw)
        if re.search(r'\bin_atomic\s*\(', code):
            self.warn('IN_ATOMIC',
                      'use of in_atomic() is incorrect outside core '
                      'kernel code', **kw)
        if re.search(r'\bNR_CPUS\b', code) \
                and not re.search(r'^#\s*(?:if|define)\b', code) \
                and not re.search(r'\[[^\]]*NR_CPUS[^\]]*\]', code):
            self.warn('NR_CPUS',
                      'usage of NR_CPUS is often wrong', **kw)
        if re.search(r'\b(?:un)?likely\s*\([^)]*\)\s*(?:[<>]=?|==|!=)',
                     code):
            self.warn('LIKELY_MISUSE',
                      'Using likely/unlikely should generally have '
                      'parentheses around the comparison', **kw)
        if re.search(r'\bsizeof\s*\(\s*&', code):
            self.warn('SIZEOF_ADDRESS',
                      'sizeof(& should be avoided', **kw)
        m = re.search(r'\bsizeof\s+([A-Za-z_]\w*)', code)
        if m and m.group(1) not in ('int', 'char', 'long', 'short'):
            self.warn('SIZEOF_PARENTHESIS',
                      f'sizeof {m.group(1)} should be '
                      f'sizeof({m.group(1)})', **kw)
            if self.fixer:
                self.fixer.modify(idx, re.sub(
                    r'\bsizeof\s+([A-Za-z_]\w*)', r'sizeof(\1)', raw))
        m = re.search(r'\(\s*(int|long|short|unsigned|signed)\s*\)\s*'
                      r'(\d+)', code)
        if m:
            self.warn('TYPECAST_INT_CONSTANT',
                      f'Unnecessary typecast of int constant '
                      f'({m.group(1)}){m.group(2)}', **kw)
        for struct in self.const_structs:
            if re.search(rf'\bstruct\s+{re.escape(struct)}\b', code) \
                    and 'const' not in code:
                self.warn('CONST_STRUCT',
                          f'struct {struct} should normally be const',
                          **kw)
                break
        if 'define' in code:
            m = re.search(r'1\s*<<\s*(\w+)', code)
            if m and not re.search(r'\bBIT\s*\(', code):
                self.warn('BIT_MACRO',
                          f'Prefer using the BIT() macro for 1 << '
                          f'{m.group(1)}', **kw)
                if self.fixer:
                    self.fixer.modify(idx, re.sub(
                        r'\(?\s*1\s*<<\s*(\w+)\s*\)?', r'BIT(\1)', raw))
        m = re.search(r'\bIS_ENABLED\s*\(\s*(\w+)\s*\)', code)
        if m and not m.group(1).startswith('CONFIG_'):
            self.warn('IS_ENABLED_CONFIG',
                      f'IS_ENABLED({m.group(1)}) is normally used as '
                      f'IS_ENABLED(CONFIG_{m.group(1)})', **kw)
        m = re.search(r'#\s*if\s+defined\s*\(?\s*(CONFIG_\w+)\s*\)?\s*\|\|'
                      r'\s*defined\s*\(?\s*\1_MODULE\s*\)?', code)
        if m:
            self.warn('PREFER_IS_ENABLED',
                      f'Prefer IS_ENABLED({m.group(1)}) over '
                      f'defined({m.group(1)}) || defined('
                      f'{m.group(1)}_MODULE)', **kw)
        if WORLD_WRITABLE_RE.search(code):
            if re.search(r'\b(debugfs_create_|DEVICE_ATTR|CLASS_ATTR|'
                         r'proc_create|__ATTR)\w*\s*\(', code):
                self.warn('EXPORTED_WORLD_WRITABLE',
                          'Exporting world writable files is usually an '
                          'error. Consider more restrictive permissions.',
                          **kw)
        if re.search(r'\(\s*(?:void|char)\s*\*\s*\)\s*\w+alloc', code):
            self.warn('UNNECESSARY_CASTS',
                      'unnecessary cast of alloc result may hide bugs',
                      **kw)

    # ---- NEW: miscellaneous structural checks ---------------------

    def _check_misc(self, code, raw, idx, kw):
        # IF_0 / IF_1
        if re.match(r'^#\s*if\s+0\b', code):
            self.warn('IF_0',
                      'Consider removing #if 0 and its #endif', **kw)
        if re.match(r'^#\s*if\s+1\b', code):
            self.warn('IF_1',
                      'Consider removing #if 1 and its #endif', **kw)
        # MISORDERED_TYPE
        for bad in ('char unsigned', 'short int unsigned',
                    'long int unsigned', 'long long int unsigned',
                    'int short unsigned', 'int long unsigned'):
            if re.search(rf'\b{bad}\b', code):
                self.warn('MISORDERED_TYPE',
                          f"type '{bad}' should be specified in "
                          f"[[un]signed] [short|int|long|long long] order",
                          **kw)
                break
        # RETURN_VOID in single-line void function
        if re.search(r'\bvoid\s+\w+\s*\([^)]*\)\s*\{[^}]*\breturn\s*;',
                     code, re.DOTALL):
            self.warn('RETURN_VOID',
                      'void function return statements are not generally '
                      'useful', **kw)
        # CONST_CONST
        m = re.search(r'\bconst\s+(\w+)\s+const\b', code)
        if m:
            self.warn('CONST_CONST',
                      f"'const {m.group(1)} const' should probably be "
                      f"'const {m.group(1)}'", **kw)
        # STORAGE_CLASS after type
        m = re.search(rf'\b({TYPE_BASE})\s+(static|extern)\b', code)
        if m:
            self.warn('STORAGE_CLASS',
                      f"storage class '{m.group(2)}' should be located "
                      f"before type '{m.group(1)}'", **kw)
        # INLINE_LOCATION
        if re.search(rf'\b{TYPE_BASE}\s+inline\b', code) \
                or re.search(r'\binline\s+(?:static|extern)\b', code):
            self.error('INLINE_LOCATION',
                       'inline keyword should sit between storage class '
                       'and type', **kw)
        # HEXADECIMAL_BOOLEAN_TEST
        if re.search(r'(?:&&|\|\||[&|])\s*0[xX][0-9a-fA-F]+', code) \
                and not re.search(r'\b[A-Za-z_]\w*\s*&\s*0[xX]', code):
            self.warn('HEXADECIMAL_BOOLEAN_TEST',
                      'boolean test with hexadecimal, perhaps just 1 & '
                      'or |?', **kw)
        # UAPI_INCLUDE
        m = re.search(r'#\s*include\s+"uapi/([^"]+)"', code)
        if m:
            self.error('UAPI_INCLUDE',
                       'No #include in ...uapi/... should use a uapi/ '
                       'path prefix', **kw)

    def _check_spdx_line(self, code, parsed, **kw):
        m = RE_SPDX.search(parsed.text)
        if not m:
            return
        license_id = m.group(1).strip()
        if not re.match(r'^(?:GPL-2\.0(?:-only|-or-later)?|'
                        r'LGPL-2\.1(?:-only|-or-later)?|'
                        r'BSD-2-Clause|BSD-3-Clause|MIT|ISC|Apache-2\.0|'
                        r'Dual\s+MIT/GPL|Dual\s+BSD/GPL|Dual\s+MPL/GPL|'
                        r'GPL-2\.0-only\s+OR\s+BSD-2-Clause|'
                        r'\(GPL-2\.0-only\s+OR\s+BSD-2-Clause\)|'
                        r'GPL-2\.0\s+and\s+additional\s+rights)', license_id):
            self.warn('SPDX_LICENSE_TAG',
                      f"'{license_id}' is not a recognized SPDX license "
                      f"identifier", **kw)

    def _check_content(self, code, **kw):
        fname = kw.get('fname', '')
        skip_camel = any(s in fname for s in (
            'Kconfig', 'MAINTAINERS', 'Documentation/', '.rst', '.txt'))
        if not skip_camel:
            for m in re.finditer(rf'\b{IDENT}\b', code):
                word = m.group(0)
                if word in CAMELCASE_EXCEPTIONS or word in self.extra_typedefs:
                    continue
                if word.isupper() or word.islower():
                    continue
                # Real CamelCase needs an INTERNAL case transition:
                #   fooBar    -> [a-z][A-Z] inside
                #   HTTPServer -> [A-Z]{2,}[a-z] at start
                # Single leading capital (Short, John) = Title Case, skip.
                if not re.search(r'[a-z][A-Z]', word) \
                        and not re.match(r'^[A-Z]{2,}[a-z]', word):
                    continue
                self.check('CAMELCASE', f'Avoid CamelCase: <{word}>', **kw)
                break
        for m in re.finditer(r'\b[A-Za-z]{3,}\b', code):
            w = m.group(0).lower()
            if w in self.spelling:
                self.warn('TYPO_SPELLING',
                          f"'{m.group(0)}' may be misspelled - perhaps "
                          f"'{self.spelling[w]}'?", **kw)
                break

    @staticmethod
    def _strip_comments(line):
        return RE_BLOCK.sub('', line)

    # ---- Line relationships ---------------------------------------

    def _check_line_relationships(self, ctx):
        for line in ctx.lines:
            if line.kind not in ('added', 'context') and not self.opts.file:
                continue
            text = line.text if line.text else line.raw
            prev = ctx.prev(line)
            kw = dict(line=line.real, patch_line=line.patch_linenr,
                      fname=ctx.filename, ctx=line.raw)
            if re.match(r'^\s*=\s*[^=]', text):
                self.check('ASSIGNMENT_CONTINUATIONS',
                           'Assignment operator should be on the previous '
                           'line', **kw)
                if self.fixer and prev and prev.raw.startswith('+'):
                    self.fixer.modify(line.idx, re.sub(
                        r'^\s*=\s*', '', line.raw))
                    self.fixer.modify(prev.idx, prev.raw.rstrip() + ' =')
            if re.match(r'^\s*(?:&&|\|\|)', text):
                self.check('LOGICAL_CONTINUATIONS',
                           'Logical continuations should be on the '
                           'previous line', **kw)
            if re.match(r'^\s*(?:\}\s*)?else\b', text) and prev:
                prev_text = prev.text if prev.text else prev.raw
                if re.search(r'\b(?:return|break|continue|goto)\b.*;\s*$',
                             prev_text):
                    self.warn('UNNECESSARY_ELSE',
                              'else is not generally useful after a '
                              'return or break', **kw)
            if re.match(r'^\s*break\s*;', text) and prev:
                prev_text = prev.text if prev.text else prev.raw
                if re.match(r'^\s*(?:return|break|goto|continue)\b',
                            prev_text):
                    self.warn('UNNECESSARY_BREAK',
                              'break is not useful after a return/'
                              'break/goto', **kw)
            # MULTILINE_DEREFERENCE
            if prev:
                prev_text = prev.text if prev.text else prev.raw
                if re.search(r'(?:\.|->)\s*$', prev_text) \
                        and not re.match(r'^\s*(?:if|for|while|switch)\b',
                                         text):
                    self.warn('MULTILINE_DEREFERENCE',
                              'Avoid multiple line dereference', **kw)
            # LINE_SPACING
            if prev:
                prev_text = prev.text if prev.text else prev.raw
                if re.search(rf'^\s*{TYPE}\s+\w+\s*[=;]', prev_text) \
                        and re.match(rf'^\s*{TYPE}\s+\w+\s*[=;]', text):
                    pass
                elif re.search(rf'^\s*{TYPE}\s+\w+\s*[=;]', prev_text) \
                        and not re.match(r'^\s*(?:$|\}|\{|return|if|for|'
                                         rf'while|switch)', text):
                    self.check('LINE_SPACING',
                               'Missing a blank line after declarations',
                               **kw)
            # EMBEDDED_FILENAME
            if ctx.filename and ctx.filename in text:
                self.warn('EMBEDDED_FILENAME',
                          "It's generally not useful to have the filename "
                          "in the file", **kw)

    def _check_line_continuations(self, ctx):
        for line in ctx.lines:
            if line.kind != 'added' and not self.opts.file:
                continue
            text = line.text if line.text else line.raw
            if not text.rstrip().endswith('\\'):
                continue
            if re.match(r'^\s*#', text):
                continue
            if re.search(r'\b(?:asm|__asm__)\b', text):
                continue
            self.warn('LINE_CONTINUATIONS',
                      'Avoid unnecessary line continuations',
                      line=line.real, patch_line=line.patch_linenr,
                      fname=ctx.filename, ctx=line.raw)

    def _check_split_strings(self, ctx):
        prev = None
        for line in ctx.lines:
            if line.kind not in ('added', 'context') and not self.opts.file:
                prev = None
                continue
            text = line.text if line.text else line.raw
            if prev:
                prev_text = prev.text if prev.text else prev.raw
                if re.search(r'"\s*$', prev_text) \
                        and re.match(r'^\s*"', text):
                    self.warn('SPLIT_STRING',
                              'quoted string split across lines',
                              line=line.real,
                              patch_line=line.patch_linenr,
                              fname=ctx.filename,
                              ctx=f'{prev.raw}\n{line.raw}')
            prev = line

    # ---- NEW: structural checks -----------------------------------

    def _check_structure(self, ctx):
        """BRACES, SUSPECT_CODE_INDENT, PARENTHESIS_ALIGNMENT."""
        for line in ctx.lines:
            if line.kind not in ('added', 'context') and not self.opts.file:
                continue
            text = line.text if line.text else line.raw
            kw = dict(line=line.real, patch_line=line.patch_linenr,
                      fname=ctx.filename, ctx=line.raw)

            # BRACES: single-statement body wrapped in braces
            m = re.match(r'^(\s*)(?:if|for|while)\b.*\{\s*$', text)
            if m:
                stmt = ctx.next(line)
                if stmt:
                    stmt_text = stmt.text if stmt.text else stmt.raw
                    close = ctx.next(stmt)
                    if close:
                        close_text = close.text if close.text else close.raw
                        if (stmt_text.strip() and
                                not stmt_text.strip().startswith(('{', '}')) and
                                re.match(r'^\s*\}\s*$', close_text) and
                                stmt_text.count(';') == 1):
                            self.warn('BRACES',
                                      'braces {} are not necessary for '
                                      'single statement blocks', **kw)

            # SUSPECT_CODE_INDENT
            m = re.match(r'^(\t*)(?:if|for|while|switch)\s*\(', text)
            if m and '{' not in text:
                indent = len(m.group(1))
                nxt = ctx.next(line)
                if nxt:
                    nxt_text = nxt.text if nxt.text else nxt.raw
                    if nxt_text.strip() and \
                            not nxt_text.lstrip().startswith('{'):
                        nxt_tabs = len(nxt_text) - len(nxt_text.lstrip('\t'))
                        if nxt_tabs != indent + 1:
                            self.warn('SUSPECT_CODE_INDENT',
                                      f'suspect code indent for conditional '
                                      f'statements ({indent}, {nxt_tabs})',
                                      **kw)

            # PARENTHESIS_ALIGNMENT
            if text.rstrip().endswith('('):
                pos = text.rfind('(')
                col = len(text[:pos].expandtabs(DEFAULT_TABSIZE)) + 1
                nxt = ctx.next(line)
                if nxt:
                    nxt_text = nxt.text if nxt.text else nxt.raw
                    if nxt_text.strip():
                        lead = len(nxt_text) - len(nxt_text.lstrip())
                        ncol = len(nxt_text[:lead].expandtabs(DEFAULT_TABSIZE))
                        if ncol not in (col, col - 1):
                            self.check('PARENTHESIS_ALIGNMENT',
                                       'Alignment should match open '
                                       'parenthesis', **kw)

    def _check_function_args(self, ctx):
        """FUNCTION_WITHOUT_ARGS: int foo() should be int foo(void)."""
        for line in ctx.lines:
            if line.kind != 'added' and not self.opts.file:
                continue
            text = line.text if line.text else line.raw
            if text.lstrip().startswith('#'):
                continue
            m = re.search(rf'^(?:{TYPE}|{TYPE_BASE}\s+{TYPE_BASE})'
                          rf'\s+({IDENT})\s*\(\s*\)', text)
            if m and not re.search(r'\b(?:return|sizeof)\b', text[:m.start()]):
                kw = dict(line=line.real, patch_line=line.patch_linenr,
                          fname=ctx.filename, ctx=line.raw)
                self.error('FUNCTION_WITHOUT_ARGS',
                           f'Bad function definition - {m.group(1)}() '
                           f'should probably be {m.group(1)}(void)', **kw)

    def _check_trailing_statements(self, ctx):
        """TRAILING_STATEMENTS: if (cond) stmt; should be on its own line."""
        for line in ctx.lines:
            if line.kind != 'added' and not self.opts.file:
                continue
            text = line.text if line.text else line.raw
            if re.match(r'^\s*#', text):
                continue
            if re.match(r'^\s*(?:if|while|for)\s*\([^)]*\)\s*'
                        r'(?!\s*(?:\{|;|\\|$))\S', text):
                kw = dict(line=line.real, patch_line=line.patch_linenr,
                          fname=ctx.filename, ctx=line.raw)
                self.error('TRAILING_STATEMENTS',
                           'trailing statements should be on next line',
                           **kw)
            # case ...: stmt;
            if re.match(r'^\s*(?:case\s*[^:]+|default)\s*:\s*(?!\{)\S',
                        text) and not re.search(r'\breturn\b', text):
                kw = dict(line=line.real, patch_line=line.patch_linenr,
                          fname=ctx.filename, ctx=line.raw)
                self.error('TRAILING_STATEMENTS',
                           'trailing statements should be on next line',
                           **kw)

    # ---- Macros ---------------------------------------------------

    def _check_macros(self, ctx):
        lines = ctx.lines
        i = 0
        while i < len(lines):
            line = lines[i]
            if line.kind != 'added' and not self.opts.file:
                i += 1
                continue
            text = line.text if line.text else line.raw
            m = RE_DEFINE.match(text)
            if not m:
                i += 1
                continue
            name = m.group(1)
            args_str = m.group(2) or ''
            body_parts = [m.group(3)]
            start = i
            cur = text
            while cur.rstrip().endswith('\\') and i + 1 < len(lines):
                i += 1
                nxt = lines[i]
                if nxt.kind not in ('added', 'context') \
                        and not self.opts.file:
                    break
                nxt_text = nxt.text if nxt.text else nxt.raw
                body_parts.append(nxt_text.rstrip('\\').rstrip())
                cur = nxt_text
            body = ' '.join(body_parts).strip()
            args = []
            if args_str:
                args = [a.strip() for a in args_str[1:-1].split(',')
                        if a.strip()]
            self._analyze_macro(name, args, body, lines[start], ctx)
            i += 1

    def _analyze_macro(self, name, args, body, line, ctx):
        kw = dict(line=line.real, patch_line=line.patch_linenr,
                  fname=ctx.filename, ctx=body)

        # TRAILING_SEMICOLON
        if body.rstrip().endswith(';'):
            if not re.search(r'do\s*\{.*\}\s*while\s*\(\s*0\s*\)\s*;',
                             body, re.DOTALL):
                self.warn('TRAILING_SEMICOLON',
                          f'macro {name} should not use a trailing '
                          f'semicolon', **kw)

        has_do_while = bool(re.search(r'\bdo\s*\{', body))
        has_flow = bool(re.search(r'\b(?:goto|return|break|continue)\b', body))
        stmt_count = body.count(';')

        if has_flow and not has_do_while:
            self.warn('MULTISTATEMENT_MACRO_USE_DO_WHILE',
                      'Macros with flow control statements should be '
                      'avoided', **kw)
        elif not has_do_while and stmt_count > 1 \
                and not body.startswith(('{', '({')) \
                and not re.search(rf'^{TYPE}\b', body) \
                and not re.search(r'\b(?:struct|union|enum|typedef)\b', body):
            self.warn('MULTISTATEMENT_MACRO_USE_DO_WHILE',
                      'Non-declarative macros with multiple statements '
                      'should be enclosed in a do - while loop', **kw)

        if has_do_while:
            inner = re.search(r'do\s*\{(.*?)\}\s*while\s*\(\s*0\s*\)',
                              body, re.DOTALL)
            if inner:
                inner_body = inner.group(1).strip()
                if inner_body.count(';') == 1 \
                        and not re.match(r'^(?:if|for|while|switch)\b',
                                         inner_body):
                    self.warn('SINGLE_STATEMENT_DO_WHILE_MACRO',
                              'Single statement macros should not use a '
                              'do {} while (0) loop', **kw)

        # COMPLEX_MACRO
        if not _is_simple_macro_body(body) and not has_do_while:
            self.warn('COMPLEX_MACRO',
                      'Macros with complex values should be enclosed in '
                      'parentheses', **kw)

        # MACRO_ARG_* checks
        for arg in args:
            if '...' in arg or arg.lower() == 'type':
                continue
            # Skip if arg is only used inside sizeof/typeof/etc.
            test_body = re.sub(
                rf'\b(?:sizeof|typeof|__typeof__|__stringify|offsetof|'
                rf'typecheck)\s*\(\s*{re.escape(arg)}\b[^)]*\)',
                ' ', body)
            count = len(re.findall(rf'\b{re.escape(arg)}\b', test_body))
            if count == 0:
                # Maybe unused; verify against full body
                if not re.search(rf'\b{re.escape(arg)}\b', body):
                    self.warn('MACRO_ARG_UNUSED',
                              f"Argument '{arg}' is not used in "
                              f"function-like macro", **kw)
                continue
            if count > 1:
                self.check('MACRO_ARG_REUSE',
                           f"Macro argument reuse '{arg}' - possible "
                           f"side-effects?", **kw)
            # Precedence: arg used adjacent to an operator, unwrapped
            if not re.search(rf'\(\s*{re.escape(arg)}\s*\)', body) \
                    and re.search(
                        rf'(?:[<>=!&|+\-*/%^])\s*{re.escape(arg)}\b'
                        rf'|\b{re.escape(arg)}\s*(?:[<>=!&|+\-*/%^])',
                        test_body):
                self.check('MACRO_ARG_PRECEDENCE',
                           f"Macro argument '{arg}' may be better as "
                           f"'({arg})' to avoid precedence issues", **kw)

    # ---- Kconfig --------------------------------------------------

    def _check_kconfig(self, ctx):
        lines = ctx.lines
        for i, line in enumerate(lines):
            if line.kind != 'added' and not self.opts.file:
                continue
            text = line.text if line.text else line.raw
            if not RE_KCONFIG_SYM.match(text):
                continue
            needs_help = has_help = False
            help_len = 0
            j = i + 1
            while j < len(lines):
                sub = lines[j]
                if sub.kind not in ('added', 'context') and not self.opts.file:
                    break
                st = sub.text if sub.text else sub.raw
                if RE_KCONFIG.match(st):
                    break
                if RE_HELP_TRIG.match(st):
                    needs_help = True
                if RE_HELP_START.match(st):
                    has_help = True
                elif has_help and st.strip():
                    help_len += 1
                j += 1
            if needs_help and help_len < self.opts.min_conf_desc_length:
                self.warn('CONFIG_DESCRIPTION',
                          f'please write a help paragraph that fully '
                          f'describes the config symbol with at least '
                          f'{self.opts.min_conf_desc_length} lines',
                          line=line.real,
                          patch_line=line.patch_linenr,
                          fname=ctx.filename, ctx=line.raw)

    # ---- MAINTAINERS ---------------------------------------------

    def _check_maintainers(self, ctx):
        prev_tag = None
        for line in ctx.lines:
            if line.kind != 'added' and not self.opts.file:
                continue
            text = line.text if line.text else line.raw
            kw = dict(line=line.real, patch_line=line.patch_linenr,
                      fname=ctx.filename, ctx=line.raw)
            if not text.strip():
                prev_tag = None
                continue
            m = RE_MAINT_TAG.match(text)
            if not m:
                if not text.startswith((' ', '\t')):
                    prev_tag = None
                continue
            tag, val = m.group(1), m.group(2)
            if not re.match(rf'^{tag}:\t', text):
                self.warn('MAINTAINERS_STYLE',
                          f'MAINTAINERS entries use one tab after '
                          f'{tag}:', **kw)
                if self.fixer:
                    self.fixer.modify(line.idx, re.sub(
                        rf'^{tag}:\s*', f'{tag}:\t', line.raw))
            if prev_tag:
                ptag, pval = prev_tag
                if tag in RE_MAINT_ORDER and ptag in RE_MAINT_ORDER:
                    if RE_MAINT_ORDER.find(tag) < RE_MAINT_ORDER.find(ptag):
                        self.warn('MAINTAINERS_STYLE',
                                  f"Misordered MAINTAINERS entry - list "
                                  f"'{tag}:' before '{ptag}:'", **kw)
                    elif tag == ptag and tag in ('F', 'X') and pval > val:
                        self.warn('MAINTAINERS_STYLE',
                                  'Misordered MAINTAINERS entry - list '
                                  'patterns in alphabetic order', **kw)
                elif tag not in RE_MAINT_ORDER:
                    self.warn('MAINTAINERS_STYLE',
                              f"Unknown MAINTAINERS entry type: '{tag}'",
                              **kw)
            prev_tag = (tag, val)

    # ---- Device Tree ----------------------------------------------

    def _check_dt(self, ctx):
        if ctx.filename.startswith('Documentation/devicetree/bindings/') \
                and ctx.filename.endswith('.txt'):
            if any(ln.kind == 'diff-meta' and 'new file' in ln.raw
                   for ln in ctx.lines):
                self.warn('DT_SCHEMA_BINDING_PATCH',
                          'DT bindings should be in DT schema format. '
                          'See: Documentation/devicetree/bindings/'
                          'writing-schema.rst', fname=ctx.filename)
        if self.opts.no_tree:
            return
        for line in ctx.lines:
            if line.kind != 'added' and not self.opts.file:
                continue
            text = line.text if line.text else line.raw
            kw = dict(line=line.real, patch_line=line.patch_linenr,
                      fname=ctx.filename, ctx=line.raw)
            for m in RE_COMPAT.finditer(text):
                compat = m.group(1)
                if re.match(r'^(?:pciclass|pci[0-9a-f]{2,4}|'
                            r'usb(?:if)?[0-9a-f]{1,4}),', compat):
                    continue
                if not self._dt_compat_documented(compat):
                    self.warn('UNDOCUMENTED_DT_STRING',
                              f'DT compatible string "{compat}" appears '
                              f'un-documented', **kw)
            if re.match(r'^\s*phy-(?:mode|connection-type)\s*=\s*"'
                        r'rgmii(?:-rxid|-txid)?"', text):
                if not self._has_comment_before(ctx, line):
                    self.warn('UNCOMMENTED_RGMII_MODE',
                              'rgmii mode without comment', **kw)

    def _dt_compat_documented(self, compat):
        if self._dt_compat_cache is None:
            self._build_dt_cache()
        if self._dt_compat_cache is None:
            return True
        if compat in self._dt_compat_cache:
            return True
        m = re.match(r'^([^,]+),([^-]+)-', compat)
        if m:
            prefix = f'{m.group(1)},{m.group(2)}-'
            return any(k.startswith(prefix) for k in self._dt_compat_cache)
        return False

    def _build_dt_cache(self):
        bindings_dir = os.path.join(
            self._dt_root, 'Documentation/devicetree/bindings')
        if not os.path.isdir(bindings_dir):
            self._dt_compat_cache = None
            return
        compat = set()
        for root, _, files in os.walk(bindings_dir):
            for fname in files:
                if not fname.endswith(('.yaml', '.yml')):
                    continue
                try:
                    with open(os.path.join(root, fname), 'r',
                              encoding='utf-8', errors='replace') as fh:
                        data = fh.read()
                except OSError:
                    continue
                for m in re.finditer(r'-\s*["\']([a-zA-Z0-9,._-]+)["\']',
                                     data):
                    if ',' in m.group(1):
                        compat.add(m.group(1))
        self._dt_compat_cache = compat

    @staticmethod
    def _has_comment_before(ctx, line):
        cur = line
        for _ in range(3):
            cur = ctx.prev(cur)
            if cur is None:
                return False
            text = cur.text if cur.text else cur.raw
            if re.search(r'/\*.*?\*/|//', text):
                return True
            if re.match(r'^\s*$', text):
                continue
            break
        return False

    # ---- Summary --------------------------------------------------

    def summary(self):
        return (f'total: {self.counts[0]} errors, '
                f'{self.counts[1]} warnings, {self.counts[2]} checks, '
                f'{self.lines_checked} lines checked')


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _edit_distance(a, b):
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1,
                           prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def _parse_email(addr):
    m = re.match(r'^(.*?)\s*<(\S+@\S+)>', addr)
    if m:
        return m.group(1).strip().strip('"').strip(), m.group(2)
    m = re.match(r'(\S+@\S+)', addr)
    if m:
        return '', m.group(1)
    return '', addr.strip()


def _same_email(a, b):
    return _parse_email(a) == _parse_email(b)


def top_of_kernel_tree(root='.'):
    return all(os.path.exists(os.path.join(root, p)) for p in (
        'COPYING', 'CREDITS', 'Kbuild', 'MAINTAINERS', 'Makefile',
        'README', 'Documentation', 'arch', 'include', 'drivers', 'fs',
        'init', 'ipc', 'kernel', 'lib', 'scripts'))


def which_conf():
    env_dir = os.environ.get(ENV_CONFIG_DIR, '')
    if env_dir:
        conf = os.path.join(env_dir, '.checkpatch.conf')
        if os.path.isfile(conf):
            return conf
    for base in DEFAULT_CONF_DIRS:
        conf = os.path.join(base, '.checkpatch.conf')
        if os.path.isfile(conf):
            return conf
    return ''


def load_config_args(conf_path):
    if not conf_path:
        return []
    args = []
    try:
        with open(conf_path, 'r', encoding='utf-8') as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith('#'):
                    args.extend(line.split())
    except OSError:
        pass
    return args


def read_source(path):
    if path == '-':
        return sys.stdin.read()
    try:
        with open(path, 'r', encoding='utf-8', errors='replace',
                  newline='') as fh:
            return fh.read()
    except OSError as exc:
        print(f'{path}: cannot read: {exc}', file=sys.stderr)
        sys.exit(2)


def git_format_patch(rev):
    try:
        result = subprocess.run(
            ['git', 'format-patch', '-M', '--stdout', '-1', rev],
            capture_output=True, text=True, check=True)
        return result.stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f'git format-patch {rev} failed: {exc}', file=sys.stderr)
        return ''


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv):
    conf = which_conf()
    conf_args = load_config_args(conf)

    parser = ArgumentParser(
        description='Linux kernel patch/source style checker.',
        epilog='When FILE is "-", read standard input.')
    parser.add_argument('files', nargs='*', metavar='FILE')
    parser.add_argument('-f', '--file', action='store_true')
    parser.add_argument('--no-signoff', action='store_true')
    parser.add_argument('--no-fixes-tag', action='store_true')
    parser.add_argument('--no-tree', action='store_true')
    parser.add_argument('--strict', action='store_true')
    parser.add_argument('-q', '--quiet', action='store_true')
    parser.add_argument('--terse', action='store_true')
    parser.add_argument('--emacs', action='store_true')
    parser.add_argument('--show-types', action='store_true')
    parser.add_argument('--showfile', action='store_true')
    parser.add_argument('--list-types', action='store_true')
    parser.add_argument('--types', action='append', default=[],
                        metavar='TYPE[,TYPE2...]')
    parser.add_argument('--ignore', action='append', default=[],
                        metavar='TYPE[,TYPE2...]')
    parser.add_argument('--test-only', metavar='WORD')
    parser.add_argument('--max-line-length', type=int, default=DEFAULT_MAX_LINE)
    parser.add_argument('--min-conf-desc-length', type=int,
                        default=MIN_CONF_DESC_LEN)
    parser.add_argument('--tab-size', type=int, default=DEFAULT_TABSIZE)
    parser.add_argument('--color', default='auto',
                        choices=('auto', 'always', 'never'))
    parser.add_argument('--summary', dest='summary',
                        action='store_true', default=True)
    parser.add_argument('--no-summary', dest='summary', action='store_false')
    parser.add_argument('--mailback', action='store_true')
    parser.add_argument('--fix', action='store_true')
    parser.add_argument('--fix-inplace', action='store_true')
    parser.add_argument('-g', '--git', action='store_true')
    parser.add_argument('--root', metavar='PATH')
    parser.add_argument('--codespell', action='store_true')
    parser.add_argument('--codespellfile', metavar='FILE')
    parser.add_argument('--spelling-file', metavar='FILE')
    parser.add_argument('--typedefsfile', metavar='FILE')
    parser.add_argument('--const-structs-file', metavar='FILE')
    parser.add_argument('--version', action='version',
                        version=f'%(prog)s {VERSION}')

    args = parser.parse_args(conf_args + list(argv))
    if not args.files and not args.list_types:
        parser.error("the following arguments are required: FILE")
    if args.git and (args.file or args.fix or args.fix_inplace):
        parser.error('--git cannot be used with --file or --fix')
    return args


def main(argv):
    opts = parse_args(argv)
    if opts.list_types:
        for t in KNOWN_TYPES:
            print(t)
        return 0
    if not opts.no_tree:
        if opts.root:
            if not top_of_kernel_tree(opts.root):
                print(f'{opts.root}: not a kernel tree', file=sys.stderr)
                return 2
        elif not top_of_kernel_tree('.') and not opts.file:
            opts.no_tree = True
    use_color = (opts.color == 'always'
                 or (opts.color == 'auto' and sys.stdout.isatty()))
    checker = Checker(opts)
    for spec in opts.files:
        data = git_format_patch(spec) if opts.git else read_source(spec)
        if not data:
            continue
        checker.run(spec, data)
    if opts.mailback and checker.counts[0] == 0 and checker.counts[1] == 0:
        return 0
    if not opts.quiet:
        for f in checker.findings:
            print(f.render(show_types=opts.show_types, color=use_color,
                           terse=opts.terse, showfile=opts.showfile))
    if opts.summary:
        print(checker.summary())
    if checker.counts[0]:
        if not opts.quiet:
            print('\nNOTE: If any of the reported defects are false '
                  'positives, please report them to the maintainer.')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
