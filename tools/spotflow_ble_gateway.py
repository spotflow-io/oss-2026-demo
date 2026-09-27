#!/usr/bin/env python3
# Copyright (c) 2026 Spotflow s.r.o.
# SPDX-License-Identifier: Apache-2.0

"""
Minimal host-side Spotflow BLE gateway (macOS / Linux).

Booth fallback. The demo's primary gateway is the Spotflow web app, which speaks
Web Bluetooth straight from the browser; this script does the same job from a
terminal for when a browser will not cooperate with the venue's machine.

The CC2340R5 has no IP connectivity, so the Spotflow Device SDK uses its BLE
transport: the device is a GATT peripheral and a gateway relays framed CBOR
messages to mqtt.spotflow.io. Discovery is by service UUID, so nothing here is
specific to a board - it was written for an EFR32MG24 and works unmodified.

Protocol (see device-sdk zephyr/src/net/transport/ble):

  service                 26530001-81E5-4861-82AE-2C92E6887922
    0002 capabilities     read   [major, minor]
    0003 device id        read   utf-8
    0004 session metadata read   CBOR
    0005 TX stream        notify framed device -> cloud
    0006 RX stream        write-without-response, framed cloud -> device

  frame: [type][flags][seq]([len_lo][len_hi] if IS_FIRST) payload...
  flags: bit0 IS_FIRST, bit1 IS_LAST
  type:  0x02 TELEMETRY, 0x03 REPORTED_CONFIGURATION, 0x04 DESIRED_CONFIGURATION

MQTT: username = device id, password = ingest key.

Usage:
    export SPOTFLOW_INGEST_KEY=sf_ikv1_...        # or put it in .env.local
    python3 tools/spotflow_ble_gateway.py
"""

import asyncio
import os
import pathlib
import ssl
import sys
import time

from bleak import BleakClient, BleakScanner
import paho.mqtt.client as mqtt

SERVICE_UUID = "26530001-81e5-4861-82ae-2c92e6887922"
UUID_CAPABILITIES = "26530002-81e5-4861-82ae-2c92e6887922"
UUID_DEVICE_ID = "26530003-81e5-4861-82ae-2c92e6887922"
UUID_SESSION_METADATA = "26530004-81e5-4861-82ae-2c92e6887922"
UUID_TX_STREAM = "26530005-81e5-4861-82ae-2c92e6887922"
UUID_RX_STREAM = "26530006-81e5-4861-82ae-2c92e6887922"

MSG_TELEMETRY = 0x02
MSG_REPORTED_CONFIGURATION = 0x03
MSG_DESIRED_CONFIGURATION = 0x04

FRAME_IS_FIRST = 0x01
FRAME_IS_LAST = 0x02

TOPIC_INGEST = "ingest-cbor"
TOPIC_REPORTED_CONFIG = "config-cbor-d2c"
TOPIC_DESIRED_CONFIG = "config-cbor-c2d"

