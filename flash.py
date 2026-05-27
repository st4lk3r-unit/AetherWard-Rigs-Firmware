#!/usr/bin/env python3
"""
aetherward-rig flash tool
Usage:  python3 flash.py rigs/<rig>.json [--build-only] [--port /dev/ttyUSBx]

All configuration questions (SPI Hz, companion count, antenna, heartbeat) are
asked upfront before any building or flashing begins.  A rig JSON that includes
a "flash" section can answer every question automatically — the only prompt left
is a single confirmation that the config looks correct.

  "flash": {
    "spi_hz":          8000000,
    "companion_count": 3,
    "ext_antenna":     false,
    "heartbeat":       true
  }
"""

import argparse
import json
import os
import subprocess
import sys
import time

try:
    import serial.tools.list_ports
    _HAS_SERIAL = True
except ImportError:
    _HAS_SERIAL = False

# ── ANSI colours ──────────────────────────────────────────────────────────────
def _ansi(*codes):
    return "\033[" + ";".join(str(c) for c in codes) + "m"

RST     = _ansi(0)
BOLD    = _ansi(1)
DIM     = _ansi(2)
CYAN    = _ansi(96)
GREEN   = _ansi(92)
YELL    = _ansi(93)
RED     = _ansi(91)
WHITE   = _ansi(97)
MAGENTA = _ansi(95)

def _w(line):       sys.stdout.write(line); sys.stdout.flush()
def ok(msg):        _w(f"  {GREEN}✓  {msg}{RST}\n")
def warn(msg):      _w(f"  {YELL}⚠  {msg}{RST}\n")
def err(msg):       _w(f"  {RED}✗  {msg}{RST}\n")
def info(k, v=""):  _w(f"  {BOLD}{WHITE}{k:<18}{RST}  {CYAN}{v}{RST}\n")
def dim(msg):       _w(f"  {DIM}{msg}{RST}\n")

def banner(title, subtitle=""):
    w = 64
    _w(f"\n{CYAN}{'━'*w}{RST}\n")
    _w(f"{CYAN}{BOLD}  {title}{RST}\n")
    if subtitle:
        _w(f"  {DIM}{subtitle}{RST}\n")
    _w(f"{CYAN}{'━'*w}{RST}\n\n")

def section(label, n, total):
    pad = 56 - len(label) - len(str(n)) - len(str(total))
    _w(f"\n{CYAN}── [{n}/{total}] {BOLD}{label}{RST}{CYAN} {'─'*max(pad,0)}{RST}\n")

def ask_yn(prompt, default=True):
    hint = "[Y/n]" if default else "[y/N]"
    while True:
        _w(f"  {BOLD}{prompt}{RST} {DIM}{hint}{RST} ")
        ans = input().strip().lower()
        if ans in ("y", "yes"):
            return True
        if ans in ("n", "no"):
            return False
        if ans == "":
            return default
        warn("Please answer y or n")

# ── PlatformIO ────────────────────────────────────────────────────────────────
_PIO          = None
_PIO_OVERRIDE = None

def _find_pio():
    global _PIO
    if _PIO:
        return _PIO
    candidates = []
    if _PIO_OVERRIDE:
        candidates.append(_PIO_OVERRIDE)
    candidates += ["pio",
                   os.path.expanduser("~/.platformio/penv/bin/pio"),
                   os.path.expanduser("~/.local/bin/pio")]
    for c in candidates:
        try:
            subprocess.run([c, "--version"], capture_output=True, check=True)
            _PIO = c
            return _PIO
        except (FileNotFoundError, subprocess.CalledProcessError):
            pass
    err("PlatformIO (pio) not found — install from https://platformio.org")
    err("If pio is in a non-standard location, use --pio /path/to/pio")
    sys.exit(1)

# ── Serial port detection ─────────────────────────────────────────────────────
def _list_ports():
    if not _HAS_SERIAL:
        return []
    return [p.device for p in serial.tools.list_ports.comports()]

