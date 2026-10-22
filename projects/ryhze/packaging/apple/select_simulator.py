"""Select an already installed iPhone simulator from completed simctl JSON."""

import argparse
import json
from pathlib import Path
import re
import sys


RUNTIME = re.compile(r"com\.apple\.CoreSimulator\.SimRuntime\.iOS-[0-9]+(?:-[0-9]+)*")
UDID = re.compile(r"[A-Fa-f0-9]{8}(?:-[A-Fa-f0-9]{4}){3}-[A-Fa-f0-9]{12}")
STATES = {"Booted", "Shutdown", "Booting", "Shutting Down"}
MAX_JSON_BYTES = 4 * 1024 * 1024


def select_device(value):
    if not isinstance(value, dict) or not isinstance(value.get("devices"), dict):
        raise ValueError("Simulator enumeration has no valid device map.")
    choices = []
    for runtime, devices in value["devices"].items():
        if not isinstance(runtime, str) or not RUNTIME.fullmatch(runtime):
            continue
        if not isinstance(devices, list):
            raise ValueError("Simulator enumeration has an invalid iOS device list.")
        for device in devices:
            if not isinstance(device, dict):
                raise ValueError("Simulator enumeration contains an invalid iOS device.")
            if device.get("isAvailable") is not True:
                continue
            name, udid, state = (device.get(key) for key in ("name", "udid", "state"))
            if not isinstance(name, str):
                raise ValueError("Available simulator has an invalid name.")
            if not name.startswith("iPhone"):
                continue
            if (not isinstance(udid, str) or not UDID.fullmatch(udid)
                    or not isinstance(state, str) or state not in STATES):
                raise ValueError("Available iPhone simulator has an invalid identifier or state.")
            if state == "Shutting Down":
                continue
            choices.append({"runtime": runtime, "udid": udid, "name": name, "state": state})
    if not choices:
        raise ValueError("No usable installed iPhone simulator; no runtime download or license acceptance is authorized.")
    state_order = {"Booted": 0, "Booting": 1, "Shutdown": 2}
    return min(choices, key=lambda device: (
        state_order[device["state"]], device["runtime"], device["name"], device["udid"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        with args.input.open("rb") as stream:
            data = stream.read(MAX_JSON_BYTES + 1)
        if len(data) > MAX_JSON_BYTES:
            raise ValueError("Simulator enumeration exceeds the inspection limit.")
        selected = select_device(json.loads(data))
        with args.output.open("x") as stream:
            json.dump(selected, stream, indent=2)
            stream.write("\n")
        print(selected["udid"])
    except (ValueError, OSError) as error:
        message = str(error) if type(error) is ValueError else "Completed simulator JSON could not be read or selected."
        parser.exit(2, message + "\n")


if __name__ == "__main__":
    main()
