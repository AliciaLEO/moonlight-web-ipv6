#!/usr/bin/env bash
# README.md describes the latest release; README.next.md is where the next one's
# README is written. A release copies the draft over, minus its draft banner.
#   bash scripts/promote-readme.sh          # write README.md, commit it before the tag
#   bash scripts/promote-readme.sh --check  # CI on a v* tag: fail if not promoted
set -euo pipefail
cd "$(dirname "$0")/.."

promoted() {
    sed '/^<!-- next-readme:begin/,/^<!-- next-readme:end/d' README.next.md | sed '1{/^$/d}'
}

if [ "${1:-}" = "--check" ]; then
    # Line endings aside: a Windows checkout may hold CRLF on one side only.
    if ! cmp -s <(promoted | tr -d '\r') <(tr -d '\r' < README.md); then
        echo "README.md is not README.next.md promoted." >&2
        echo "Run 'bash scripts/promote-readme.sh', commit README.md, then tag." >&2
        exit 1
    fi
    echo "README.md matches README.next.md."
    exit 0
fi

promoted > README.md
echo "README.md promoted from README.next.md."
