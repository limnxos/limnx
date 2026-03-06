#!/bin/bash
set -e

DEPS_DIR="$(dirname "$0")/deps"
mkdir -p "$DEPS_DIR"

# Limine protocol header
if [ ! -f "$DEPS_DIR/limine.h" ]; then
    echo "Fetching limine.h..."
    curl -sSL "https://raw.githubusercontent.com/limine-bootloader/limine/v8.x/limine.h" \
        -o "$DEPS_DIR/limine.h"
    echo "  -> $DEPS_DIR/limine.h"
fi

echo "All dependencies ready in $DEPS_DIR/"
