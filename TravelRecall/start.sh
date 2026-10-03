#!/bin/sh
# TravelRecall launcher for macOS / Linux: serves this folder on http://localhost and opens the browser.
cd "$(dirname "$0")" || exit 1
PORT=${1:-8765}
URL="http://localhost:$PORT/index.html"
if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 not found. Open index.html directly instead (use the Esri map styles)."
  exit 1
fi
echo "TravelRecall is running at $URL  (Ctrl+C to stop)"
( sleep 1; (command -v open >/dev/null && open "$URL") || xdg-open "$URL" >/dev/null 2>&1 ) &
exec python3 -m http.server "$PORT" --bind 127.0.0.1
