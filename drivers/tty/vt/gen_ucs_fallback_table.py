#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Leverage Python's unicodedata module to generate ucs_fallback_table.h
#
# The generated table maps complex characters to their simpler fallback forms
# for a terminal display when corresponding glyphs are unavailable.
#
# Usage:
#   python3 gen_ucs_fallback_table.py         # Generate fallback tables
#   python3 gen_ucs_fallback_table.py -o FILE # Specify output file

import unicodedata
import sys
import argparse
from collections import defaultdict

# This script's file name
from pathlib import Path
this_file = Path(__file__).name

# Default output file name
DEFAULT_OUT_FILE = "ucs_fallback_table.h"

def collect_accented_latin_letters():
    """Collect already composed Latin letters with diacritics."""
    fallback_map = {}

    # Latin-1 Supplement (0x00C0-0x00FF)
    # Capital letters with accents to their base forms
    fallback_map[0x00C0] = ord('A')  # À LATIN CAPITAL LETTER A WITH GRAVE
    fallback_map[0x00C1] = ord('A')  # Á LATIN CAPITAL LETTER A WITH ACUTE
    fallback_map[0x00C2] = ord('A')  # Â LATIN CAPITAL LETTER A WITH CIRCUMFLEX
    fallback_map[0x00C3] = ord('A')  # Ã LATIN CAPITAL LETTER A WITH TILDE
    fallback_map[0x00C4] = ord('A')  # Ä LATIN CAPITAL LETTER A WITH DIAERESIS
    fallback_map[0x00C5] = ord('A')  # Å LATIN CAPITAL LETTER A WITH RING ABOVE
    fallback_map[0x00C7] = ord('C')  # Ç LATIN CAPITAL LETTER C WITH CEDILLA
    fallback_map[0x00C8] = ord('E')  # È LATIN CAPITAL LETTER E WITH GRAVE
    fallback_map[0x00C9] = ord('E')  # É LATIN CAPITAL LETTER E WITH ACUTE
    fallback_map[0x00CA] = ord('E')  # Ê LATIN CAPITAL LETTER E WITH CIRCUMFLEX
    fallback_map[0x00CB] = ord('E')  # Ë LATIN CAPITAL LETTER E WITH DIAERESIS
    fallback_map[0x00CC] = ord('I')  # Ì LATIN CAPITAL LETTER I WITH GRAVE
    fallback_map[0x00CD] = ord('I')  # Í LATIN CAPITAL LETTER I WITH ACUTE
    fallback_map[0x00CE] = ord('I')  # Î LATIN CAPITAL LETTER I WITH CIRCUMFLEX
    fallback_map[0x00CF] = ord('I')  # Ï LATIN CAPITAL LETTER I WITH DIAERESIS
    fallback_map[0x00D1] = ord('N')  # Ñ LATIN CAPITAL LETTER N WITH TILDE
    fallback_map[0x00D2] = ord('O')  # Ò LATIN CAPITAL LETTER O WITH GRAVE
    fallback_map[0x00D3] = ord('O')  # Ó LATIN CAPITAL LETTER O WITH ACUTE
    fallback_map[0x00D4] = ord('O')  # Ô LATIN CAPITAL LETTER O WITH CIRCUMFLEX
    fallback_map[0x00D5] = ord('O')  # Õ LATIN CAPITAL LETTER O WITH TILDE
    fallback_map[0x00D6] = ord('O')  # Ö LATIN CAPITAL LETTER O WITH DIAERESIS
    fallback_map[0x00D9] = ord('U')  # Ù LATIN CAPITAL LETTER U WITH GRAVE
    fallback_map[0x00DA] = ord('U')  # Ú LATIN CAPITAL LETTER U WITH ACUTE
    fallback_map[0x00DB] = ord('U')  # Û LATIN CAPITAL LETTER U WITH CIRCUMFLEX
    fallback_map[0x00DC] = ord('U')  # Ü LATIN CAPITAL LETTER U WITH DIAERESIS
    fallback_map[0x00DD] = ord('Y')  # Ý LATIN CAPITAL LETTER Y WITH ACUTE

    # Lowercase letters with accents to their base forms
    fallback_map[0x00E0] = ord('a')  # à LATIN SMALL LETTER A WITH GRAVE
    fallback_map[0x00E1] = ord('a')  # á LATIN SMALL LETTER A WITH ACUTE
    fallback_map[0x00E2] = ord('a')  # â LATIN SMALL LETTER A WITH CIRCUMFLEX
    fallback_map[0x00E3] = ord('a')  # ã LATIN SMALL LETTER A WITH TILDE
    fallback_map[0x00E4] = ord('a')  # ä LATIN SMALL LETTER A WITH DIAERESIS
    fallback_map[0x00E5] = ord('a')  # å LATIN SMALL LETTER A WITH RING ABOVE
    fallback_map[0x00E7] = ord('c')  # ç LATIN SMALL LETTER C WITH CEDILLA
    fallback_map[0x00E8] = ord('e')  # è LATIN SMALL LETTER E WITH GRAVE
    fallback_map[0x00E9] = ord('e')  # é LATIN SMALL LETTER E WITH ACUTE
    fallback_map[0x00EA] = ord('e')  # ê LATIN SMALL LETTER E WITH CIRCUMFLEX
    fallback_map[0x00EB] = ord('e')  # ë LATIN SMALL LETTER E WITH DIAERESIS
    fallback_map[0x00EC] = ord('i')  # ì LATIN SMALL LETTER I WITH GRAVE
    fallback_map[0x00ED] = ord('i')  # í LATIN SMALL LETTER I WITH ACUTE
    fallback_map[0x00EE] = ord('i')  # î LATIN SMALL LETTER I WITH CIRCUMFLEX
    fallback_map[0x00EF] = ord('i')  # ï LATIN SMALL LETTER I WITH DIAERESIS
    fallback_map[0x00F1] = ord('n')  # ñ LATIN SMALL LETTER N WITH TILDE
    fallback_map[0x00F2] = ord('o')  # ò LATIN SMALL LETTER O WITH GRAVE
    fallback_map[0x00F3] = ord('o')  # ó LATIN SMALL LETTER O WITH ACUTE
    fallback_map[0x00F4] = ord('o')  # ô LATIN SMALL LETTER O WITH CIRCUMFLEX
    fallback_map[0x00F5] = ord('o')  # õ LATIN SMALL LETTER O WITH TILDE
    fallback_map[0x00F6] = ord('o')  # ö LATIN SMALL LETTER O WITH DIAERESIS
    fallback_map[0x00F9] = ord('u')  # ù LATIN SMALL LETTER U WITH GRAVE
    fallback_map[0x00FA] = ord('u')  # ú LATIN SMALL LETTER U WITH ACUTE
    fallback_map[0x00FB] = ord('u')  # û LATIN SMALL LETTER U WITH CIRCUMFLEX
    fallback_map[0x00FC] = ord('u')  # ü LATIN SMALL LETTER U WITH DIAERESIS
    fallback_map[0x00FD] = ord('y')  # ý LATIN SMALL LETTER Y WITH ACUTE
    fallback_map[0x00FF] = ord('y')  # ÿ LATIN SMALL LETTER Y WITH DIAERESIS

    # Special letters
    fallback_map[0x00D0] = ord('D')  # Ð LATIN CAPITAL LETTER ETH
    fallback_map[0x00F0] = ord('d')  # ð LATIN SMALL LETTER ETH
    fallback_map[0x00DE] = ord('P')  # Þ LATIN CAPITAL LETTER THORN
    fallback_map[0x00FE] = ord('p')  # þ LATIN SMALL LETTER THORN

    # Ligatures to component letters
    fallback_map[0x00C6] = ord('E')  # Æ LATIN CAPITAL LETTER AE -> E (could also be 'AE')
    fallback_map[0x00E6] = ord('e')  # æ LATIN SMALL LETTER AE -> e (could also be 'ae')
    fallback_map[0x0152] = ord('E')  # Œ LATIN CAPITAL LIGATURE OE -> E
    fallback_map[0x0153] = ord('e')  # œ LATIN SMALL LIGATURE OE -> e
    fallback_map[0x00DF] = ord('s')  # ß LATIN SMALL LETTER SHARP S -> s

    # These could also be handled by decomposition, but including for completeness
    fallback_map[0x00D8] = ord('O')  # Ø LATIN CAPITAL LETTER O WITH STROKE -> O
    fallback_map[0x00F8] = ord('o')  # ø LATIN SMALL LETTER O WITH STROKE -> o

    # Space variants - map all to regular ASCII space
    fallback_map[0x00A0] = ord(' ')  # NO-BREAK SPACE
    fallback_map[0x1680] = ord(' ')  # OGHAM SPACE MARK

    # Various space widths (EN QUAD through HAIR SPACE)
    for cp in range(0x2000, 0x200A+1):
        fallback_map[cp] = ord(' ')

    fallback_map[0x202F] = ord(' ')  # NARROW NO-BREAK SPACE
    fallback_map[0x205F] = ord(' ')  # MEDIUM MATHEMATICAL SPACE

    # Extended Latin
    fallback_map[0x0141] = ord('L')  # Ł LATIN CAPITAL LETTER L WITH STROKE -> L
    fallback_map[0x0142] = ord('l')  # ł LATIN SMALL LETTER L WITH STROKE -> l

    # Additional characters with cedilla and similar marks
    fallback_map[0x0122] = ord('G')  # Ģ LATIN CAPITAL LETTER G WITH CEDILLA -> G
    fallback_map[0x0123] = ord('g')  # ģ LATIN SMALL LETTER G WITH CEDILLA -> g
    fallback_map[0x0136] = ord('K')  # Ķ LATIN CAPITAL LETTER K WITH CEDILLA -> K
    fallback_map[0x0137] = ord('k')  # ķ LATIN SMALL LETTER K WITH CEDILLA -> k
    fallback_map[0x0145] = ord('N')  # Ņ LATIN CAPITAL LETTER N WITH CEDILLA -> N
    fallback_map[0x0146] = ord('n')  # ņ LATIN SMALL LETTER N WITH CEDILLA -> n
    fallback_map[0x0156] = ord('R')  # Ŗ LATIN CAPITAL LETTER R WITH CEDILLA -> R
    fallback_map[0x0157] = ord('r')  # ŗ LATIN SMALL LETTER R WITH CEDILLA -> r
    fallback_map[0x015E] = ord('S')  # Ş LATIN CAPITAL LETTER S WITH CEDILLA -> S
    fallback_map[0x015F] = ord('s')  # ş LATIN SMALL LETTER S WITH CEDILLA -> s
    fallback_map[0x0162] = ord('T')  # Ţ LATIN CAPITAL LETTER T WITH CEDILLA -> T
    fallback_map[0x0163] = ord('t')  # ţ LATIN SMALL LETTER T WITH CEDILLA -> t

    # Additional Romanian and Turkish specific letters with cedilla/comma below
    fallback_map[0x0218] = ord('S')  # Ș LATIN CAPITAL LETTER S WITH COMMA BELOW -> S
    fallback_map[0x0219] = ord('s')  # ș LATIN SMALL LETTER S WITH COMMA BELOW -> s
    fallback_map[0x021A] = ord('T')  # Ț LATIN CAPITAL LETTER T WITH COMMA BELOW -> T
    fallback_map[0x021B] = ord('t')  # ț LATIN SMALL LETTER T WITH COMMA BELOW -> t

    # Letters with caron/háček (Czech, Slovak, Slovenian, Croatian, Lithuanian, Latvian)
    fallback_map[0x010C] = ord('C')  # Č LATIN CAPITAL LETTER C WITH CARON -> C
    fallback_map[0x010D] = ord('c')  # č LATIN SMALL LETTER C WITH CARON -> c
    fallback_map[0x010E] = ord('D')  # Ď LATIN CAPITAL LETTER D WITH CARON -> D
    fallback_map[0x010F] = ord('d')  # ď LATIN SMALL LETTER D WITH CARON -> d
    fallback_map[0x011A] = ord('E')  # Ě LATIN CAPITAL LETTER E WITH CARON -> E
    fallback_map[0x011B] = ord('e')  # ě LATIN SMALL LETTER E WITH CARON -> e
    fallback_map[0x013D] = ord('L')  # Ľ LATIN CAPITAL LETTER L WITH CARON -> L
    fallback_map[0x013E] = ord('l')  # ľ LATIN SMALL LETTER L WITH CARON -> l
    fallback_map[0x0147] = ord('N')  # Ň LATIN CAPITAL LETTER N WITH CARON -> N
    fallback_map[0x0148] = ord('n')  # ň LATIN SMALL LETTER N WITH CARON -> n
    fallback_map[0x0158] = ord('R')  # Ř LATIN CAPITAL LETTER R WITH CARON -> R
    fallback_map[0x0159] = ord('r')  # ř LATIN SMALL LETTER R WITH CARON -> r
    fallback_map[0x0160] = ord('S')  # Š LATIN CAPITAL LETTER S WITH CARON -> S
    fallback_map[0x0161] = ord('s')  # š LATIN SMALL LETTER S WITH CARON -> s
    fallback_map[0x0164] = ord('T')  # Ť LATIN CAPITAL LETTER T WITH CARON -> T
    fallback_map[0x0165] = ord('t')  # ť LATIN SMALL LETTER T WITH CARON -> t
    fallback_map[0x017D] = ord('Z')  # Ž LATIN CAPITAL LETTER Z WITH CARON -> Z
    fallback_map[0x017E] = ord('z')  # ž LATIN SMALL LETTER Z WITH CARON -> z

    # Letters with acute (Polish, Hungarian, Czech, Slovak, Icelandic)
    fallback_map[0x0139] = ord('L')  # Ĺ LATIN CAPITAL LETTER L WITH ACUTE -> L
    fallback_map[0x013A] = ord('l')  # ĺ LATIN SMALL LETTER L WITH ACUTE -> l
    fallback_map[0x0143] = ord('N')  # Ń LATIN CAPITAL LETTER N WITH ACUTE -> N
    fallback_map[0x0144] = ord('n')  # ń LATIN SMALL LETTER N WITH ACUTE -> n
    fallback_map[0x0154] = ord('R')  # Ŕ LATIN CAPITAL LETTER R WITH ACUTE -> R
    fallback_map[0x0155] = ord('r')  # ŕ LATIN SMALL LETTER R WITH ACUTE -> r
    fallback_map[0x015A] = ord('S')  # Ś LATIN CAPITAL LETTER S WITH ACUTE -> S
    fallback_map[0x015B] = ord('s')  # ś LATIN SMALL LETTER S WITH ACUTE -> s
    fallback_map[0x0179] = ord('Z')  # Ź LATIN CAPITAL LETTER Z WITH ACUTE -> Z
    fallback_map[0x017A] = ord('z')  # ź LATIN SMALL LETTER Z WITH ACUTE -> z
    fallback_map[0x017B] = ord('Z')  # Ż LATIN CAPITAL LETTER Z WITH DOT ABOVE -> Z
    fallback_map[0x017C] = ord('z')  # ż LATIN SMALL LETTER Z WITH DOT ABOVE -> z

    # Letters with diaeresis/umlaut (used in various languages)
    fallback_map[0x0178] = ord('Y')  # Ÿ LATIN CAPITAL LETTER Y WITH DIAERESIS -> Y

    # Other common European letters
    fallback_map[0x00D0] = ord('D')  # Đ LATIN CAPITAL LETTER ETH -> D
    fallback_map[0x0110] = ord('D')  # Đ LATIN CAPITAL LETTER D WITH STROKE -> D
    fallback_map[0x0111] = ord('d')  # đ LATIN SMALL LETTER D WITH STROKE -> d
    fallback_map[0x0126] = ord('H')  # Ħ LATIN CAPITAL LETTER H WITH STROKE -> H
    fallback_map[0x0127] = ord('h')  # ħ LATIN SMALL LETTER H WITH STROKE -> h

    return fallback_map

