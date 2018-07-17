#!/bin/bash
set -e
set -x

SCRIPT_DIR="`dirname "$0"`" 

find . -name "*.[c,h]" | xargs -n 1 unifdef -m -f "$SCRIPT_DIR"/defile
