#!/usr/bin/env bash
export LC_ALL=C.UTF-8
# Run Grafana locally against the live statoshi.info Graphite data.
#
# The dashboards in dashboards/*.json are mounted read-only, so this serves
# exactly what is checked into the repo. Data comes from statoshi.info's public
# Graphite proxy, so panels show production metrics with no local node needed.
#
# The checked-in dashboards use the classic dashboard model (schemaVersion 42,
# a top-level "panels" array, and a "uid"), which is what Grafana's UI import
# page and the classic /api/dashboards/* endpoints accept:
#
#   { "uid": "<uid>", "panels": [ ... ], "schemaVersion": 42, ... }
#
# Two different Grafana surfaces want two different formats, so do not change
# the format of these files:
#
#   * Dashboards > New > Import (and /api/dashboards/import) is classic-only.
#     A resource-form file is rejected with "Dashboard using new layout cannot
#     be imported because the feature is not enabled" (or "Invalid JSON" in
#     some paths; server-side: "dashboard appears to be in v2 format"), and
#     wrapping it for /api/dashboards/import returns 200 and "imported": true
#     while silently storing zero panels.
#
#   * The dashboard editor, when the dashboardNewLayouts feature toggle is on
#     (as it is on statoshi.info), edits the v2 resource shape. Pasting a
#     classic file there fails schema validation with "The required property
#     `metadata` is missing" / "`spec` is missing".
#
# So these files stay classic, and local/to-v2.py converts a file on demand
# for pasting into the editor:
#
#   ./local/to-v2.py ../blocks.json > /tmp/blocks-v2.json
#   ./local/to-v2.py --paste ../blocks.json
#
# Usage: dashboards/local/run.sh [port]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dashboards="$(cd "$here/.." && pwd)"
port="${1:-3000}"
name="statoshi-grafana"

if ! command -v docker >/dev/null 2>&1; then
  echo "error: docker not found in PATH" >&2
  exit 1
fi

# Fail early with a clear message rather than rendering empty panels.
if ! curl -fsS -m 10 -o /dev/null \
    "https://statoshi.info/api/datasources/proxy/uid/000000001/metrics/find?query=stats.gauges.blocks.currentHeight"; then
  echo "warning: statoshi.info Graphite proxy is not reachable right now." >&2
  echo "         Grafana will still start, but panels will show 'No data'." >&2
fi

docker rm -f "$name" >/dev/null 2>&1 || true

exec docker run --rm \
  --name "$name" \
  -p "${port}:3000" \
  -e GF_AUTH_ANONYMOUS_ENABLED=true \
  -e GF_AUTH_ANONYMOUS_ORG_ROLE=Admin \
  -e GF_AUTH_DISABLE_LOGIN_FORM=true \
  -e GF_USERS_ALLOW_SIGN_UP=false \
  -e GF_ANALYTICS_REPORTING_ENABLED=false \
  -e GF_ANALYTICS_CHECK_FOR_UPDATES=false \
  -e GF_ANALYTICS_CHECK_FOR_PLUGIN_UPDATES=false \
  -e GF_LOG_LEVEL=warn \
  -e GF_DASHBOARDS_DEFAULT_HOME_DASHBOARD_PATH=/var/lib/grafana/dashboards/home.json \
  -v "$here/provisioning:/etc/grafana/provisioning:ro" \
  -v "$dashboards:/var/lib/grafana/dashboards:ro" \
  grafana/grafana:13.3.1
