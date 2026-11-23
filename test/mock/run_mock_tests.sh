#!/usr/bin/env bash
# Starts the mock Google server, points the ADC environment at it, and runs the mock sqllogictests.
# Usage: test/mock/run_mock_tests.sh [build dir, default build/release] [test pattern]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${1:-build/release}"
PATTERN="${2:-test/sql/mock/*}"
WORK="$(mktemp -d)"
PORT_FILE="$WORK/port"

python3 "$ROOT/test/mock/mock_google.py" --dir "$WORK/fixtures" --port-file "$PORT_FILE" &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

for _ in $(seq 1 50); do
	[ -s "$PORT_FILE" ] && break
	sleep 0.1
done
PORT="$(cat "$PORT_FILE")"

export GCS_ADC_MOCK_PORT="$PORT"
export GCS_ADC_MOCK_DIR="$WORK/fixtures"
export GOOGLE_APPLICATION_CREDENTIALS="$WORK/fixtures/authorized_user.json"
export CLOUDSDK_CONFIG="$WORK/fixtures/gcloud"
export GCE_METADATA_HOST="localhost:$PORT"
unset NO_GCE_CHECK

cd "$ROOT"
# The sqllogictest runner turns HTTP errors into skipped tests; against a local mock a skip is a failure
OUTPUT="$WORK/unittest.log"
set +e
"$BUILD/test/unittest" "$PATTERN" 2>&1 | tee "$OUTPUT"
STATUS=${PIPESTATUS[0]}
set -e
if [ "$STATUS" -ne 0 ]; then
	exit "$STATUS"
fi
if grep -q "skipped" "$OUTPUT"; then
	echo "run_mock_tests: some mock tests were skipped, which means a request to the mock failed" >&2
	exit 1
fi
