#!/usr/bin/env python3
"""
Read AetherWard brain UART AWF1 sniff-stream lines and write PCAP and/or
AetherWard session JSONL.

Firmware command expected on the brain console:
    sniff-stream [channels=all] [hop_ms=200] [duration_s=10] [filter=default]

Install host dependency:
    python3 -m pip install pyserial

Examples:
    python3 aw_sniff_uart.py /dev/ttyUSB0 --cmd "sniff-stream all 200 30" -o capture.pcap
    python3 aw_sniff_uart.py /dev/ttyUSB0 --cmd "sniff-stream 1,6,11 200 60" -o capture.pcap --jsonl session.jsonl
    python3 aw_sniff_uart.py /dev/ttyACM0 --no-command --jsonl session.jsonl --no-pcap
"""
from __future__ import annotations

import argparse
import base64
import binascii
import json
import re
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Dict, Iterable, Optional, Tuple

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover - host hint
    serial = None

PCAP_LINKTYPE_IEEE802_11 = 105
PCAP_LINKTYPE_IEEE802_11_RADIOTAP = 127

SUBTYPE_MGMT = {
    0: "assoc_req",
    1: "assoc_resp",
    2: "reassoc_req",
    3: "reassoc_resp",
    4: "probe_req",
    5: "probe_resp",
    8: "beacon",
    9: "atim",
    10: "disassoc",
    11: "auth",
    12: "deauth",
    13: "action",
}
SUBTYPE_CTRL = {
    8: "block_ack_req",
    9: "block_ack",
    10: "ps_poll",
    11: "rts",
    12: "cts",
    13: "ack",
    14: "cf_end",
    15: "cf_end_ack",
}
SUBTYPE_DATA = {
    0: "data",
    4: "null",
    8: "qos_data",
    12: "qos_null",
}


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def mac(b: bytes) -> str:
    return ":".join(f"{x:02x}" for x in b)


def channel_to_freq_hz(ch: int) -> Optional[int]:
    if 1 <= ch <= 13:
        return (2407 + 5 * ch) * 1_000_000
    if ch == 14:
        return 2484_000_000
    return None


def parse_awf1(line: str) -> Optional[Dict[str, object]]:
    """Parse one AWF1 key=value line from brain sniff-stream."""
    if not line.startswith("AWF1 "):
        return None
    kv: Dict[str, str] = {}
    for token in line.strip().split()[1:]:
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        kv[k] = v
    raw_hex = kv.get("raw", "")
    try:
        raw = binascii.unhexlify(raw_hex)
        ts_us = int(kv.get("ts", "0"), 16)
        return {
            "node": int(kv.get("node", "0"), 0),
            "seq": int(kv.get("seq", "0"), 0),
            "ts_us": ts_us,
            "channel": int(kv.get("ch", "0"), 0),
            "rssi": int(kv.get("rssi", "0"), 0),
            "flags": int(kv.get("flags", "0"), 0),
            "orig_len": int(kv.get("orig", str(len(raw))), 0),
            "captured_len": int(kv.get("len", str(len(raw))), 0),
            "fc0": int(kv.get("fc0", "0"), 0),
            "raw": raw,
        }
    except (ValueError, binascii.Error) as exc:
        raise ValueError(f"bad AWF1 line: {exc}: {line!r}") from exc


def iter_ies(frame: bytes, off: int) -> Iterable[Tuple[int, bytes]]:
    while off + 2 <= len(frame):
        eid = frame[off]
        ln = frame[off + 1]
        off += 2
        if off + ln > len(frame):
            return
        yield eid, frame[off:off + ln]
        off += ln


def decode_ssid(ie: bytes) -> str:
    try:
        return ie.decode("utf-8", "replace")
    except Exception:
        return "".join(chr(c) if 32 <= c < 127 else "?" for c in ie)


