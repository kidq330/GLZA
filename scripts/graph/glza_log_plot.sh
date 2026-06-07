#!/usr/bin/env bash
# Run glza_log_plot.py in a local venv (scripts/.venv); creates it on first use.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="${SCRIPT_DIR}/.venv"
REQ="${SCRIPT_DIR}/requirements-glza-plot.txt"

if [[ ! -d "${VENV}" ]]; then
  echo "Creating venv at ${VENV}" >&2
  python3 -m venv "${VENV}"
fi

if ! "${VENV}/bin/python" -c "import matplotlib" 2>/dev/null; then
  echo "Installing dependencies from ${REQ}" >&2
  "${VENV}/bin/pip" install -q -r "${REQ}"
fi

exec "${VENV}/bin/python" "${SCRIPT_DIR}/glza_log_plot.py" "$@"
