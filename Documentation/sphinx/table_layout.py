# SPDX-License-Identifier: GPL-2.0
#
"""Provide responsive table layout hooks for HTML documentation.

Wrap rendered tables in an outer container that enables contained horizontal
scrolling on narrow viewports as needed. Allowing the table to remain wider
than the viewport helps prevent columns from collapsing into unreadable
vertical text, while containing page-wide overflow that would increase the
total page width and break the page margins. Applying overflow directly to the
table instead of the wrapper creates a double-border rendering defect.
"""

from sphinx.writers.html5 import HTML5Translator

__version__ = "1.0"


class LayoutInjectionHTMLTranslator(HTML5Translator):
    """Add HTML containers needed for responsive layout behavior."""

    def visit_table(self, node):
        self.body.append('<div class="table-overflow">\n')
        super().visit_table(node)

    def depart_table(self, node):
        super().depart_table(node)
        self.body.append("</div>\n")


def setup(app):
    for builder in ("html", "dirhtml", "singlehtml"):
        app.set_translator(builder, LayoutInjectionHTMLTranslator, override=True)

    return dict(
        version=__version__,
        parallel_read_safe=True,
        parallel_write_safe=True,
    )
