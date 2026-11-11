#!/usr/bin/env bash
set -euo pipefail

python -m unittest discover -s tests -p "test_registry_metadata.py"
python -m unittest discover -s tests -p "test_package_links.py"
