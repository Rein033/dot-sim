"""The USB cable, from a page: the computer on it plugs in, enumerates what
answers (the fake esp-emu is a boot keyboard) and shows what it types."""

import json

import pytest

import kd
from sim import next_json, ws


def usb(j):
    return "usb" in j


async def _send(w, cmd):
    await w.send_str(json.dumps({"usb": cmd}))


async def test_a_page_plugs_the_cable_in_and_the_computer_enumerates_the_keyboard(sim):
    w = await ws(sim)
    first = await next_json(w, usb)
    assert first["usb"]["stage"] == "no_cable"
    await _send(w, {"cmd": "plug"})
    u = (await next_json(w, lambda j: usb(j) and j["usb"]["stage"] == "keyboard"))["usb"]
    assert u["cable"] and u["device"]["class"] == "keyboard" and u["device"]["product"] == "Fake"
    assert u["device"]["vid"] == "306a"
    await w.close()
    await w.session.close()


async def test_what_the_keyboard_reports_is_typed_on_every_page_and_a_late_one_is_told(sim):
    a = await ws(sim)
    await _send(a, {"cmd": "plug"})
    await next_json(a, lambda j: usb(j) and j["usb"]["stage"] == "keyboard")
    await a.send_str(json.dumps({"console_in": "key 2 11"}))   # shift + h
    await a.send_str(json.dumps({"console_in": "key 0 12"}))   # i
    u = (await next_json(a, lambda j: usb(j) and j["usb"]["typed"] == "Hi"))["usb"]
    assert u["layout"] == "es"
    late = await ws(sim)
    told = (await next_json(late, usb))["usb"]
    assert told["typed"] == "Hi" and told["stage"] == "keyboard", "a page that joins late is sent the cable's state"
    for w in (a, late):
        await w.close()
        await w.session.close()


async def test_pulling_the_cable_out_forgets_the_device(sim):
    w = await ws(sim)
    await _send(w, {"cmd": "plug"})
    await next_json(w, lambda j: usb(j) and j["usb"]["stage"] == "keyboard")
    await _send(w, {"cmd": "unplug"})
    u = (await next_json(w, lambda j: usb(j) and j["usb"]["stage"] == "no_cable"))["usb"]
    assert not u["cable"] and "device" not in u
    await w.close()
    await w.session.close()


async def test_a_command_it_does_not_know_does_nothing(sim):
    w = await ws(sim)
    await _send(w, {"cmd": "format_everything"})
    await _send(w, {"cmd": "put", "name": "a", "data": "not base64!"})
    await _send(w, {"cmd": "plug"})
    u = (await next_json(w, lambda j: usb(j) and j["usb"]["cable"]))["usb"]
    assert u["stage"] in ("waiting", "enumerating", "keyboard")
    await w.close()
    await w.session.close()
