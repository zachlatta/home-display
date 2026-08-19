#!/bin/sh
# Mechanical backstop before pushing to a public repository.
#
# This is a floor, not a ceiling. It cannot recognise a real person's name in a
# fixture or a paraphrased private message, so AGENTS.md still requires reading
# the full diff. What it does catch is the mistakes that are easy to make and
# expensive to undo.
#
#   sh scripts/audit-public.sh          audit tracked files
#   sh scripts/audit-public.sh --staged audit what is about to be committed
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

FAILURES=0
fail() {
  printf 'FAIL  %s\n' "$1"
  FAILURES=$((FAILURES + 1))
}
pass() { printf 'ok    %s\n' "$1"; }

if [ "${1:-}" = "--staged" ]; then
  FILES=$(git diff --cached --name-only --diff-filter=ACM)
  LABEL="staged files"
else
  FILES=$(git ls-files)
  LABEL="tracked files"
fi

# Text files only: fixture photo payloads are base64 and produce endless false
# positives on any high-entropy pattern.
TEXT_FILES=$(printf '%s\n' "$FILES" | grep -vE '^sim/fixtures/.*\.json$' | grep -vE '^sim/vendor/' || true)

printf 'auditing %s\n\n' "$LABEL"

# --- 1. files that must never be tracked --------------------------------------
FORBIDDEN=$(printf '%s\n' "$FILES" | grep -E '(^|/)(\.secrets/|\.state/|\.env$|secrets\.h$)' || true)
if [ -n "$FORBIDDEN" ]; then
  fail "credential files are tracked:"
  printf '%s\n' "$FORBIDDEN" | sed 's/^/        /'
else
  pass "no credential files tracked"
fi

# --- 2. rendered output, which may show real data -----------------------------
RENDERS=$(printf '%s\n' "$FILES" | grep -E '^sim/out/' || true)
if [ -n "$RENDERS" ]; then
  fail "rendered output is tracked (may contain real data):"
  printf '%s\n' "$RENDERS" | sed 's/^/        /'
else
  pass "no rendered output tracked"
fi

# --- 3. credential-shaped content ---------------------------------------------
if [ -n "$TEXT_FILES" ]; then
  HITS=$(printf '%s\n' "$TEXT_FILES" | xargs grep -nIE \
    -e 'BEGIN [A-Z ]*PRIVATE KEY' \
    -e 'gh[pousr]_[A-Za-z0-9]{20,}' \
    -e 'xox[abps]-[A-Za-z0-9-]{10,}' \
    -e 'sk-[A-Za-z0-9]{20,}' \
    -e 'AKIA[0-9A-Z]{16}' \
    2>/dev/null || true)
  if [ -n "$HITS" ]; then
    fail "credential-shaped strings found:"
    printf '%s\n' "$HITS" | sed 's/^/        /'
  else
    pass "no credential-shaped strings"
  fi
fi

# --- 4. the live secrets, if this checkout has them ---------------------------
CHECKED_LIVE=0
for SECRET_FILE in .secrets/dashboard-token .secrets/setup-password; do
  [ -f "$SECRET_FILE" ] || continue
  VALUE=$(tr -d '\n' < "$SECRET_FILE")
  [ ${#VALUE} -ge 8 ] || continue
  CHECKED_LIVE=1
  if [ -n "$TEXT_FILES" ] && printf '%s\n' "$TEXT_FILES" | xargs grep -qIF "$VALUE" 2>/dev/null; then
    fail "the live value of $SECRET_FILE appears in tracked content"
  fi
done
[ "$CHECKED_LIVE" = 1 ] && pass "live secret values absent from tracked content"

# --- 5. internal network topology ---------------------------------------------
if [ -n "$TEXT_FILES" ]; then
  TOPOLOGY=$(printf '%s\n' "$TEXT_FILES" | xargs grep -nIE \
    -e '[a-z0-9-]+\.ts\.net' \
    -e '\b(10\.[0-9]{1,3}|192\.168|172\.(1[6-9]|2[0-9]|3[01]))\.[0-9]{1,3}\.[0-9]{1,3}\b' \
    2>/dev/null | grep -v '192\.168\.4\.1' || true)
  if [ -n "$TOPOLOGY" ]; then
    fail "internal hostnames or private IPs found:"
    printf '%s\n' "$TOPOLOGY" | sed 's/^/        /'
  else
    pass "no internal hostnames or private IPs"
  fi
fi

# --- 6. real-looking Slack identifiers ----------------------------------------
# Invented ids used by fixtures and tests spell EXAMPLE or UNKNOWN.
if [ -n "$TEXT_FILES" ]; then
  SLACK=$(printf '%s\n' "$TEXT_FILES" | xargs grep -noIE '\b[UWC]0[A-Z0-9]{8,}\b' 2>/dev/null \
    | grep -viE 'EXAMPLE|UNKNOWN|ZZZZ' || true)
  if [ -n "$SLACK" ]; then
    fail "real-looking Slack identifiers found (fixtures must use invented ids):"
    printf '%s\n' "$SLACK" | sed 's/^/        /'
  else
    pass "no real-looking Slack identifiers"
  fi
fi

# --- 7. fixtures must be generated, never pasted from a live payload ----------
if git ls-files --error-unmatch sim/make-fixtures.mjs >/dev/null 2>&1; then
  if grep -q 'never be seeded from real payloads' sim/make-fixtures.mjs; then
    pass "fixture generator carries its invented-data warning"
  else
    fail "sim/make-fixtures.mjs lost its invented-data warning"
  fi
fi

printf '\n'
if [ "$FAILURES" -gt 0 ]; then
  printf '%s check(s) failed — do not push\n' "$FAILURES"
  exit 1
fi
printf 'mechanical checks passed — now read the full diff before pushing\n'
