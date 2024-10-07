#! /bin/sh
#
# Extract an RPC protocol specification from an RFC document.
# The version of this script comes from RFC 8166.
#
# Usage:
#  $ extract.sh < rfcNNNN.txt > protocol.x
#

grep '^ *///' | sed 's?^ */// ??' | sed 's?^ *///$??'