def parse_rsn(ie: bytes) -> Dict[str, object]:
    """Small RSN parser. Enough to label WPA2/WPA3 PSK/SAE/CCMP."""
    out: Dict[str, object] = {"present": True, "akm": [], "pairwise": []}
    if len(ie) < 8:
        return out
    pos = 2  # version
    if pos + 4 > len(ie):
        return out
    group = ie[pos:pos + 4]
    pos += 4
    if pos + 2 > len(ie):
        return out
    pair_count = int.from_bytes(ie[pos:pos + 2], "little")
    pos += 2
    pairwise = []
    for _ in range(pair_count):
        if pos + 4 > len(ie):
            break
        pairwise.append(ie[pos:pos + 4])
        pos += 4
    if pos + 2 > len(ie):
        out["pairwise"] = [cipher_name(x) for x in pairwise]
        return out
    akm_count = int.from_bytes(ie[pos:pos + 2], "little")
    pos += 2
    akm = []
    for _ in range(akm_count):
        if pos + 4 > len(ie):
            break
        akm.append(ie[pos:pos + 4])
        pos += 4
    out["group"] = cipher_name(group)
    out["pairwise"] = [cipher_name(x) for x in pairwise]
    out["akm"] = [akm_name(x) for x in akm]
    return out


def cipher_name(suite: bytes) -> str:
    if len(suite) != 4:
        return "?"
    oui, typ = suite[:3], suite[3]
    if oui not in (b"\x00\x0f\xac", b"\x00\x50\xf2"):
        return f"{suite.hex()}"
    return {
        0: "USE-GROUP",
        1: "WEP40",
        2: "TKIP",
        4: "CCMP",
        5: "WEP104",
        6: "BIP",
        8: "GCMP",
        9: "GCMP-256",
        10: "CCMP-256",
    }.get(typ, f"suite-{typ}")


def akm_name(suite: bytes) -> str:
    if len(suite) != 4:
        return "?"
    oui, typ = suite[:3], suite[3]
    if oui not in (b"\x00\x0f\xac", b"\x00\x50\xf2"):
        return f"{suite.hex()}"
    return {
        1: "802.1X",
        2: "PSK",
        3: "FT-802.1X",
        4: "FT-PSK",
        5: "802.1X-SHA256",
        6: "PSK-SHA256",
        8: "SAE",
        9: "FT-SAE",
        18: "OWE",
    }.get(typ, f"akm-{typ}")


def parse_wifi_frame(frame: bytes) -> Dict[str, object]:
    meta: Dict[str, object] = {"protocol": "802.11"}
    if len(frame) < 2:
        meta["frame_type"] = "short"
        return {"metadata": meta}

    fc0, fc1 = frame[0], frame[1]
    ftype = (fc0 >> 2) & 0x03
    subtype = (fc0 >> 4) & 0x0F
    to_ds = bool(fc1 & 0x01)
    from_ds = bool(fc1 & 0x02)
    meta.update({
        "fc0": fc0,
        "fc1": fc1,
        "type": ftype,
        "subtype": subtype,
        "to_ds": to_ds,
        "from_ds": from_ds,
    })

    result: Dict[str, object] = {"metadata": meta}
    subtype_name = "unknown"
    bssid = None
    src = None
    dst = None

    if ftype == 0:
        subtype_name = SUBTYPE_MGMT.get(subtype, "mgmt")
        meta["frame_type"] = "management"
        meta["frame_subtype"] = subtype_name
        if len(frame) >= 24:
            dst = mac(frame[4:10])
            src = mac(frame[10:16])
            bssid = mac(frame[16:22])
            meta.update({"addr1": dst, "addr2": src, "addr3": bssid})
            result["id"] = bssid or src
            result["bssid"] = bssid
        ie_off = 36 if subtype in (5, 8) else 24
        privacy = False
        if subtype in (5, 8) and len(frame) >= 36:
            capability = int.from_bytes(frame[34:36], "little")
            privacy = bool(capability & 0x0010)
            meta["capability"] = capability
        if len(frame) >= ie_off:
            rsn = None
            has_wpa = False
            for eid, val in iter_ies(frame, ie_off):
                if eid == 0:
                    ssid = decode_ssid(val)
                    result["ssid"] = ssid
                    meta["ssid_len"] = len(val)
                elif eid == 3 and val:
                    result["channel"] = int(val[0])
                    meta["ds_channel"] = int(val[0])
                elif eid == 48:
                    rsn = parse_rsn(val)
                    meta["rsn"] = rsn
                elif eid == 221 and len(val) >= 4 and val[:4] == b"\x00\x50\xf2\x01":
                    has_wpa = True
                    meta["wpa_vendor_ie"] = True
            sec, auth = security_from_ies(privacy, rsn, has_wpa)
            result["security"] = sec
            result["auth_mode"] = auth
    elif ftype == 1:
        subtype_name = SUBTYPE_CTRL.get(subtype, "ctrl")
        meta["frame_type"] = "control"
        meta["frame_subtype"] = subtype_name
    elif ftype == 2:
        subtype_name = SUBTYPE_DATA.get(subtype, "data")
        meta["frame_type"] = "data"
        meta["frame_subtype"] = subtype_name
        if len(frame) >= 24:
            a1 = mac(frame[4:10])
            a2 = mac(frame[10:16])
            a3 = mac(frame[16:22])
            meta.update({"addr1": a1, "addr2": a2, "addr3": a3})
            dst = a1
            src = a2
            if not to_ds and not from_ds:
                bssid = a3
            elif to_ds and not from_ds:
                bssid = a1
            elif from_ds and not to_ds:
                bssid = a2
            else:
                bssid = None
                if len(frame) >= 30:
                    meta["addr4"] = mac(frame[24:30])
            result["id"] = bssid or src
            if bssid:
                result["bssid"] = bssid
    else:
        meta["frame_type"] = "extension"
        meta["frame_subtype"] = subtype_name

    if src:
        meta["src"] = src
    if dst:
        meta["dst"] = dst
    return result


