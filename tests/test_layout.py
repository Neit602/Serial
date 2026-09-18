"""The src layout the PRD mandates must stay intact."""

from __future__ import annotations

import importlib
from pathlib import Path

import pytest

PACKAGES = [
    "portside.core",
    "portside.transport",
    "portside.emulation",
    "portside.transfer",
    "portside.profiles",
    "portside.logging",
    "portside.platform",
    "portside.gui",
    "portside.tui",
    "portside.cli",
]


@pytest.mark.parametrize("name", PACKAGES)
def test_every_package_imports(name: str) -> None:
    assert importlib.import_module(name) is not None


def test_package_is_typed() -> None:
    import portside

    root = Path(portside.__file__).parent
    assert (root / "py.typed").is_file()


def test_portside_logging_does_not_shadow_stdlib() -> None:
    """Our `portside.logging` package must not break `import logging`."""
    import logging

    import portside.logging as portside_logging

    assert logging.getLogger("portside") is not None
    assert portside_logging is not logging
