# SPDX-License-Identifier: GPL-2.0
#
"""Provide responsive table layout hooks for HTML documentation.

Wrap rendered tables in an outer container that enables contained horizontal
scrolling on narrow viewports as needed. Allowing the table to remain wider
than the viewport helps prevent columns from collapsing into unreadable
vertical text, while containing page-wide overflow that would increase the
total page width and break the page margins. Applying overflow directly to the
table instead of the wrapper creates a double-border rendering defect.

Derive a targeted minimum width for each logical column from the content
belonging to that column. These minimums prevent aggressive wrapping from
collapsing text and inline literals into nearly vertical or excessively narrow
columns, while preserving content-aware column widths and allowing the overall
table to scroll within its container on narrow viewports.
"""

from docutils import nodes
from sphinx.writers.html5 import HTML5Translator

__version__ = "1.0"


MAX_COLUMN_WIDTH = 40


def _content_length(entry):
    """Return a stable character count for rendered cell content."""
    return len(" ".join(entry.astext().split()))


def _width_class(content_length):
    if content_length <= 1:
        return None

    width = min(content_length, MAX_COLUMN_WIDTH)
    return f"table-column-width-{width}"


def _table_rows(tgroup):
    for section in tgroup.children:
        if not isinstance(section, (nodes.thead, nodes.tbody)):
            continue
        for row in section.children:
            if isinstance(row, nodes.row):
                yield row


def _inject_table_column_classes(table):
    tgroup = next(
        (child for child in table.children if isinstance(child, nodes.tgroup)),
        None,
    )
    if tgroup is None:
        return

    column_count = int(tgroup.get("cols", 0))
    if column_count < 1:
        return

    column_lengths = [0] * column_count
    positioned_entries = []
    active_rowspans = [0] * column_count

    for row in _table_rows(tgroup):
        next_rowspans = [max(0, span - 1) for span in active_rowspans]
        column = 0

        for entry in row.children:
            if not isinstance(entry, nodes.entry):
                continue

            while column < column_count and active_rowspans[column]:
                column += 1
            if column >= column_count:
                break

            colspan = int(entry.get("morecols", 0)) + 1
            colspan = min(colspan, column_count - column)
            positioned_entries.append((entry, column, colspan))

            if colspan == 1:
                column_lengths[column] = max(
                    column_lengths[column],
                    _content_length(entry),
                )

            rowspan = int(entry.get("morerows", 0))
            if rowspan:
                for index in range(column, column + colspan):
                    next_rowspans[index] = max(next_rowspans[index], rowspan)

            column += colspan

        active_rowspans = next_rowspans

    column_classes = [_width_class(length) for length in column_lengths]
    for entry, column, colspan in positioned_entries:
        if colspan != 1:
            continue

        class_name = column_classes[column]
        if class_name:
            entry["classes"].append(class_name)


def inject_layout_classes(_app, doctree, _docname):
    """Add generated classes used by the documentation layout CSS."""
    for table in doctree.traverse(nodes.table):
        _inject_table_column_classes(table)


class LayoutInjectionHTMLTranslator(HTML5Translator):
    """Add HTML containers needed for responsive layout behavior."""

    def visit_table(self, node):
        self.body.append('<div class="table-overflow">\n')
        super().visit_table(node)

    def depart_table(self, node):
        super().depart_table(node)
        self.body.append("</div>\n")


def setup(app):
    app.connect("doctree-resolved", inject_layout_classes)

    for builder in ("html", "dirhtml", "singlehtml"):
        app.set_translator(builder, LayoutInjectionHTMLTranslator, override=True)

    return dict(
        version=__version__,
        parallel_read_safe=True,
        parallel_write_safe=True,
    )
