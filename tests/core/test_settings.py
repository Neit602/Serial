"""Line settings and their validation (F3)."""

from __future__ import annotations

import pytest
from hypothesis import given
from hypothesis import strategies as st

from portside.core.errors import InvalidSettingsError
from portside.core.settings import (
    BAUD_MAX,
    BAUD_MIN,
    DATA_BITS_CHOICES,
    FlowControl,
    Parity,
    SerialSettings,
    StopBits,
)


def test_defaults_are_115200_8n1() -> None:
    settings = SerialSettings()
    assert settings.baudrate == 115200
    assert settings.describe() == "115200 8N1 none"


@pytest.mark.parametrize("baud", [BAUD_MIN, 9600, 115200, 921600, BAUD_MAX])
def test_supported_baud_rates_are_accepted(baud: int) -> None:
    assert SerialSettings(baudrate=baud).baudrate == baud


@pytest.mark.parametrize("baud", [0, 49, BAUD_MAX + 1, -115200])
def test_out_of_range_baud_is_rejected(baud: int) -> None:
    with pytest.raises(InvalidSettingsError):
        SerialSettings(baudrate=baud)


@pytest.mark.parametrize("bits", [4, 9, 0])
def test_bad_data_bits_are_rejected(bits: int) -> None:
    with pytest.raises(InvalidSettingsError):
        SerialSettings(data_bits=bits)


def test_non_integer_baud_is_rejected() -> None:
    with pytest.raises(InvalidSettingsError):
        SerialSettings(baudrate=115200.5)  # type: ignore[arg-type]
    with pytest.raises(InvalidSettingsError):
        SerialSettings(baudrate=True)  # bool is an int subclass, and 1 is not a baud rate


def test_unknown_enum_values_are_rejected() -> None:
    with pytest.raises(InvalidSettingsError):
        SerialSettings(parity="Z")  # type: ignore[arg-type]
    with pytest.raises(InvalidSettingsError):
        SerialSettings(flow_control="hardware")  # type: ignore[arg-type]


def test_every_parity_and_stop_bit_combination_is_expressible() -> None:
    for parity in Parity:
        for stop in StopBits:
            for flow in FlowControl:
                settings = SerialSettings(parity=parity, stop_bits=stop, flow_control=flow)
                assert settings.parity is parity
                assert settings.stop_bits is stop
                assert settings.flow_control is flow


def test_pyserial_kwargs_map_flow_control_exclusively() -> None:
    rtscts = SerialSettings(flow_control=FlowControl.RTSCTS).to_pyserial_kwargs()
    assert (rtscts["rtscts"], rtscts["dsrdtr"], rtscts["xonxoff"]) == (True, False, False)
    xon = SerialSettings(flow_control=FlowControl.XONXOFF).to_pyserial_kwargs()
    assert (xon["rtscts"], xon["dsrdtr"], xon["xonxoff"]) == (False, False, True)
    dsr = SerialSettings(flow_control=FlowControl.DSRDTR).to_pyserial_kwargs()
    assert (dsr["rtscts"], dsr["dsrdtr"], dsr["xonxoff"]) == (False, True, False)


def test_evolve_revalidates() -> None:
    settings = SerialSettings()
    assert settings.evolve(baudrate=9600).baudrate == 9600
    with pytest.raises(InvalidSettingsError):
        settings.evolve(baudrate=1)


def test_settings_are_immutable_and_hashable() -> None:
    settings = SerialSettings()
    with pytest.raises(AttributeError):
        settings.baudrate = 9600  # type: ignore[misc]
    assert {settings, SerialSettings()} == {settings}


@given(
    baud=st.integers(min_value=BAUD_MIN, max_value=BAUD_MAX),
    data_bits=st.sampled_from(DATA_BITS_CHOICES),
    parity=st.sampled_from(list(Parity)),
    stop_bits=st.sampled_from(list(StopBits)),
    flow=st.sampled_from(list(FlowControl)),
)
def test_round_trip_through_dict(
    baud: int, data_bits: int, parity: Parity, stop_bits: StopBits, flow: FlowControl
) -> None:
    """Profiles (M2) will store these as TOML, so the round trip must be exact."""
    original = SerialSettings(
        baudrate=baud,
        data_bits=data_bits,
        parity=parity,
        stop_bits=stop_bits,
        flow_control=flow,
    )
    assert SerialSettings.from_dict(original.to_dict()) == original


@given(baud=st.integers().filter(lambda b: not BAUD_MIN <= b <= BAUD_MAX))
def test_any_out_of_range_baud_raises(baud: int) -> None:
    with pytest.raises(InvalidSettingsError):
        SerialSettings(baudrate=baud)


def test_from_dict_rejects_unknown_values() -> None:
    with pytest.raises(InvalidSettingsError):
        SerialSettings.from_dict({"parity": "Q"})
    with pytest.raises(InvalidSettingsError):
        SerialSettings.from_dict({"stop_bits": 3})