def _pick_port(label, hint=None):
    if hint:
        return hint
    ports = _list_ports()
    if len(ports) == 1:
        dim(f"auto-detected port: {ports[0]}")
        return ports[0]
    if len(ports) > 1:
        _w(f"\n  Available ports:\n")
        for i, p in enumerate(ports):
            _w(f"    {BOLD}[{i+1}]{RST}  {p}\n")
        while True:
            choice = input(f"\n  Select port for {label} [1-{len(ports)} or path]: ").strip()
            try:
                idx = int(choice) - 1
                if 0 <= idx < len(ports):
                    return ports[idx]
            except ValueError:
                if choice:
                    return choice
    return input(f"  Enter port for {label} (e.g. /dev/ttyUSB0): ").strip()

# ── Build + flash ─────────────────────────────────────────────────────────────
_SKIPPED = object()   # sentinel — distinct from True/False in results

def _pio_run(project_dir, env, targets, port=None, extra_flags=None):
    pio = _find_pio()
    cmd = [pio, "run", "-e", env] + [arg for t in targets for arg in ("-t", t)]
    if port:
        cmd += ["--upload-port", port]
    dim("$ " + " ".join(cmd))
    _w("\n")
    run_env = os.environ.copy()
    if extra_flags:
        existing = run_env.get("PLATFORMIO_BUILD_FLAGS", "")
        run_env["PLATFORMIO_BUILD_FLAGS"] = (existing + " " + " ".join(extra_flags)).strip()
    result = subprocess.run(cmd, cwd=project_dir, env=run_env)
    return result.returncode == 0

def flash_device(root, project, env, label, port=None,
                 build_only=False, extra_flags=None):
    project_dir = os.path.join(root, project)
    targets     = ["build"] if build_only else ["upload"]
    verb        = "Building" if build_only else "Flashing"
    _w(f"  {DIM}{verb}…{RST}\n\n")
    success = _pio_run(project_dir, env, targets, port, extra_flags)
    if success:
        ok(f"{label} {'built' if build_only else 'flashed'} successfully")
    else:
        err(f"{'Build' if build_only else 'Flash'} failed for {label}")
    return success

def _ask_recovery(label):
    """Prompt retry / skip / exit after a failure. Returns 'retry' or 'skip'."""
    _w(f"\n  {RED}{BOLD}  ✗  {label} failed.{RST}  What now?\n\n")
    _w(f"    {BOLD}[r]{RST}  retry\n")
    _w(f"    {BOLD}[s]{RST}  skip this device\n")
    _w(f"    {BOLD}[x]{RST}  exit\n")
    while True:
        choice = input(f"\n  Choice [r/s/x]: ").strip().lower()
        if choice in ("r", "retry"):
            return "retry"
        if choice in ("s", "skip"):
            return "skip"
        if choice in ("x", "exit", "q"):
            _w(f"  {YELL}Exiting.{RST}\n\n")
            sys.exit(1)
        warn("Enter r, s, or x")

def flash_with_recovery(root, project, env, label,
                        build_only, extra_flags, connect_fn):
    """
    Flash one device with retry / skip / exit on failure.

    connect_fn() is called before each attempt; it should prompt the operator
    to plug in the device and return the port string (or None for build-only).
    Returns True on success, _SKIPPED if the operator chose to skip.
    """
    while True:
        port = connect_fn()
        success = flash_device(root, project, env, label,
                               port, build_only, extra_flags)
        if success:
            return True
        action = _ask_recovery(label)
        if action == "skip":
            warn(f"{label} skipped")
            return _SKIPPED
        # action == "retry" — loop again

# ── Pin matrix display ────────────────────────────────────────────────────────
def print_pin_matrix(brain, comp_tmpl, brain_pins, comp_pins):
    _w(f"\n{CYAN}── Pin Matrix {'─'*44}{RST}\n")

    _w(f"\n  {BOLD}{WHITE}Brain{RST}  {DIM}({brain.get('board', '')}){RST}\n")
    for key, val in brain_pins.items():
        _w(f"    {WHITE}{key:<20}{RST}  {CYAN}{val}{RST}\n")

    if comp_tmpl and comp_pins:
        _w(f"\n  {BOLD}{WHITE}Companion{RST}  {DIM}({comp_tmpl.get('board', '')}){RST}\n")
        for key, val in comp_pins.items():
            _w(f"    {WHITE}{key:<20}{RST}  {CYAN}{val}{RST}\n")

    _w("\n")