def security_from_ies(privacy: bool, rsn: Optional[Dict[str, object]], has_wpa: bool) -> Tuple[str, str]:
    if rsn:
        akm = [str(x) for x in rsn.get("akm", [])]
        pairwise = [str(x) for x in rsn.get("pairwise", [])] or ["CCMP"]
        if "SAE" in akm and "PSK" in akm:
            sec = "WPA2/WPA3"
        elif "SAE" in akm:
            sec = "WPA3"
        else:
            sec = "WPA2"
        auth = f"[{sec}-{'/'.join(akm) if akm else 'RSN'}-{'/'.join(pairwise)}][ESS]"
        return sec, auth
    if has_wpa:
        return "WPA", "[WPA-PSK][ESS]"
    if privacy:
        return "WEP", "[WEP][ESS]"
    return "OPEN", "[OPEN][ESS]"


def awf1_to_session_record(obs: Dict[str, object], store_raw: bool = True) -> Dict[str, object]:
    raw = bytes(obs["raw"])
    ch = int(obs.get("channel", 0))
    parsed = parse_wifi_frame(raw)
    parsed_ch = parsed.get("channel")
    freq = channel_to_freq_hz(ch)
    rec: Dict[str, object] = {
        "t": int(obs.get("ts_us", 0)) / 1_000_000.0,
        "freq": freq,
        "bw": 20_000_000,
        "rssi": float(obs.get("rssi", 0)),
        "ant": f"comp-{int(obs.get('node', 0))}",
        "frame_len": int(obs.get("orig_len", len(raw))),
        "protocol": "802.11",
        "channel": int(parsed_ch if parsed_ch is not None else ch),
    }
    rec.update({k: v for k, v in parsed.items() if k != "metadata"})
    meta = dict(parsed.get("metadata", {}))
    meta.update({
        "node": int(obs.get("node", 0)),
        "frame_seq": int(obs.get("seq", 0)),
        "record_flags": int(obs.get("flags", 0)),
        "captured_len": int(obs.get("captured_len", len(raw))),
        "original_len": int(obs.get("orig_len", len(raw))),
        "truncated": bool(int(obs.get("flags", 0)) & 0x02),
    })
    rec["metadata"] = meta
    if "id" not in rec:
        src = meta.get("src")
        if src:
            rec["id"] = src
    if store_raw:
        rec["raw_frame_hex"] = raw.hex()
    # The GPS fields are intentionally omitted here. Merge them upstream if the
    # brain grows GPS, or post-process by joining with an external GPS logger.
    return rec