MQTT_HOST = "mqtt.spotflow.io"
MQTT_PORT = 8883


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def load_ingest_key():
    key = os.environ.get("SPOTFLOW_INGEST_KEY")
    if key:
        return key.strip()

    env_file = pathlib.Path(__file__).resolve().parent.parent / ".env.local"
    if env_file.exists():
        for line in env_file.read_text().splitlines():
            line = line.strip()
            if line.startswith("SPOTFLOW_INGEST_KEY="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")

    sys.exit("SPOTFLOW_INGEST_KEY is not set (env var or .env.local)")


class Reassembler:
    """Reassembles TX-stream fragments into whole messages."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.msg_type = None
        self.sequence = None
        self.total_len = 0
        self.buf = bytearray()

    def push(self, frame: bytes):
        """Returns (msg_type, payload) once a message is complete, else None."""
        if len(frame) < 3:
            return None

        msg_type, flags, sequence = frame[0], frame[1], frame[2]
        is_first = bool(flags & FRAME_IS_FIRST)
        is_last = bool(flags & FRAME_IS_LAST)

        if is_first:
            if len(frame) < 5:
                return None
            self.msg_type = msg_type
            self.sequence = sequence
            self.total_len = int.from_bytes(frame[3:5], "little")
            self.buf = bytearray(frame[5:])
        else:
            if self.sequence != sequence or self.msg_type != msg_type:
                # Fragment from a message we never saw the start of.
                self.reset()
                return None
            self.buf.extend(frame[3:])

        if not is_last:
            return None

        payload = bytes(self.buf)
        msg_type = self.msg_type
        expected = self.total_len
        self.reset()

        if len(payload) != expected:
            log(f"  ! dropped message: got {len(payload)} bytes, header said {expected}")
            return None

        return msg_type, payload


class MqttUplink:
    def __init__(self, device_id, ingest_key, on_desired_config):
        self.device_id = device_id
        self.on_desired_config = on_desired_config
        self.connected = asyncio.Event()
        self.loop = asyncio.get_running_loop()

        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=device_id,
            protocol=mqtt.MQTTv5,
        )
        self.client.username_pw_set(device_id, ingest_key)
        self.client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code != 0:
            log(f"MQTT connect refused: {reason_code}")
            return
        log(f"MQTT connected as '{self.device_id}' to {MQTT_HOST}:{MQTT_PORT}")
        client.subscribe(TOPIC_DESIRED_CONFIG, qos=1)
        self.loop.call_soon_threadsafe(self.connected.set)

    def _on_disconnect(self, client, userdata, flags, reason_code, properties=None):
        log(f"MQTT disconnected: {reason_code}")
        self.loop.call_soon_threadsafe(self.connected.clear)

    def _on_message(self, client, userdata, message):
        self.loop.call_soon_threadsafe(self.on_desired_config, message.payload)

    def start(self):
        self.client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
        self.client.loop_start()

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()

    def publish(self, topic, payload):
        info = self.client.publish(topic, payload, qos=1)
        return info


async def run_forever(address=None):
    """Relay one device, reconnecting after it crashes and reboots."""
    while True:
        try:
            await run(address)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            log(f"session ended: {type(exc).__name__}: {exc}")
        log("device gone; rescanning in 2 s ...")
        await asyncio.sleep(2)


async def run(address=None):
    ingest_key = load_ingest_key()

    log("scanning for a Spotflow BLE peripheral ...")
    device = None
    if address:
        device = await BleakScanner.find_device_by_address(address, timeout=20.0)
    else:
        device = await BleakScanner.find_device_by_filter(
            lambda d, ad: SERVICE_UUID in [u.lower() for u in (ad.service_uuids or [])],
            timeout=20.0,
        )
    if device is None:
        raise RuntimeError("no Spotflow device found (is the board advertising?)")

    log(f"found {device.name} [{device.address}]")

    async with BleakClient(device) as client:
        caps = await client.read_gatt_char(UUID_CAPABILITIES)
        device_id = (await client.read_gatt_char(UUID_DEVICE_ID)).decode()
        log(f"connected; protocol v{caps[0]}.{caps[1]}, device id '{device_id}'")

        pending = []

        def on_desired_config(payload):
            pending.append(payload)

        uplink = MqttUplink(device_id, ingest_key, on_desired_config)
        uplink.start()
        try:
            await asyncio.wait_for(uplink.connected.wait(), timeout=30)
        except asyncio.TimeoutError:
            uplink.stop()
            raise RuntimeError("MQTT did not connect within 30 s (check the ingest key)")

        # Session metadata first, exactly like the Android gateway does.
        session_metadata = await client.read_gatt_char(UUID_SESSION_METADATA)
        uplink.publish(TOPIC_INGEST, session_metadata)
        log(f"published session metadata ({len(session_metadata)} bytes)")

        reasm = Reassembler()
        counters = {"telemetry": 0, "config": 0, "bytes": 0}

        def on_notify(_char, data: bytearray):
            result = reasm.push(bytes(data))
            if result is None:
                return
            msg_type, payload = result
            if msg_type == MSG_TELEMETRY:
                topic = TOPIC_INGEST
                counters["telemetry"] += 1
            elif msg_type == MSG_REPORTED_CONFIGURATION:
                topic = TOPIC_REPORTED_CONFIG
                counters["config"] += 1
            else:
                return
            counters["bytes"] += len(payload)
            uplink.publish(topic, payload)
            log(
                f"-> {topic}  {len(payload):5d} B   "
                f"(telemetry={counters['telemetry']} config={counters['config']} "
                f"total={counters['bytes']} B)"
            )

        await client.start_notify(UUID_TX_STREAM, on_notify)
        log("subscribed to TX stream; relaying. Ctrl-C to stop.")

        try:
            while client.is_connected:
                while pending:
                    payload = pending.pop(0)
                    await send_desired_config(client, payload)
                await asyncio.sleep(0.2)
        except asyncio.CancelledError:
            pass
        finally:
            uplink.stop()
            log(
                f"stopped. telemetry messages={counters['telemetry']} "
                f"config={counters['config']} bytes={counters['bytes']}"
            )


async def send_desired_config(client, payload, mtu_payload=180):
    """Fragment a desired-configuration message onto the RX stream."""
    offset = 0
    seq = 0
    while offset < len(payload):
        first = offset == 0
        header_len = 5 if first else 3
        chunk = payload[offset : offset + (mtu_payload - header_len)]
        last = (offset + len(chunk)) == len(payload)
        flags = (FRAME_IS_FIRST if first else 0) | (FRAME_IS_LAST if last else 0)
        frame = bytes([MSG_DESIRED_CONFIGURATION, flags, seq])
        if first:
            frame += len(payload).to_bytes(2, "little")
        frame += chunk
        await client.write_gatt_char(UUID_RX_STREAM, frame, response=False)
        offset += len(chunk)
    log(f"<- desired configuration ({len(payload)} bytes)")


if __name__ == "__main__":
    addr = sys.argv[1] if len(sys.argv) > 1 else None
    try:
        asyncio.run(run_forever(addr))
    except KeyboardInterrupt:
        pass