def collect_drawing_character_mappings():
    """Collect box drawing characters with ASCII mappings."""
    fallback_map = {}

    # Box drawing characters
    # Horizontal lines
    for cp in range(0x2500, 0x2501+1):  # ─ ━
        fallback_map[cp] = ord('-')

    # Vertical lines
    for cp in range(0x2502, 0x2503+1):  # │ ┃
        fallback_map[cp] = ord('|')

    # Box corners and intersections

    # ┌ ┍ ┎ ┏
    for cp in range(0x250C, 0x250F+1):
        fallback_map[cp] = ord('+')

    # ┐ ┑ ┒ ┓
    for cp in range(0x2510, 0x2513+1):
        fallback_map[cp] = ord('+')

    # └ ┕ ┖ ┗
    for cp in range(0x2514, 0x2517+1):
        fallback_map[cp] = ord('+')

    # ┘ ┙ ┚ ┛
    for cp in range(0x2518, 0x251B+1):
        fallback_map[cp] = ord('+')

    # ├ ┝ ┞ ┟ ┠ ┡ ┢ ┣
    for cp in range(0x251C, 0x2523+1):
        fallback_map[cp] = ord('+')

    # ┤ ┥ ┦ ┧ ┨ ┩ ┪ ┫
    for cp in range(0x2524, 0x252B+1):
        fallback_map[cp] = ord('+')

    # ┬ ┭ ┮ ┯ ┰ ┱ ┲ ┳
    for cp in range(0x252C, 0x2533+1):
        fallback_map[cp] = ord('+')

    # ┴ ┵ ┶ ┷ ┸ ┹ ┺ ┻
    for cp in range(0x2534, 0x253B+1):
        fallback_map[cp] = ord('+')

    # ┼ ┽ ┾ ┿ ╀ ╁ ╂ ╃ ╄ ╅ ╆ ╇ ╈ ╉ ╊ ╋
    for cp in range(0x253C, 0x254B+1):
        fallback_map[cp] = ord('+')

    # Double box drawing characters
    fallback_map[0x2550] = ord('-')  # ═ BOX DRAWINGS DOUBLE HORIZONTAL
    fallback_map[0x2551] = ord('|')  # ║ BOX DRAWINGS DOUBLE VERTICAL

    # Double and mixed box corners and intersections
    # ╒ ╓ ╔ - top-left corners
    for cp in range(0x2552, 0x2554+1):
        fallback_map[cp] = ord('+')

    # ╕ ╖ ╗ - top-right corners
    for cp in range(0x2555, 0x2557+1):
        fallback_map[cp] = ord('+')

    # ╘ ╙ ╚ - bottom-left corners
    for cp in range(0x2558, 0x255A+1):
        fallback_map[cp] = ord('+')

    # ╛ ╜ ╝ - bottom-right corners
    for cp in range(0x255B, 0x255D+1):
        fallback_map[cp] = ord('+')

    # ╞ ╟ ╠ - left T-junctions
    for cp in range(0x255E, 0x2560+1):
        fallback_map[cp] = ord('+')

    # ╡ ╢ ╣ - right T-junctions
    for cp in range(0x2561, 0x2563+1):
        fallback_map[cp] = ord('+')

    # ╤ ╥ ╦ - top T-junctions
    for cp in range(0x2564, 0x2566+1):
        fallback_map[cp] = ord('+')

    # ╧ ╨ ╩ - bottom T-junctions
    for cp in range(0x2567, 0x2569+1):
        fallback_map[cp] = ord('+')

    # ╪ ╫ ╬ - crosses
    for cp in range(0x256A, 0x256C+1):
        fallback_map[cp] = ord('+')

    # Box drawing with arcs
    # ╭ ╮ ╯ ╰
    for cp in range(0x256D, 0x2570+1):
        fallback_map[cp] = ord('+')

    # Box drawing partials
    # Horizontal segments - map to dash
    # ╴ ╶ ╸ ╺ ╼ ╾
    fallback_map[0x2574] = ord('-')  # light left
    fallback_map[0x2576] = ord('-')  # light right
    fallback_map[0x2578] = ord('-')  # heavy left
    fallback_map[0x257A] = ord('-')  # heavy right
    fallback_map[0x257C] = ord('-')  # light left and heavy right
    fallback_map[0x257E] = ord('-')  # heavy left and light right

    # Vertical segments - map to pipe
    # ╵ ╷ ╹ ╻ ╽ ╿
    fallback_map[0x2575] = ord('|')  # light up
    fallback_map[0x2577] = ord('|')  # light down
    fallback_map[0x2579] = ord('|')  # heavy up
    fallback_map[0x257B] = ord('|')  # heavy down
    fallback_map[0x257D] = ord('|')  # light up and heavy down
    fallback_map[0x257F] = ord('|')  # heavy up and light down

    # Block elements
    # █ ▉ ▊ ▋ ▌ ▍ ▎ ▏ - map to #
    for cp in range(0x2588, 0x258F+1):
        fallback_map[cp] = ord('#')

    # ▀ ▁ ▂ ▃ ▄ ▅ ▆ ▇
    for cp in range(0x2580, 0x2587+1):
        fallback_map[cp] = ord('#')

    # Right side blocks
    fallback_map[0x2590] = ord('#')  # ▐ RIGHT HALF BLOCK
    fallback_map[0x2595] = ord('#')  # ▕ RIGHT ONE EIGHTH BLOCK
    fallback_map[0x2594] = ord('#')  # ▔ UPPER ONE EIGHTH BLOCK

    # Quadrant blocks
    for cp in range(0x2596, 0x259F+1):
        fallback_map[cp] = ord('#')  # Quadrant blocks (▖ ▗ ▘ ▙ ▚ ▛ ▜ ▝ ▞ ▟)

    # ▓ ▒ ░ - map to different densities of shading
    fallback_map[0x2593] = ord('#')  # ▓ Dark shade
    fallback_map[0x2592] = ord('%')  # ▒ Medium shade
    fallback_map[0x2591] = ord('.')  # ░ Light shade

    # Additional square/rectangle characters
    fallback_map[0x25AA] = ord('.')  # ▪ BLACK SMALL SQUARE
    fallback_map[0x25AB] = ord('.')  # ▫ WHITE SMALL SQUARE
    fallback_map[0x25AC] = ord('#')  # ▬ BLACK RECTANGLE
    fallback_map[0x25AD] = ord('-')  # ▭ WHITE RECTANGLE
    fallback_map[0x25AE] = ord('|')  # ▮ BLACK VERTICAL RECTANGLE
    fallback_map[0x25AF] = ord('|')  # ▯ WHITE VERTICAL RECTANGLE

    # Technical corner/bracket characters
    # Bottom corners
    fallback_map[0x23A3] = ord('|')  # ⎣ RIGHT BOTTOM CORNER -> |
    fallback_map[0x23A6] = ord('|')  # ⎦ RIGHT SQUARE BRACKET LOWER CORNER -> |
    fallback_map[0x23A9] = ord('|')  # ⎩ RIGHT SQUARE BRACKET LOWER CORNER WITH UPPER CORNER -> |
    fallback_map[0x23B3] = ord('|')  # ⎳ BOTTOM CURLY BRACKET -> |
    fallback_map[0x23B8] = ord('|')  # ⎸ LEFT VERTICAL BOX LINE -> |
    fallback_map[0x23B9] = ord('|')  # ⎹ RIGHT VERTICAL BOX LINE -> |
    fallback_map[0x23BD] = ord('_')  # ⎽ BOTTOM SQUARE BRACKET -> _
    fallback_map[0x23BF] = ord('L')  # ⎿ BOTTOM RIGHT CORNER -> L
    fallback_map[0x23BE] = ord('L')  # ⎾ TOP RIGHT CORNER -> L
    fallback_map[0x23BC] = ord('J')  # ⎼ TOP SQUARE BRACKET -> J

    # Top corners
    fallback_map[0x23A1] = ord('|')  # ⎡ LEFT SQUARE BRACKET UPPER CORNER -> |
    fallback_map[0x23A4] = ord('|')  # ⎤ RIGHT SQUARE BRACKET UPPER CORNER -> |
    fallback_map[0x23A7] = ord('|')  # ⎧ LEFT CURLY BRACKET UPPER HOOK -> |
    fallback_map[0x23AB] = ord('|')  # ⎫ RIGHT CURLY BRACKET UPPER HOOK -> |
    fallback_map[0x23B0] = ord('(')  # ⎰ UPPER LEFT OR LOWER RIGHT CURLY BRACKET SECTION -> (
    fallback_map[0x23B1] = ord(')')  # ⎱ UPPER RIGHT OR LOWER LEFT CURLY BRACKET SECTION -> )

    # Other useful box-drawing-like characters
    # Diagonal lines
    # ╱ ╲ ╳
    fallback_map[0x2571] = ord('/')   # ╱ Diagonal up-right to down-left
    fallback_map[0x2572] = ord('\\')  # ╲ Diagonal up-left to down-right
    fallback_map[0x2573] = ord('X')   # ╳ Diagonal cross

    # Arrows to ASCII equivalent
    # → ⇒ ⟹ etc. to ->
    for cp in [0x2192, 0x21D2, 0x27F9]:
        fallback_map[cp] = ord('>')  # Treat as '>' for simplicity

    # ← ⇐ ⟸ etc. to <-
    for cp in [0x2190, 0x21D0, 0x27F8]:
        fallback_map[cp] = ord('<')  # Treat as '<' for simplicity

    # ↑ ⇑ etc. to ^
    for cp in [0x2191, 0x21D1]:
        fallback_map[cp] = ord('^')

    # ↓ ⇓ etc. to v
    for cp in [0x2193, 0x21D3]:
        fallback_map[cp] = ord('v')

    # Mathematical symbols
    fallback_map[0x00B1] = ord('+')  # ± PLUS-MINUS SIGN -> +
    fallback_map[0x00D7] = ord('x')  # × MULTIPLICATION SIGN -> x
    fallback_map[0x00F7] = ord('/')  # ÷ DIVISION SIGN -> /
    fallback_map[0x2212] = ord('-')  # − MINUS SIGN -> -
    fallback_map[0x2213] = ord('+')  # ∓ MINUS-OR-PLUS SIGN -> +
    fallback_map[0x2215] = ord('/')  # ∕ DIVISION SLASH -> /
    fallback_map[0x2216] = ord('\\')  # ∖ SET MINUS -> \
    fallback_map[0x2217] = ord('*')  # ∗ ASTERISK OPERATOR -> *
    fallback_map[0x2218] = ord('o')  # ∘ RING OPERATOR -> o
    fallback_map[0x2219] = ord('.')  # ∙ BULLET OPERATOR -> .
    fallback_map[0x221A] = ord('v')  # √ SQUARE ROOT -> v
    fallback_map[0x221E] = ord('8')  # ∞ INFINITY -> 8
    fallback_map[0x2223] = ord('|')  # ∣ DIVIDES -> |
    fallback_map[0x2225] = ord('|')  # ∥ PARALLEL TO -> |
    fallback_map[0x2227] = ord('&')  # ∧ LOGICAL AND -> & (C-style)
    fallback_map[0x2228] = ord('|')  # ∨ LOGICAL OR -> | (C-style)
    fallback_map[0x2229] = ord('n')  # ∩ INTERSECTION -> n
    fallback_map[0x222A] = ord('u')  # ∪ UNION -> u
    fallback_map[0x222B] = ord('S')  # ∫ INTEGRAL -> S
    fallback_map[0x2234] = ord(':')  # ∴ THEREFORE -> :
    fallback_map[0x2235] = ord(':')  # ∵ BECAUSE -> :
    fallback_map[0x2248] = ord('~')  # ≈ ALMOST EQUAL TO -> ~
    fallback_map[0x2264] = ord('<')  # ≤ LESS-THAN OR EQUAL TO -> <
    fallback_map[0x2265] = ord('>')  # ≥ GREATER-THAN OR EQUAL TO -> >
    fallback_map[0x2282] = ord('c')  # ⊂ SUBSET OF -> c
    fallback_map[0x2283] = ord('C')  # ⊃ SUPERSET OF -> C
    fallback_map[0x2286] = ord('c')  # ⊆ SUBSET OF OR EQUAL TO -> c
    fallback_map[0x2287] = ord('C')  # ⊇ SUPERSET OF OR EQUAL TO -> C
    fallback_map[0x22C5] = ord('.')  # ⋅ DOT OPERATOR -> .

    # Currency symbols
    fallback_map[0x00A2] = ord('c')  # ¢ CENT SIGN -> c
    fallback_map[0x00A3] = ord('L')  # £ POUND SIGN -> L
    fallback_map[0x00A5] = ord('Y')  # ¥ YEN SIGN -> Y
    fallback_map[0x20AC] = ord('E')  # € EURO SIGN -> E

    # Common symbols
    fallback_map[0x00A9] = ord('C')  # © COPYRIGHT SIGN -> C
    fallback_map[0x00AE] = ord('R')  # ® REGISTERED SIGN -> R
    fallback_map[0x2122] = ord('T')  # ™ TRADE MARK SIGN -> T
    fallback_map[0x00A7] = ord('S')  # § SECTION SIGN -> S
    fallback_map[0x00B6] = ord('P')  # ¶ PILCROW SIGN -> P
    fallback_map[0x00A6] = ord('|')  # ¦ BROKEN BAR -> |
    fallback_map[0x00B0] = ord('o')  # ° DEGREE SIGN -> o
    fallback_map[0x00B5] = ord('u')  # µ MICRO SIGN -> u
    fallback_map[0x2103] = ord('C')  # ℃ DEGREE CELSIUS -> C
    fallback_map[0x2109] = ord('F')  # ℉ DEGREE FAHRENHEIT -> F

    # Superscript and subscript numbers
    fallback_map[0x00B2] = ord('2')  # ² SUPERSCRIPT TWO -> 2
    fallback_map[0x00B3] = ord('3')  # ³ SUPERSCRIPT THREE -> 3
    fallback_map[0x00B9] = ord('1')  # ¹ SUPERSCRIPT ONE -> 1
    fallback_map[0x2070] = ord('0')  # ⁰ SUPERSCRIPT ZERO -> 0
    fallback_map[0x2074] = ord('4')  # ⁴ SUPERSCRIPT FOUR -> 4
    fallback_map[0x2075] = ord('5')  # ⁵ SUPERSCRIPT FIVE -> 5
    fallback_map[0x2076] = ord('6')  # ⁶ SUPERSCRIPT SIX -> 6
    fallback_map[0x2077] = ord('7')  # ⁷ SUPERSCRIPT SEVEN -> 7
    fallback_map[0x2078] = ord('8')  # ⁸ SUPERSCRIPT EIGHT -> 8
    fallback_map[0x2079] = ord('9')  # ⁹ SUPERSCRIPT NINE -> 9
    fallback_map[0x2080] = ord('0')  # ₀ SUBSCRIPT ZERO -> 0
    fallback_map[0x2081] = ord('1')  # ₁ SUBSCRIPT ONE -> 1
    fallback_map[0x2082] = ord('2')  # ₂ SUBSCRIPT TWO -> 2
    fallback_map[0x2083] = ord('3')  # ₃ SUBSCRIPT THREE -> 3
    fallback_map[0x2084] = ord('4')  # ₄ SUBSCRIPT FOUR -> 4
    fallback_map[0x2085] = ord('5')  # ₅ SUBSCRIPT FIVE -> 5
    fallback_map[0x2086] = ord('6')  # ₆ SUBSCRIPT SIX -> 6
    fallback_map[0x2087] = ord('7')  # ₇ SUBSCRIPT SEVEN -> 7
    fallback_map[0x2088] = ord('8')  # ₈ SUBSCRIPT EIGHT -> 8
    fallback_map[0x2089] = ord('9')  # ₉ SUBSCRIPT NINE -> 9

    # Common Greek letters used in math/science
    fallback_map[0x03B1] = ord('a')  # α GREEK SMALL LETTER ALPHA -> a
    fallback_map[0x03B2] = ord('B')  # β GREEK SMALL LETTER BETA -> B
    fallback_map[0x03B3] = ord('y')  # γ GREEK SMALL LETTER GAMMA -> y
    fallback_map[0x0393] = ord('I')  # Γ GREEK CAPITAL LETTER GAMMA -> I
    fallback_map[0x03B4] = ord('d')  # δ GREEK SMALL LETTER DELTA -> d
    fallback_map[0x0394] = ord('A')  # Δ GREEK CAPITAL LETTER DELTA -> A
    fallback_map[0x03B5] = ord('e')  # ε GREEK SMALL LETTER EPSILON -> e
    fallback_map[0x03B6] = ord('z')  # ζ GREEK SMALL LETTER ZETA -> z
    fallback_map[0x03B7] = ord('n')  # η GREEK SMALL LETTER ETA -> n
    fallback_map[0x03B8] = ord('0')  # θ GREEK SMALL LETTER THETA -> 0
    fallback_map[0x0398] = ord('O')  # Θ GREEK CAPITAL LETTER THETA -> O
    fallback_map[0x03BB] = ord('l')  # λ GREEK SMALL LETTER LAMBDA -> l
    fallback_map[0x039B] = ord('A')  # Λ GREEK CAPITAL LETTER LAMBDA -> A
    fallback_map[0x03BC] = ord('u')  # μ GREEK SMALL LETTER MU -> u
    fallback_map[0x03C0] = ord('n')  # π GREEK SMALL LETTER PI -> n
    fallback_map[0x03A0] = ord('n')  # Π GREEK CAPITAL LETTER PI -> n
    fallback_map[0x03C1] = ord('p')  # ρ GREEK SMALL LETTER RHO -> p
    fallback_map[0x03C3] = ord('o')  # σ GREEK SMALL LETTER SIGMA -> o
    fallback_map[0x03A3] = ord('E')  # Σ GREEK CAPITAL LETTER SIGMA -> E
    fallback_map[0x03C4] = ord('t')  # τ GREEK SMALL LETTER TAU -> t
    fallback_map[0x03C6] = ord('f')  # φ GREEK SMALL LETTER PHI -> f
    fallback_map[0x03A6] = ord('O')  # Φ GREEK CAPITAL LETTER PHI -> O
    fallback_map[0x03C7] = ord('X')  # χ GREEK SMALL LETTER CHI -> X
    fallback_map[0x03C8] = ord('w')  # ψ GREEK SMALL LETTER PSI -> w
    fallback_map[0x03A8] = ord('Y')  # Ψ GREEK CAPITAL LETTER PSI -> Y
    fallback_map[0x03C9] = ord('w')  # ω GREEK SMALL LETTER OMEGA -> w
    fallback_map[0x03A9] = ord('O')  # Ω GREEK CAPITAL LETTER OMEGA -> O

    # Additional punctuation
    fallback_map[0x2018] = ord('\'')  # ' LEFT SINGLE QUOTATION MARK -> '
    fallback_map[0x2019] = ord('\'')  # ' RIGHT SINGLE QUOTATION MARK -> '
    fallback_map[0x201A] = ord(',')  # ‚ SINGLE LOW-9 QUOTATION MARK -> ,
    fallback_map[0x201B] = ord('\'')  # ‛ SINGLE HIGH-REVERSED-9 QUOTATION MARK -> '
    fallback_map[0x201C] = ord('"')  # " LEFT DOUBLE QUOTATION MARK -> "
    fallback_map[0x201D] = ord('"')  # " RIGHT DOUBLE QUOTATION MARK -> "
    fallback_map[0x201E] = ord('"')  # „ DOUBLE LOW-9 QUOTATION MARK -> "
    fallback_map[0x201F] = ord('"')  # ‟ DOUBLE HIGH-REVERSED-9 QUOTATION MARK -> "
    fallback_map[0x2026] = ord('.')  # … HORIZONTAL ELLIPSIS -> .
    fallback_map[0x2039] = ord('<')  # ‹ SINGLE LEFT-POINTING ANGLE QUOTATION MARK -> <
    fallback_map[0x203A] = ord('>')  # › SINGLE RIGHT-POINTING ANGLE QUOTATION MARK -> >
    fallback_map[0x00AB] = ord('<')  # « LEFT-POINTING DOUBLE ANGLE QUOTATION MARK -> <
    fallback_map[0x00BB] = ord('>')  # » RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK -> >

    # Various dashes and hyphens - all map to ASCII hyphen-minus
    for cp in range(0x2010, 0x2015+1):
        fallback_map[cp] = ord('-')  # All forms of hyphens and dashes -> -
    fallback_map[0x2043] = ord('-')  # ⁃ HYPHEN BULLET -> -
    fallback_map[0x2052] = ord('-')  # ⁒ COMMERCIAL MINUS SIGN -> -

    # Other punctuation and symbols
    fallback_map[0x2023] = ord('>')  # ‣ TRIANGULAR BULLET -> >
    fallback_map[0x2027] = ord('.')  # ‧ HYPHENATION POINT -> .
    fallback_map[0x2032] = ord('\'')  # ′ PRIME -> '
    fallback_map[0x2033] = ord('"')  # ″ DOUBLE PRIME -> "
    # Note: Triple prime (U+2034) intentionally omitted to avoid misleading fallback
    fallback_map[0x203B] = ord('*')  # ※ REFERENCE MARK -> *
    fallback_map[0x203C] = ord('!')  # ‼ DOUBLE EXCLAMATION MARK -> !
    fallback_map[0x203D] = ord('?')  # ‽ INTERROBANG -> ?
    fallback_map[0x2044] = ord('/')  # ⁄ FRACTION SLASH -> /
    fallback_map[0x2047] = ord('?')  # ⁇ DOUBLE QUESTION MARK -> ?
    fallback_map[0x2048] = ord('?')  # ⁈ QUESTION EXCLAMATION MARK -> ?
    fallback_map[0x2049] = ord('!')  # ⁉ EXCLAMATION QUESTION MARK -> !
    fallback_map[0x204A] = ord('&')  # ⁊ TIRONIAN SIGN ET -> &
    fallback_map[0x204B] = ord('P')  # ⁋ REVERSED PILCROW SIGN -> P
    fallback_map[0x204C] = ord('<')  # ⁌ BLACK LEFTWARDS BULLET -> <
    fallback_map[0x204D] = ord('>')  # ⁍ BLACK RIGHTWARDS BULLET -> >
    fallback_map[0x204E] = ord('*')  # ⁎ LOW ASTERISK -> *
    fallback_map[0x204F] = ord(';')  # ⁏ REVERSED SEMICOLON -> ;
    fallback_map[0x2053] = ord('~')  # ⁓ SWUNG DASH -> ~
    fallback_map[0x2055] = ord('*')  # ⁕ FLOWER PUNCTUATION MARK -> *
    fallback_map[0x205B] = ord(':')  # ⁛ FOUR DOT MARK -> :

    # Precomposed negated symbols require special handling. Standard Unicode
    # decomposition would strip the negation and preserve only the base symbol,
    # which would reverse the intended meaning (e.g., "not equal" would become
    # "equal"). This could lead to confusion or errors when reading text with
    # fallback characters. Let's override decomposition by providing explicit
    # mappings in a best effort at avoiding misleading interpretations.

    # Negated mathematical operators
    fallback_map[0x2204] = ord('!')  # ∄ THERE DOES NOT EXIST -> !
    fallback_map[0x2209] = ord('!')  # ∉ NOT AN ELEMENT OF -> !
    fallback_map[0x220C] = ord('!')  # ∌ DOES NOT CONTAIN AS MEMBER -> !
    fallback_map[0x2224] = ord('!')  # ∤ DOES NOT DIVIDE -> !
    fallback_map[0x2226] = ord('!')  # ∦ NOT PARALLEL TO -> !
    fallback_map[0x2241] = ord('#')  # ≁ NOT TILDE -> # (better than ~)
    fallback_map[0x2244] = ord('#')  # ≄ NOT ASYMPTOTICALLY EQUAL TO -> # (better than ~)
    fallback_map[0x2249] = ord('#')  # ≉ NOT ALMOST EQUAL TO -> # (better than ~)
    fallback_map[0x2260] = ord('#')  # ≠ NOT EQUAL TO -> # (better than =)
    fallback_map[0x2262] = ord('#')  # ≢ NOT IDENTICAL TO -> # (better than =)
    fallback_map[0x2268] = ord('#')  # ≨ LESS-THAN BUT NOT EQUAL TO -> # (better than <)
    fallback_map[0x2269] = ord('#')  # ≩ GREATER-THAN BUT NOT EQUAL TO -> # (better than >)
    fallback_map[0x226D] = ord('#')  # ≭ NOT EQUIVALENT TO -> # (better than =)
    fallback_map[0x226E] = ord('!')  # ≮ NOT LESS-THAN -> ! (better than <)
    fallback_map[0x226F] = ord('!')  # ≯ NOT GREATER-THAN -> ! (better than >)

    # Negated set operators
    fallback_map[0x2280] = ord('!')  # ⊀ DOES NOT PRECEDE -> !
    fallback_map[0x2281] = ord('!')  # ⊁ DOES NOT SUCCEED -> !
    fallback_map[0x2284] = ord('!')  # ⊄ NOT A SUBSET OF -> ! (better than c)
    fallback_map[0x2285] = ord('!')  # ⊅ NOT A SUPERSET OF -> ! (better than C)
    fallback_map[0x228A] = ord('#')  # ⊊ SUBSET OF WITH NOT EQUAL TO -> #
    fallback_map[0x228B] = ord('#')  # ⊋ SUPERSET OF WITH NOT EQUAL TO -> #

    # Negated logical operators
    fallback_map[0x22AC] = ord('!')  # ⊬ DOES NOT PROVE -> !
    fallback_map[0x22AD] = ord('!')  # ⊭ NOT TRUE -> !
    fallback_map[0x22AE] = ord('!')  # ⊮ DOES NOT FORCE -> !
    fallback_map[0x22E0] = ord('!')  # ⋠ DOES NOT PRECEDE OR EQUAL -> !
    fallback_map[0x22E1] = ord('!')  # ⋡ DOES NOT SUCCEED OR EQUAL -> !
    fallback_map[0x22EA] = ord('!')  # ⋪ NOT NORMAL SUBGROUP OF -> !
    fallback_map[0x22EB] = ord('!')  # ⋫ DOES NOT CONTAIN AS NORMAL SUBGROUP -> !

    # Negated arrows
    fallback_map[0x219A] = ord('!')  # ↚ LEFTWARDS ARROW WITH STROKE -> !
    fallback_map[0x219B] = ord('!')  # ↛ RIGHTWARDS ARROW WITH STROKE -> !
    fallback_map[0x21AE] = ord('!')  # ↮ LEFT RIGHT ARROW WITH STROKE -> !
    fallback_map[0x21CD] = ord('!')  # ⇍ LEFTWARDS DOUBLE ARROW WITH STROKE -> !
    fallback_map[0x21CE] = ord('!')  # ⇎ LEFT RIGHT DOUBLE ARROW WITH STROKE -> !
    fallback_map[0x21CF] = ord('!')  # ⇏ RIGHTWARDS DOUBLE ARROW WITH STROKE -> !

    # Bullets and geometric shapes
    # • ◦ ○ ◎ ● ◆ ■ □ ▲ △ ▼ ▽
    fallback_map[0x2022] = ord('*')  # • Bullet
    fallback_map[0x25E6] = ord('o')  # ◦ White bullet
    fallback_map[0x25CB] = ord('o')  # ○ White circle
    fallback_map[0x25CE] = ord('o')  # ◎ Bullseye
    fallback_map[0x25CF] = ord('*')  # ● Black circle
    fallback_map[0x25C6] = ord('*')  # ◆ Black diamond
    fallback_map[0x25A0] = ord('#')  # ■ Black square
    fallback_map[0x25A1] = ord('o')  # □ White square
    fallback_map[0x25B2] = ord('^')  # ▲ Black up-pointing triangle
    fallback_map[0x25B3] = ord('^')  # △ White up-pointing triangle
    fallback_map[0x25BC] = ord('v')  # ▼ Black down-pointing triangle
    fallback_map[0x25BD] = ord('v')  # ▽ White down-pointing triangle

    # Middle dot and other punctuation
    fallback_map[0x00B7] = ord('.')  # · MIDDLE DOT
    fallback_map[0x0387] = ord('.')  # · GREEK ANO TELEIA (identical to middle dot)
    fallback_map[0x2027] = ord('.')  # ‧ HYPHENATION POINT
    fallback_map[0x2219] = ord('.')  # ∙ BULLET OPERATOR
    fallback_map[0x22C5] = ord('.')  # ⋅ DOT OPERATOR
    fallback_map[0x00A1] = ord('!')  # ¡ INVERTED EXCLAMATION MARK
    fallback_map[0x00BF] = ord('?')  # ¿ INVERTED QUESTION MARK
    fallback_map[0x203D] = ord('?')  # ‽ INTERROBANG

    # Note: Vulgar fractions (like ½, ¼, etc.) are intentionally not mapped to ASCII.
    # Using just the numerator (e.g., ½ → 1) would be misleading, and there's no good
    # single-character ASCII representation for fractions.

    # Check marks and X marks
    fallback_map[0x2713] = ord('v')  # ✓ CHECK MARK
    fallback_map[0x2714] = ord('V')  # ✔ HEAVY CHECK MARK
    fallback_map[0x2715] = ord('x')  # ✕ MULTIPLICATION X
    fallback_map[0x2716] = ord('X')  # ✖ HEAVY MULTIPLICATION X
    fallback_map[0x2717] = ord('x')  # ✗ BALLOT X
    fallback_map[0x2718] = ord('X')  # ✘ HEAVY BALLOT X

    # Asterism and asterisk variants
    fallback_map[0x2042] = ord('*')  # ⁂ ASTERISM
    fallback_map[0x204E] = ord('*')  # ⁎ LOW ASTERISK
    fallback_map[0x2051] = ord('*')  # ⁑ TWO ASTERISKS ALIGNED VERTICALLY
    fallback_map[0x2055] = ord('*')  # ⁕ FLOWER PUNCTUATION MARK
    fallback_map[0x2217] = ord('*')  # ∗ ASTERISK OPERATOR
    fallback_map[0x229B] = ord('*')  # ⊛ CIRCLED ASTERISK OPERATOR
    fallback_map[0x22C6] = ord('*')  # ⋆ STAR OPERATOR
    fallback_map[0x235F] = ord('*')  # ⍟ APL FUNCTIONAL SYMBOL CIRCLE STAR
    fallback_map[0x2363] = ord('*')  # ⍣ APL FUNCTIONAL SYMBOL STAR DIAERESIS

    # Stars
    fallback_map[0x2605] = ord('*')  # ★ BLACK STAR
    fallback_map[0x2606] = ord('*')  # ☆ WHITE STAR
    fallback_map[0x262A] = ord('*')  # ☪ STAR AND CRESCENT
    fallback_map[0x269D] = ord('*')  # ⚝ OUTLINED WHITE STAR
    fallback_map[0x2721] = ord('*')  # ✡ STAR OF DAVID
    fallback_map[0x2726] = ord('*')  # ✦ BLACK FOUR POINTED STAR
    fallback_map[0x2727] = ord('*')  # ✧ WHITE FOUR POINTED STAR
    fallback_map[0x2729] = ord('*')  # ✩ STRESS OUTLINED WHITE STAR
    fallback_map[0x272A] = ord('*')  # ✪ CIRCLED WHITE STAR
    fallback_map[0x272B] = ord('*')  # ✫ OPEN CENTRE BLACK STAR
    fallback_map[0x272C] = ord('*')  # ✬ BLACK CENTRE WHITE STAR
    fallback_map[0x272D] = ord('*')  # ✭ OUTLINED BLACK STAR
    fallback_map[0x272E] = ord('*')  # ✮ HEAVY OUTLINED BLACK STAR
    fallback_map[0x272F] = ord('*')  # ✯ PINWHEEL STAR
    fallback_map[0x2730] = ord('*')  # ✰ SHADOWED WHITE STAR
    fallback_map[0x2734] = ord('*')  # ✴ EIGHT POINTED BLACK STAR
    fallback_map[0x2735] = ord('*')  # ✵ EIGHT POINTED PINWHEEL STAR
    fallback_map[0x2736] = ord('*')  # ✶ SIX POINTED BLACK STAR
    fallback_map[0x2737] = ord('*')  # ✷ EIGHT POINTED RECTILINEAR BLACK STAR
    fallback_map[0x2738] = ord('*')  # ✸ HEAVY EIGHT POINTED RECTILINEAR BLACK STAR
    fallback_map[0x2739] = ord('*')  # ✹ TWELVE POINTED BLACK STAR

    # Asterisk variants
    fallback_map[0x273A] = ord('*')  # ✺ SIXTEEN POINTED ASTERISK
    fallback_map[0x273B] = ord('*')  # ✻ TEARDROP-SPOKED ASTERISK
    fallback_map[0x273C] = ord('*')  # ✼ OPEN CENTRE TEARDROP-SPOKED ASTERISK
    fallback_map[0x273D] = ord('*')  # ✽ HEAVY TEARDROP-SPOKED ASTERISK
    fallback_map[0x2722] = ord('*')  # ✢ FOUR TEARDROP-SPOKED ASTERISK
    fallback_map[0x2723] = ord('*')  # ✣ FOUR BALLOON-SPOKED ASTERISK
    fallback_map[0x2724] = ord('*')  # ✤ HEAVY FOUR BALLOON-SPOKED ASTERISK
    fallback_map[0x2725] = ord('*')  # ✥ FOUR CLUB-SPOKED ASTERISK
    fallback_map[0x2731] = ord('*')  # ✱ HEAVY ASTERISK
    fallback_map[0x2732] = ord('*')  # ✲ OPEN CENTRE ASTERISK
    fallback_map[0x2733] = ord('*')  # ✳ EIGHT SPOKED ASTERISK
    fallback_map[0x2749] = ord('*')  # ❉ BALLOON-SPOKED ASTERISK
    fallback_map[0x274A] = ord('*')  # ❊ EIGHT TEARDROP-SPOKED PROPELLER ASTERISK
    fallback_map[0x274B] = ord('*')  # ❋ HEAVY EIGHT TEARDROP-SPOKED PROPELLER ASTERISK
    fallback_map[0x2743] = ord('*')  # ❃ HEAVY TEARDROP-SPOKED PINWHEEL ASTERISK

    # Florettes and snowflakes
    fallback_map[0x273E] = ord('*')  # ✾ SIX PETALLED BLACK AND WHITE FLORETTE
    fallback_map[0x273F] = ord('*')  # ✿ BLACK FLORETTE
    fallback_map[0x2740] = ord('*')  # ❀ WHITE FLORETTE
    fallback_map[0x2741] = ord('*')  # ❁ EIGHT PETALLED OUTLINED BLACK FLORETTE
    fallback_map[0x2742] = ord('*')  # ❂ CIRCLED OPEN CENTRE EIGHT POINTED STAR
    fallback_map[0x2744] = ord('*')  # ❄ SNOWFLAKE
    fallback_map[0x2745] = ord('*')  # ❅ TIGHT TRIFOLIATE SNOWFLAKE
    fallback_map[0x2746] = ord('*')  # ❆ HEAVY CHEVRON SNOWFLAKE
    fallback_map[0x2698] = ord('*')  # ⚘ FLOWER

    # Add special ASCII characters with full-width equivalents
    # Map between full-width and ASCII forms
    for i, cp in enumerate(range(0xFF01, 0xFF5E+1)):
        # Full-width to ASCII mapping (covering all printable ASCII 33-126)
        # 0xFF01 (！) to 0xFF5E (～) -> ASCII 33 (!) to 126 (~)
        fallback_map[cp] = 33 + i

    return fallback_map

