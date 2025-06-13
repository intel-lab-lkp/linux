"""
Sphinx extension for processing YAML files
"""

import os

from docutils.parsers.rst import Parser as RSTParser
from docutils.statemachine import ViewList

from sphinx.util import logging
from sphinx.parsers import Parser

from pprint import pformat

logger = logging.getLogger(__name__)

class YamlParser(Parser):
    """Custom parser for YAML files."""

    supported = ('yaml', 'yml')

    # Overrides docutils.parsers.Parser. See sphinx.parsers.RSTParser
    def parse(self, inputstring, document):
        """Parse YAML and generate a document tree."""

        self.setup_parse(inputstring, document)

        result = ViewList()

        try:
            # FIXME: Test logic to generate some ReST content
            basename = os.path.basename(document.current_source)
            title = os.path.splitext(basename)[0].replace('_', ' ').title()

            msg = f"{title}\n"
            msg += "=" * len(title) + "\n\n"
            msg += "Something\n"

            # Parse message with RSTParser
            for i, line in enumerate(msg.split('\n')):
                result.append(line, document.current_source, i)

            rst_parser = RSTParser()
            rst_parser.parse('\n'.join(result), document)

        except Exception as e:
            document.reporter.error("YAML parsing error: %s" % pformat(e))

        self.finish_parse()

def setup(app):
    """Setup function for the Sphinx extension."""

    # Add YAML parser
    app.add_source_parser(YamlParser)
    app.add_source_suffix('.yaml', 'yaml')
    app.add_source_suffix('.yml', 'yaml')

    return {
        'version': '1.0',
        'parallel_read_safe': True,
        'parallel_write_safe': True,
    }
