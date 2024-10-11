#!/usr/bin/env sh

DIR="$(dirname -- "$0")"

PATH="${PATH}:${DIR}"

set -x
"${DIR}/script-ask.inc"
