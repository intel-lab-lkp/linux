"""
Sphinx extension for processing YAML files
"""

import os
import re
import sys

from pprint import pformat

from docutils.parsers.rst import Parser as RSTParser
from docutils.statemachine import ViewList

from sphinx.util import logging
from sphinx.parsers import Parser

srctree = os.path.abspath(os.environ["srctree"])
sys.path.insert(0, os.path.join(srctree, "scripts/lib"))

from netlink_yml_parser import NetlinkYamlParser      # pylint: disable=C0413

logger = logging.getLogger(__name__)

class YamlParser(Parser):
    """Custom parser for YAML files."""

    supported = ('yaml', 'yml')

    netlink_parser = NetlinkYamlParser()

    def do_parse(self, inputstring, document, msg):
        """Parse YAML and generate a document tree."""

        self.setup_parse(inputstring, document)

        result = ViewList()

        try:
            # Parse message with RSTParser
            for i, line in enumerate(msg.split('\n')):
                result.append(line, document.current_source, i)

            rst_parser = RSTParser()
            rst_parser.parse('\n'.join(result), document)

        except Exception as e:
            document.reporter.error("YAML parsing error: %s" % pformat(e))

        self.finish_parse()

    # Overrides docutils.parsers.Parser. See sphinx.parsers.RSTParser
    def parse(self, inputstring, document):
        """Check if a YAML is meant to be parsed."""

        fname = document.current_source

        # Handle netlink yaml specs
        if re.search("/netlink/specs/", fname):
            if fname.endswith("index.yaml"):
                msg = self.netlink_parser.generate_main_index_rst(fname, None)
            else:
                msg = self.netlink_parser.parse_yaml_file(fname)

            self.do_parse(inputstring, document, msg)

        # All other yaml files are ignored

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