@dataclass
class PcapWriter:
    fh: BinaryIO
    linktype: int = PCAP_LINKTYPE_IEEE802_11_RADIOTAP
    radiotap: bool = True

    def __post_init__(self) -> None:
        # Classic PCAP, little-endian, microsecond timestamps.
        self.fh.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, self.linktype))

    def write_80211(self, raw: bytes, ts_us: Optional[int], channel: int = 0, rssi: int = 0) -> None:
        if not ts_us:
            ts_us = int(time.time() * 1_000_000)
        sec = ts_us // 1_000_000
        usec = ts_us % 1_000_000
        packet = raw
        if self.radiotap:
            packet = build_radiotap(ts_us, channel, rssi) + raw
        self.fh.write(struct.pack("<IIII", sec, usec, len(packet), len(packet)))
        self.fh.write(packet)

    def close(self) -> None:
        self.fh.close()


def build_radiotap(ts_us: int, channel: int, rssi: int) -> bytes:
    # Present: TSFT, channel, dBm antenna signal.
    present = (1 << 0) | (1 << 3) | (1 << 5)
    freq_hz = channel_to_freq_hz(channel) or 0
    freq_mhz = freq_hz // 1_000_000
    channel_flags = 0x0080  # 2 GHz; CCK/OFDM left unspecified.
    # Header is 8 bytes. TSFT is naturally aligned at offset 8, channel at 16,
    # signal at 20. Total length is 21 bytes.
    return struct.pack("<BBHIQHHb", 0, 0, 21, present, ts_us, freq_mhz, channel_flags, int(rssi))


def record_to_raw(record: Dict[str, object]) -> Optional[bytes]:
    if "raw_frame_hex" in record:
        try:
            return binascii.unhexlify(str(record["raw_frame_hex"]))
        except binascii.Error:
            return None
    if "raw_frame_b64" in record:
        try:
            return base64.b64decode(str(record["raw_frame_b64"]), validate=True)
        except binascii.Error:
            return None
    return None


def session_record_to_pcap(pcap: PcapWriter, record: Dict[str, object]) -> bool:
    raw = record_to_raw(record)
    if not raw:
        return False
    t = record.get("t")
    ts_us = int(float(t) * 1_000_000) if t is not None else None
    channel = int(record.get("channel") or 0)
    rssi = int(float(record.get("rssi") or 0))
    pcap.write_80211(raw, ts_us, channel, rssi)
    return True


def open_pcap(path: Optional[Path], radiotap: bool) -> Optional[PcapWriter]:
    if path is None:
        return None
    path.parent.mkdir(parents=True, exist_ok=True)
    fh = path.open("wb")
    linktype = PCAP_LINKTYPE_IEEE802_11_RADIOTAP if radiotap else PCAP_LINKTYPE_IEEE802_11
    return PcapWriter(fh=fh, linktype=linktype, radiotap=radiotap)


