"""Fixtures for the Qt tests.

Everything here runs headless (``QT_QPA_PLATFORM=offscreen``), so the GUI is
tested on all three CI platforms without a display.
"""

from __future__ import annotations

import os
from collections.abc import Iterator

import pytest

# Must be set before Qt is imported anywhere.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from portside.core.session import Session
from portside.gui.controller import SessionController
from portside.gui.runtime import AsyncRuntime
from portside.transport.loopback import LoopbackTransport

pytestmark = pytest.mark.gui


@pytest.fixture
def runtime() -> Iterator[AsyncRuntime]:
    """An asyncio loop on its own thread, stopped afterwards."""
    with AsyncRuntime() as started:
        yield started


@pytest.fixture
def transport() -> LoopbackTransport:
    """A loopback 'device' the GUI can talk to without hardware."""
    return LoopbackTransport(echo=False)


@pytest.fixture
def controller(runtime: AsyncRuntime, transport: LoopbackTransport) -> Iterator[SessionController]:
    """An open session, wrapped for Qt."""
    session = Session(transport)
    runtime.run(session.open())
    wrapped = SessionController(session, runtime)
    try:
        yield wrapped
    finally:
        wrapped.close()
