from __future__ import annotations

from importlib.metadata import version
from pathlib import Path

import pynumstore as ns

# The one source of truth for both the C and the Python version.
VERSION_TXT = Path(__file__).resolve().parent.parent / "version.txt"


def _expected():
    return VERSION_TXT.read_text().strip().lstrip("v")


class TestVersion:
    def test_wheel_version_matches_version_txt(self):
        assert version("pynumstore") == _expected()

    def test_package_exposes_dunder_version(self):
        assert ns.__version__ == _expected()