# ── Config summary ────────────────────────────────────────────────────────────
def print_config_summary(rig, brain, comp_tmpl, num_companions,
                         spi_hz, ext_antenna, heartbeat):
    _w(f"\n{CYAN}── Configuration Summary {'─'*37}{RST}\n\n")
    info("Rig",        rig["rig"].upper())
    info("Brain",      f"{brain.get('board', '')}  env={brain['env']}")
    if comp_tmpl:
        info("Companions", f"{num_companions} × {comp_tmpl.get('board', '')}  env={comp_tmpl['env']}")
    info("SPI clock",  f"{spi_hz:,} Hz")
    if comp_tmpl and num_companions > 0:
        info("Antenna",   "external U.FL" if ext_antenna else "onboard")
        info("Heartbeat", "enabled"        if heartbeat   else "disabled")
    _w("\n")

# ── SPI Hz ────────────────────────────────────────────────────────────────────
_SPI_PRESETS = [
    ("40 MHz — ESP32 HSPI max, short wires only",        40_000_000),
    ("20 MHz — near max for most SPI slaves",            20_000_000),
    ("16 MHz — fast, verify signal integrity first",     16_000_000),
    ("8 MHz  — 2× throughput, usually reliable",          8_000_000),
    ("4 MHz  — safe default, works with all SPI slaves",  4_000_000),
]

def ask_spi_hz():
    _w(f"\n  {MAGENTA}{BOLD}SPI clock speed:{RST}\n\n")
    for i, (desc, _) in enumerate(_SPI_PRESETS):
        _w(f"    {BOLD}[{i+1}]{RST}  {desc}\n")
    _w(f"    {BOLD}[c]{RST}  custom value\n")
    while True:
        choice = input(f"\n  Select SPI Hz [1-{len(_SPI_PRESETS)}/c, Enter=1]: ").strip().lower()
        if choice in ("", "1"):
            return _SPI_PRESETS[0][1]
        if choice == "c":
            try:
                return int(input("  Enter Hz (e.g. 10000000): ").strip())
            except ValueError:
                warn("Invalid value, defaulting to 4 MHz")
                return 4_000_000
        try:
            idx = int(choice) - 1
            if 0 <= idx < len(_SPI_PRESETS):
                return _SPI_PRESETS[idx][1]
        except ValueError:
            pass
        warn("Invalid selection")

# ── Companion count ───────────────────────────────────────────────────────────
def ask_companion_count(default):
    _w(f"\n  {MAGENTA}{BOLD}Companion count:{RST}\n")
    while True:
        _w(f"  Number of companions {DIM}[Enter={default}]{RST}: ")
        raw = input().strip()
        if raw == "":
            return default
        try:
            n = int(raw)
            if n >= 1:
                return n
        except ValueError:
            pass
        warn("Enter a positive integer")

# ── Pin-matrix → build flags ──────────────────────────────────────────────────
_BRAIN_PIN_MAP = {
    "spi_sck":        "AWBUS_SPI_SCK_PIN",
    "spi_mosi":       "AWBUS_SPI_MOSI_PIN",
    "spi_miso":       "AWBUS_SPI_MISO_PIN",
    "reset":          "AWBUS_RESET_PIN",
    "reset_pulse_ms": "AWBUS_RESET_PULSE_MS",
    "led":            "BRAIN_LED_GPIO",
}

_COMPANION_PIN_MAP = {
    "spi_sck":     "AWBUS_SPI_SCK_PIN",
    "spi_mosi":    "AWBUS_SPI_MOSI_PIN",
    "spi_miso":    "AWBUS_SPI_MISO_PIN",
    "spi_cs":      "AWBUS_SPI_CS_PIN",
    "ready":       "AWBUS_READY_PIN",
    "reset":       "AWBUS_RESET_PIN",
    "wifi_enable": "COMPANION_WIFI_ENABLE_GPIO",
    "ant":         "COMPANION_ANT_GPIO",
    "led":         "COMPANION_LED_GPIO",
}

def brain_pin_flags(brain_pins, companion_count):
    flags = [f"-DAWBUS_COMPANION_COUNT={companion_count}"]
    for key, macro in _BRAIN_PIN_MAP.items():
        if key in brain_pins:
            flags.append(f"-D{macro}={brain_pins[key]}")
    cs    = brain_pins.get("companion_cs",    [])
    ready = brain_pins.get("companion_ready", [])
    for i in range(companion_count):
        if i < len(cs):
            flags.append(f"-DAWBUS_CS_PIN_{i}={cs[i]}")
        if i < len(ready):
            flags.append(f"-DAWBUS_READY_PIN_{i}={ready[i]}")
    return flags

