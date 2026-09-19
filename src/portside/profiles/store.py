"""Named TOML profiles (PRD F20).

A profile is everything a session needs to behave the way a given device wants:
line settings, line discipline, break mode, pacing, logging and terminal
options.  Profiles are keyed by :class:`~portside.core.identity.DeviceIdentity`
so the right one is applied when an adapter comes back on a different port name
(F2), on any of the three platforms.

Files are plain TOML in the platform's config directory, with a
``schema_version`` so a future Portside can migrate them — and so an *older*
Portside refuses a file it does not understand instead of silently misreading
it.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

import tomli_w

from portside.core.breaks import BreakMode
from portside.core.errors import PortsideError
from portside.core.identity import DeviceIdentity
from portside.core.pacing import PacePolicy
from portside.core.settings import SerialSettings
from portside.core.textio import Backspace, LineEnding, TextPolicy
from portside.emulation.decoder import CharacterSet
from portside.emulation.terminal import DEFAULT_SCROLLBACK
from portside.logging.session_log import DEFAULT_TEMPLATE, LogFormat, LogSettings
from portside.logging.timestamps import TimestampMode

__all__ = ["SCHEMA_VERSION", "Profile", "ProfileError", "ProfileStore", "TerminalOptions"]

#: Bumped only for changes that older versions cannot read.
SCHEMA_VERSION = 1

PROFILE_SUFFIX = ".toml"


class ProfileError(PortsideError):
    """A profile could not be read, written or understood."""


@dataclass(frozen=True, slots=True)
class TerminalOptions:
    """Terminal-side settings a profile carries (F7, F8, F10, F11).

    ``font_family`` empty means "the platform's monospace font", which is the
    only portable default: no font name exists on all three OSes.
    """

    scrollback: int = DEFAULT_SCROLLBACK
    charset: CharacterSet = CharacterSet.UTF8
    strip_high_bit: bool = False
    columns: int = 80
    rows: int = 24
    font_family: str = ""
    font_size: int = 11
    theme: str = "dark"


@dataclass(frozen=True, slots=True)
class Profile:
    """A named bundle of settings, optionally bound to a device (F20)."""

    name: str
    serial: SerialSettings = field(default_factory=SerialSettings)
    text: TextPolicy = field(default_factory=TextPolicy)
    terminal: TerminalOptions = field(default_factory=TerminalOptions)
    pacing: PacePolicy = field(default_factory=PacePolicy)
    logging: LogSettings = field(default_factory=LogSettings)
    break_mode: BreakMode = BreakMode.AUTO
    break_duration: float = 0.25
    #: Device identity keys this profile auto-applies to (F2 + F20).
    device_keys: tuple[str, ...] = ()
    description: str = ""

    def with_device(self, identity: DeviceIdentity) -> Profile:
        """Return a copy bound to ``identity`` as well as its current devices."""
        key = identity.key()
        if key in self.device_keys:
            return self
        return replace(self, device_keys=(*self.device_keys, key))

    def matches(self, identity: DeviceIdentity | None) -> bool:
        """Whether this profile should auto-apply to ``identity``."""
        return identity is not None and identity.key() in self.device_keys

    # -- serialisation -----------------------------------------------------

    def to_dict(self) -> dict[str, Any]:
        """TOML-ready mapping (no ``None`` values: TOML has no null)."""
        return {
            "schema_version": SCHEMA_VERSION,
            "name": self.name,
            "description": self.description,
            "device_keys": list(self.device_keys),
            "serial": self.serial.to_dict(),
            "text": {
                "send_ending": self.text.send_ending.value,
                "receive_ending": self.text.receive_ending.value,
                "local_echo": self.text.local_echo,
                "backspace": self.text.backspace.value,
            },
            "terminal": {
                "scrollback": self.terminal.scrollback,
                "charset": self.terminal.charset.value,
                "strip_high_bit": self.terminal.strip_high_bit,
                "columns": self.terminal.columns,
                "rows": self.terminal.rows,
                "font_family": self.terminal.font_family,
                "font_size": self.terminal.font_size,
                "theme": self.terminal.theme,
            },
            "pacing": {
                "char_delay": self.pacing.char_delay,
                "line_delay": self.pacing.line_delay,
                "wait_for_prompt": self.pacing.wait_for_prompt or "",
                "prompt_timeout": self.pacing.prompt_timeout,
                "abort_on_timeout": self.pacing.abort_on_timeout,
            },
            "logging": {
                "directory": str(self.logging.directory) if self.logging.directory else "",
                "template": self.logging.template,
                "format": self.logging.log_format.value,
                "timestamps": self.logging.timestamps.value,
                "charset": self.logging.charset.value,
                "auto_start": self.logging.auto_start,
                "max_bytes": self.logging.max_bytes,
                "keep": self.logging.keep,
            },
            "break": {"mode": self.break_mode.value, "duration": self.break_duration},
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> Profile:
        """Rebuild a profile, rejecting files from a newer Portside."""
        version = int(data.get("schema_version", SCHEMA_VERSION))
        if version > SCHEMA_VERSION:
            raise ProfileError(
                f"profile uses schema version {version}, but this Portside understands "
                f"up to {SCHEMA_VERSION}",
                remedy="Update Portside, or export the profile again from the older version.",
            )
        try:
            text = data.get("text", {})
            terminal = data.get("terminal", {})
            pacing = data.get("pacing", {})
            logs = data.get("logging", {})
            breaks = data.get("break", {})
            prompt = str(pacing.get("wait_for_prompt", "")) or None
            directory = str(logs.get("directory", "")) or None
            return cls(
                name=str(data["name"]),
                description=str(data.get("description", "")),
                device_keys=tuple(str(key) for key in data.get("device_keys", [])),
                serial=SerialSettings.from_dict(data.get("serial", {})),
                text=TextPolicy(
                    send_ending=LineEnding(text.get("send_ending", LineEnding.CR.value)),
                    receive_ending=LineEnding(text.get("receive_ending", LineEnding.NONE.value)),
                    local_echo=bool(text.get("local_echo", False)),
                    backspace=Backspace(text.get("backspace", Backspace.DEL.value)),
                ),
                terminal=TerminalOptions(
                    scrollback=int(terminal.get("scrollback", DEFAULT_SCROLLBACK)),
                    charset=CharacterSet(terminal.get("charset", CharacterSet.UTF8.value)),
                    strip_high_bit=bool(terminal.get("strip_high_bit", False)),
                    columns=int(terminal.get("columns", 80)),
                    rows=int(terminal.get("rows", 24)),
                    font_family=str(terminal.get("font_family", "")),
                    font_size=int(terminal.get("font_size", 11)),
                    theme=str(terminal.get("theme", "dark")),
                ),
                pacing=PacePolicy(
                    char_delay=float(pacing.get("char_delay", 0.0)),
                    line_delay=float(pacing.get("line_delay", 0.05)),
                    wait_for_prompt=prompt,
                    prompt_timeout=float(pacing.get("prompt_timeout", 5.0)),
                    abort_on_timeout=bool(pacing.get("abort_on_timeout", True)),
                ),
                logging=LogSettings(
                    directory=Path(directory) if directory else None,
                    template=str(logs.get("template", DEFAULT_TEMPLATE)),
                    log_format=LogFormat(logs.get("format", LogFormat.TEXT.value)),
                    timestamps=TimestampMode(logs.get("timestamps", TimestampMode.OFF.value)),
                    charset=CharacterSet(logs.get("charset", CharacterSet.UTF8.value)),
                    auto_start=bool(logs.get("auto_start", False)),
                    max_bytes=int(logs.get("max_bytes", 0)),
                    keep=int(logs.get("keep", 5)),
                ),
                break_mode=BreakMode(breaks.get("mode", BreakMode.AUTO.value)),
                break_duration=float(breaks.get("duration", 0.25)),
            )
        except ProfileError:
            raise
        except (KeyError, TypeError, ValueError) as exc:
            raise ProfileError(f"malformed profile: {exc}") from exc
        except PortsideError as exc:
            # A validation failure deeper down (a bad baud rate, say) is still a
            # bad profile from the user's point of view, and the message says why.
            raise ProfileError(f"invalid profile: {exc}") from exc


class ProfileStore:
    """Profiles on disk, one TOML file each (F20)."""

    def __init__(self, directory: Path | None = None) -> None:
        if directory is None:
            from portside.platform import get_platform

            directory = get_platform().config_dir() / "profiles"
        self._directory = directory

    @property
    def directory(self) -> Path:
        """Where profiles live on this platform."""
        return self._directory

    def path_for(self, name: str) -> Path:
        """File backing the profile called ``name``."""
        from portside.logging.session_log import _sanitise

        return self._directory / f"{_sanitise(name)}{PROFILE_SUFFIX}"

    def names(self) -> list[str]:
        """Every profile name, alphabetically."""
        if not self._directory.is_dir():
            return []
        return sorted(path.stem for path in self._directory.glob(f"*{PROFILE_SUFFIX}"))

    def save(self, profile: Profile) -> Path:
        """Write ``profile``, creating the directory if needed."""
        path = self.path_for(profile.name)
        path.parent.mkdir(parents=True, exist_ok=True)
        self.export(profile, path)
        return path

    def load(self, name: str) -> Profile:
        """Read the profile called ``name``."""
        path = self.path_for(name)
        if not path.is_file():
            raise ProfileError(
                f"no profile named {name!r}",
                remedy=f"Profiles live in {self._directory}. Use 'portside profiles' to list them.",
            )
        return self.import_file(path)

    def delete(self, name: str) -> None:
        """Remove a profile; missing profiles are not an error."""
        self.path_for(name).unlink(missing_ok=True)

    def all(self) -> list[Profile]:
        """Every readable profile.  Unreadable ones are skipped, not fatal."""
        profiles: list[Profile] = []
        for name in self.names():
            try:
                profiles.append(self.load(name))
            except ProfileError:
                continue
        return profiles

    def for_device(self, identity: DeviceIdentity | None) -> Profile | None:
        """The profile bound to ``identity``, if any (F2 + F20 auto-apply)."""
        if identity is None:
            return None
        for profile in self.all():
            if profile.matches(identity):
                return profile
        return None

    # -- import / export ---------------------------------------------------

    def export(self, profile: Profile, path: Path) -> Path:
        """Write ``profile`` to an arbitrary path (F20 import/export)."""
        try:
            path.write_bytes(tomli_w.dumps(profile.to_dict()).encode("utf-8"))
        except OSError as exc:
            raise ProfileError(f"could not write {path}: {exc}") from exc
        return path

    def import_file(self, path: Path) -> Profile:
        """Read a profile from an arbitrary path (F20 import/export)."""
        try:
            data = tomllib.loads(path.read_text(encoding="utf-8"))
        except OSError as exc:
            raise ProfileError(f"could not read {path}: {exc}") from exc
        except tomllib.TOMLDecodeError as exc:
            raise ProfileError(f"{path} is not valid TOML: {exc}") from exc
        data.setdefault("name", path.stem)
        return Profile.from_dict(data)