def collect_decomposition_pairs():
    """Collect all possible decomposition pairs from the Unicode data."""
    # Map to store decomposition pairs: composite -> base
    fallback_map = {}

    # Process all assigned Unicode code points in BMP (Basic Multilingual Plane)
    # We limit to BMP (0x0000-0xFFFF) to keep our table smaller with uint16_t
    for cp in range(0, 0x10000):
        try:
            char = chr(cp)

            # Skip unassigned or control characters
            if not unicodedata.name(char, ''):
                continue

            # Find decomposition
            decomp = unicodedata.decomposition(char)
            if not decomp or '<' in decomp:  # Skip compatibility decompositions
                continue

            # Parse the decomposition
            parts = decomp.split()
            if len(parts) == 2:  # Simple base + combining mark
                base = int(parts[0], 16)
                combining = int(parts[1], 16)

                # Only store if base is in BMP
                if base < 0x10000:
                    fallback_map[cp] = base

        except (ValueError, TypeError):
            continue

    return fallback_map

def create_hybrid_tables(fallback_map):
    """
    Create optimized hybrid tables for fallback characters.

    Args:
        fallback_map: The original mapping of complex characters to base characters

    Returns:
        A tuple of (intervals, singles, dropped) where:
        - intervals: List of (first, last, fallback) tuples
        - singles: List of (codepoint, fallback) tuples
        - dropped: List of (codepoint, fallback) tuples
    """

    # Create a map with fallbacks that fit in a single byte (≤ 0xFF)
    # following fallback chains until a suitable byte-sized fallback is found.
    # Using byte-sized fallbacks saves table space and Latin-1 glyphs are
    # more likely to exist. Runtime code may recurse further if necessary.
    byte_fallback_map = {}
    dropped = []
    for complex_char, fallback in fallback_map.items():
        while fallback > 0xFF and fallback in fallback_map:
            fallback = fallback_map[fallback]
        if fallback <= 0xFF:
            byte_fallback_map[complex_char] = fallback
        else:
            dropped.append((complex_char, fallback))

    # Group characters by their base character
    base_groups = defaultdict(list)
    for complex_char, base in byte_fallback_map.items():
        base_groups[base].append(complex_char)

    # Sort complex characters in each group
    for base in base_groups:
        base_groups[base].sort()

    # Create interval tables and single-entry tables
    intervals = []
    singles = []

    for base, complex_char_list in base_groups.items():
        # Identify continuous ranges
        ranges = []
        current_range = [complex_char_list[0], complex_char_list[0]]

        for i in range(1, len(complex_char_list)):
            if complex_char_list[i] == current_range[1] + 1:
                # Extend current range
                current_range[1] = complex_char_list[i]
            else:
                # Finish current range and start a new one
                ranges.append(tuple(current_range))
                current_range = [complex_char_list[i], complex_char_list[i]]

        # Add the last range
        ranges.append(tuple(current_range))

        # Add to appropriate table
        for first, last in ranges:
            if first == last:
                # Single entry
                singles.append((first, base))
            else:
                # Range
                intervals.append((first, last, base))

    # Sort tables by first code point for binary search
    intervals.sort()
    singles.sort()

    return intervals, singles, dropped