def run(args: argparse.Namespace) -> int:
    if serial is None:
        eprint("pyserial is missing. Install it with: python3 -m pip install pyserial")
        return 2

    pcap_path = None if args.no_pcap else Path(args.output)
    pcap = open_pcap(pcap_path, radiotap=not args.raw_80211)
    jsonl_fh = None
    if args.jsonl:
        Path(args.jsonl).parent.mkdir(parents=True, exist_ok=True)
        jsonl_fh = open(args.jsonl, "a", encoding="utf-8")

    n_frames = 0
    n_json = 0
    n_bad = 0
    try:
        with serial.Serial(args.port, args.baud, timeout=args.timeout) as ser:
            if not args.no_command:
                # Wake the console line editor, optionally probe, then launch the stream.
                time.sleep(args.settle)
                ser.write(b"\r\n")
                ser.flush()
                time.sleep(0.05)
                if args.probe:
                    ser.write(b"bus probe\r\n")
                    ser.flush()
                    time.sleep(args.probe_delay)
                cmd = args.cmd or "sniff-stream all 200 10"
                ser.write(cmd.encode("utf-8") + b"\r\n")
                ser.flush()
                if not args.quiet:
                    eprint(f"sent: {cmd}")

            while True:
                raw_line = ser.readline()
                if not raw_line:
                    if args.no_command:
                        continue
                    # Finite firmware command should eventually print AWSNIFF end.
                    continue
                line = raw_line.decode("utf-8", "replace").strip()
                if not line:
                    continue

                if line.startswith("AWF1 "):
                    try:
                        obs = parse_awf1(line)
                        if obs is None:
                            continue
                        raw = bytes(obs["raw"])
                        if pcap:
                            pcap.write_80211(
                                raw,
                                int(obs.get("ts_us", 0)),
                                int(obs.get("channel", 0)),
                                int(obs.get("rssi", 0)),
                            )
                        if jsonl_fh:
                            rec = awf1_to_session_record(obs, store_raw=not args.no_raw_jsonl)
                            jsonl_fh.write(json.dumps(rec, separators=(",", ":"), ensure_ascii=False) + "\n")
                        n_frames += 1
                        if args.progress and n_frames % args.progress == 0:
                            eprint(f"frames={n_frames}")
                    except Exception as exc:  # keep capture alive on one malformed line
                        n_bad += 1
                        if not args.quiet:
                            eprint(f"bad AWF1: {exc}")
                    continue

                if line.startswith("{"):
                    # Also accept already-JSONL session records from UART/file replay.
                    try:
                        rec = json.loads(line)
                        raw = record_to_raw(rec)
                        if pcap and raw:
                            session_record_to_pcap(pcap, rec)
                        if jsonl_fh:
                            jsonl_fh.write(json.dumps(rec, separators=(",", ":"), ensure_ascii=False) + "\n")
                        n_json += 1
                    except Exception as exc:
                        n_bad += 1
                        if not args.quiet:
                            eprint(f"bad JSONL: {exc}: {line[:120]}")
                    continue

                if line.startswith("AWSNIFF end"):
                    if not args.quiet:
                        eprint(line)
                    break

                if not args.quiet and (args.show_all or line.startswith("AWSNIFF") or line.startswith("[bus]") or line.startswith("test-sniff")):
                    eprint(line)
    except KeyboardInterrupt:
        eprint("interrupted")
    finally:
        if pcap:
            pcap.close()
        if jsonl_fh:
            jsonl_fh.close()

    if not args.quiet:
        outs = []
        if pcap_path:
            outs.append(f"pcap={pcap_path}")
        if args.jsonl:
            outs.append(f"jsonl={args.jsonl}")
        eprint(f"done frames={n_frames} json_records={n_json} bad={n_bad} " + " ".join(outs))
    return 0


def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Read AetherWard brain sniff-stream UART and write PCAP/JSONL")
    p.add_argument("port", help="serial port, e.g. /dev/ttyUSB0 or /dev/ttyACM0")
    p.add_argument("-b", "--baud", type=int, default=115200)
    p.add_argument("-o", "--output", default="capture.pcap", help="PCAP output path, default: capture.pcap")
    p.add_argument("--jsonl", help="also append AetherWard session JSONL records to this file")
    p.add_argument("--no-pcap", action="store_true", help="do not write PCAP")
    p.add_argument("--raw-80211", action="store_true", help="write PCAP linktype 105 raw 802.11 instead of radiotap")
    p.add_argument("--no-raw-jsonl", action="store_true", help="omit raw_frame_hex from JSONL output")
    p.add_argument("--cmd", default="sniff-stream all 200 10", help="brain command to send")
    p.add_argument("--no-command", action="store_true", help="only listen; do not send a command")
    p.add_argument("--probe", action="store_true", help="send 'bus probe' before the sniff command")
    p.add_argument("--probe-delay", type=float, default=1.0)
    p.add_argument("--settle", type=float, default=0.25)
    p.add_argument("--timeout", type=float, default=0.2, help="serial readline timeout")
    p.add_argument("--progress", type=int, default=100, help="print every N frames; 0 disables")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--show-all", action="store_true", help="mirror all non-frame UART lines to stderr")
    return p


if __name__ == "__main__":
    raise SystemExit(run(build_argparser().parse_args()))
