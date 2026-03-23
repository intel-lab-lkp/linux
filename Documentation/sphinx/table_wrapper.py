# SPDX-License-Identifier: GPL-2.0
#
"""Wrap generated HTML tables in a responsive overflow container."""

from sphinx.writers.html5 import HTML5Translator

__version__ = "1.0"


class TableWrapperHTMLTranslator(HTML5Translator):
    """Add a wrapper around tables so CSS can control overflow behavior."""

    def visit_table(self, node):
        self.body.append('<div class="table-overflow">\n')
        super().visit_table(node)

    def depart_table(self, node):
        super().depart_table(node)
        self.body.append("</div>\n")


def setup(app):
    for builder in ("html", "dirhtml", "singlehtml"):
        app.set_translator(builder, TableWrapperHTMLTranslator, override=True)

    return dict(
        version=__version__,
        parallel_read_safe=True,
        parallel_write_safe=True,
    )