def cp_name(cp):
    try:
        return unicodedata.name(chr(cp))
    except:
        return f"U+{cp:04X}"

def generate_fallback_tables(out_file=DEFAULT_OUT_FILE):
    """Generate the fallback character tables."""
    # Collect standard decomposition pairs
    decomposition_map = collect_decomposition_pairs()
    print(f"Collected {len(decomposition_map)} standard decomposition pairs")

    # Collect composed Latin letters
    latin_map = collect_accented_latin_letters()
    print(f"Collected {len(latin_map)} already composed Latin letter mappings")

    # Collect drawing character mappings
    drawing_map = collect_drawing_character_mappings()
    print(f"Collected {len(drawing_map)} drawing character mappings")

    # Combine maps - prioritize explicit mappings over decomposition
    # This ensures that composed characters can be handled directly even if
    # decomposition would also work
    fallback_map = {**decomposition_map, **latin_map, **drawing_map}
    print(f"Combined into {len(fallback_map)} total mappings")

    # Create hybrid tables with fallback values limited to 1 byte (0xFF)
    intervals, singles, dropped = create_hybrid_tables(fallback_map)
    print(f"Dropped {len(dropped)} mappings whose fallback was larger than a byte")
    print(f"Created {len(intervals)} intervals and {len(singles)} single entries")

    # Generate C tables
    with open(out_file, 'w') as f:
        f.write(f"""\
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * {out_file} - Unicode character fallback table for display simplification
 *
 * Auto-generated by {this_file}
 *
 * Unicode Version: {unicodedata.unidata_version}
 *
 * This file contains tables that map complex Unicode characters to simpler
 * fallback characters for terminal display when corresponding glyphs are
 * unavailable.
 */

static const struct ucs_interval16 ucs_fallback_intervals[] = {{
""")

        # Write interval table
        for first, last, fallback in intervals:
            comment = f"/* {cp_name(first)} - {cp_name(last)} -> {cp_name(fallback)} */"
            f.write(f"\t{{ 0x{first:04X}, 0x{last:04X}, }}, {comment}\n")

        f.write("""\
};

static const u8 ucs_fallback_intervals_subs[] = {
""")

        # Write interval fallback character table
        for first, last, fallback in intervals:
            comment = f"/* {cp_name(first)} - {cp_name(last)} -> {cp_name(fallback)} */"
            f.write(f"\t0x{fallback:02X}, {comment}\n")

        f.write("""\
};

static const u16 ucs_fallback_singles[] = {
""")

        # Write single entry table
        for codepoint, fallback in singles:
            comment = f"/* {cp_name(codepoint)} -> {cp_name(fallback)} */"
            f.write(f"\t0x{codepoint:04X}, {comment}\n")

        f.write("""\
};

static const u8 ucs_fallback_singles_subs[] = {
""")

        # Write single fallback character table
        for codepoint, fallback in singles:
            comment = f"/* {cp_name(codepoint)} -> {cp_name(fallback)} */"
            f.write(f"\t0x{fallback:02X}, {comment}\n")

        f.write("};\n")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate Unicode fallback character tables")
    parser.add_argument("-o", "--output", dest="output_file", default=DEFAULT_OUT_FILE,
                       help=f"Output file name (default: {DEFAULT_OUT_FILE})")
    args = parser.parse_args()

    generate_fallback_tables(out_file=args.output_file)