def companion_pin_flags(comp_pins):
    return [
        f"-D{macro}={comp_pins[key]}"
        for key, macro in _COMPANION_PIN_MAP.items()
        if key in comp_pins
    ]

# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    global _PIO_OVERRIDE

    ap = argparse.ArgumentParser(description="AetherWard rig flash tool")
    ap.add_argument("rig_json",       help="Path to rig JSON (e.g. rigs/prism.json)")
    ap.add_argument("--build-only",   action="store_true",
                    help="Build without flashing (no port needed)")
    ap.add_argument("--port",         default=None,
                    help="Force a specific serial port for all devices")
    ap.add_argument("--pio",          default=None,
                    help="Path to pio executable (overrides auto-detection)")
    ap.add_argument("--spi-hz",       type=int, default=None,
                    help="AW-Bus SPI clock in Hz (e.g. 8000000). Skips prompt.")
    ap.add_argument("--num-companions", type=int, default=None,
                    help="Number of companions to flash. Skips prompt.")
    ap.add_argument("--all-ext-ant",  action="store_true",
                    help="Use external U.FL antenna on all companions")
    ap.add_argument("--all-heartbeat", action="store_true",
                    help="Enable LED heartbeat on all companions")
    args = ap.parse_args()

    if args.pio:
        _PIO_OVERRIDE = args.pio

    rig_path = os.path.abspath(args.rig_json)
    if not os.path.isfile(rig_path):
        err(f"Rig file not found: {rig_path}"); sys.exit(1)

    with open(rig_path) as f:
        rig = json.load(f)

    root          = os.path.dirname(os.path.abspath(__file__))
    brain         = rig["brain"]
    comp_tmpl     = rig.get("companion")
    default_count = rig.get("companion_default_count", 3)
    pins          = rig.get("pins", {})
    brain_pins    = pins.get("brain", {})
    comp_pins     = pins.get("companion", {})
    flash_cfg     = rig.get("flash", {})   # optional pre-defined flash config

    # ── Phase 1: Configuration ────────────────────────────────────────────────
    banner(
        f"aetherward-rig flash  ·  {rig['rig'].upper()}",
        rig.get("description", "")
    )
    info("Mode", "build only" if args.build_only else "build + flash")

    if not _HAS_SERIAL and not args.build_only:
        warn("pyserial not installed — manual port entry required  "
             "(pip install pyserial)")

    # Pin matrix always shown first so the operator can verify wiring
    print_pin_matrix(brain, comp_tmpl, brain_pins, comp_pins)

    # Companion count
    if comp_tmpl is None:
        num_companions = 0
    elif args.num_companions is not None:
        num_companions = args.num_companions
        dim(f"companion count: {num_companions}  (--num-companions)")
    elif "companion_count" in flash_cfg:
        num_companions = flash_cfg["companion_count"]
        dim(f"companion count: {num_companions}  (from JSON)")
    else:
        num_companions = ask_companion_count(default_count)

    # SPI Hz
    if args.spi_hz is not None:
        spi_hz = args.spi_hz
        dim(f"SPI clock: {spi_hz:,} Hz  (--spi-hz)")
    elif "spi_hz" in flash_cfg:
        spi_hz = flash_cfg["spi_hz"]
        dim(f"SPI clock: {spi_hz:,} Hz  (from JSON)")
    else:
        spi_hz = ask_spi_hz()
    ok(f"SPI clock: {spi_hz:,} Hz")

    # Companion antenna + heartbeat (asked once for all companions)
    if comp_tmpl is not None and num_companions > 0:
        if args.all_ext_ant:
            ext_antenna = True
        elif "ext_antenna" in flash_cfg:
            ext_antenna = flash_cfg["ext_antenna"]
            dim(f"ext antenna: {ext_antenna}  (from JSON)")
        else:
            _w(f"\n  {MAGENTA}{BOLD}Companion options (applied to all):{RST}\n")
            ext_antenna = ask_yn("  Use external U.FL antenna?", default=False)

        if args.all_heartbeat:
            heartbeat = True
        elif "heartbeat" in flash_cfg:
            heartbeat = flash_cfg["heartbeat"]
            dim(f"heartbeat: {heartbeat}  (from JSON)")
        else:
            heartbeat = ask_yn("  Enable LED heartbeat?", default=True)
    else:
        ext_antenna = False
        heartbeat   = False

    # ── Config summary + single confirmation ─────────────────────────────────
    print_config_summary(rig, brain, comp_tmpl, num_companions,
                         spi_hz, ext_antenna, heartbeat)

    verb = "build" if args.build_only else "flash"
    if not ask_yn(f"Proceed with {verb}?", default=True):
        _w(f"  {YELL}Aborted.{RST}\n\n")
        sys.exit(0)

    # ── Phase 2: Build / flash each device ───────────────────────────────────
    companions = []
    for i in range(num_companions):
        node_id = i + 1
        companions.append({
            "label":       f"{comp_tmpl['label_prefix']} {node_id}",
            "description": comp_tmpl.get("description", ""),
            "project":     comp_tmpl["project"],
            "env":         comp_tmpl["env"],
            "board":       comp_tmpl.get("board", ""),
            "node_id":     node_id,
        })

    total = 1 + len(companions)
    info("Devices", f"{total} total  (1 brain + {len(companions)} companion(s))")

    results = {}

    # Brain
    section(brain["label"], 1, total)
    info("board", brain.get("board", ""))
    info("env",   brain["env"])
    info("desc",  brain.get("description", ""))

    brain_flags  = [f"-DAWBUS_SPI_HZ={spi_hz}UL"]
    brain_flags += brain_pin_flags(brain_pins, num_companions)

    def brain_connect():
        if args.build_only:
            return None
        _w(f"\n  {BOLD}Plug in the {brain['label']} and press Enter…{RST} ")
        input()
        return _pick_port(brain["label"], args.port)

    results[brain["label"]] = flash_with_recovery(
        root, brain["project"], brain["env"],
        brain["label"], args.build_only, brain_flags, brain_connect)

    # Companions
    flags_summary = ", ".join((
        "ext-ant"    if ext_antenna else "onboard-ant",
        "heartbeat"  if heartbeat   else "no-heartbeat",
    ))
    comp_extra = companion_pin_flags(comp_pins)
    if ext_antenna: comp_extra.append("-DCOMPANION_EXT_ANTENNA=1")
    if heartbeat:   comp_extra.append("-DCOMPANION_HEARTBEAT=1")

    for i, comp in enumerate(companions):
        section(comp["label"], i + 2, total)
        info("board",   comp.get("board", ""))
        info("env",     comp["env"])
        info("node_id", str(comp["node_id"]))
        info("desc",    comp.get("description", ""))

        extra_flags = [f"-DAWBUS_NODE_ID={comp['node_id']}"] + comp_extra

        def comp_connect(label=comp["label"]):
            if args.build_only:
                return None
            _w(f"\n  {DIM}({flags_summary}){RST}\n")
            _w(f"  {BOLD}Plug in {label} and press Enter…{RST} ")
            input()
            return _pick_port(label, args.port)

        results[comp["label"]] = flash_with_recovery(
            root, comp["project"], comp["env"],
            comp["label"], args.build_only, extra_flags, comp_connect)

    # ── Summary ───────────────────────────────────────────────────────────────
    banner("Summary")
    any_failed = False
    any_skipped = False
    for label, result in results.items():
        if result is True:
            ok(label)
        elif result is _SKIPPED:
            _w(f"  {YELL}⊘  {label}  (skipped){RST}\n")
            any_skipped = True
        else:
            err(label)
            any_failed = True

    past = "built" if args.build_only else "flashed"
    if not any_failed and not any_skipped:
        _w(f"\n{GREEN}{BOLD}  All devices {past} successfully.{RST}\n\n")
    elif not any_failed:
        _w(f"\n{YELL}{BOLD}  Done with skipped devices.{RST}\n\n")
    else:
        _w(f"\n{RED}{BOLD}  Some devices failed — check output above.{RST}\n\n")
        sys.exit(1)

if __name__ == "__main__":
    main()
