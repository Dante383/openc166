# SPDX-License-Identifier: MIT
"""Serves a simulated ECU's serial port (ASC0) to host tools over TCP.

    openc166-sim serve image.bin --board 5wy19 --port 5555

Tools that use pyserial connect with the URL rfc2217://localhost:5555
(gkflasher and gkbus take it as their interface). RFC 2217 carries the
baud rate, DTR/RTS and break on and off, so a K-line fast init done with
the serial port's break reaches the simulated ECU as a 25 ms low pulse.
With --raw the port is a plain TCP byte stream (socket://localhost:5555);
there is no break, so after the bus has been idle a wake-up pulse is made
up in front of the first byte the tester sends.

The bridge behaves like a K-line interface: every byte the tester sends
comes straight back to it (the bus echo), and the ECU hears its own
transmission too. Bytes the ECU sends at a baud rate the tester is not
listening at are dropped.

Simulated time is paced to wall-clock time, so the tester's timeouts and
the ECU's timers agree. When the simulator is slower than real time, the
tester's input is still placed in simulated time with the gaps it had in
real time.
"""

from __future__ import annotations

import logging
import select
import socket
import time
from typing import List, Optional, Tuple

from . import sim as simmod

logger = logging.getLogger(__name__)

#: simulated time run between checks of the network
SLICE = 0.002
#: raw mode: a tester byte after this much bus silence gets a wake-up pulse
IDLE_WAKE = 0.3
#: the wake-up pulse (ISO 14230 fast init: 25 ms low, 25 ms high)
WAKE_LOW = 0.025
WAKE_HIGH = 0.025
#: RFC 2217 breaks shorter than this are fast inits. pyserial's client waits
#: for the server to acknowledge break on and break off, so the break the
#: bridge measures, and the gap to the next byte, are longer than what the
#: tester timed (75 ms each for a 25 ms fast init is typical) - too long for
#: an ECU that wants its StartCommunication within a few ms of 25 ms. Such a
#: break is replayed as the standard pattern: WAKE_LOW low, WAKE_HIGH high,
#: then the tester's next bytes.
FAST_INIT_MAX = 0.15


class _Port:
    """What pyserial's RFC 2217 PortManager drives: records the tester's
    line settings and breaks."""

    def __init__(self, bridge: "SerialBridge"):
        self._bridge = bridge
        self.baudrate = 10400
        self.bytesize = 8
        self.parity = "N"
        self.stopbits = 1
        self.rts = False
        self.dtr = False
        self.rtscts = False
        self.xonxoff = False
        self.cts = True
        self.dsr = True
        self.ri = False
        self.cd = True
        self._break = False

    @property
    def break_condition(self) -> bool:
        return self._break

    @break_condition.setter
    def break_condition(self, value: bool) -> None:
        if value != self._break:
            self._break = value
            self._bridge._on_break(value)

    def reset_input_buffer(self) -> None:
        pass

    def reset_output_buffer(self) -> None:
        pass


class _Connection:
    def __init__(self, sock: socket.socket):
        self.sock = sock

    def write(self, data: bytes) -> None:
        self.sock.sendall(data)


class SerialBridge:
    """Connects one TCP client at a time to the simulator's ASC0."""

    def __init__(
        self,
        s: simmod.Simulator,
        port: int = 5555,
        host: str = "127.0.0.1",
        rfc2217: bool = True,
        kline: bool = True,
        realtime: bool = True,
        tester_baud: Optional[int] = None,
    ):
        self.s = s
        self.host = host
        self.port = port
        self.rfc2217 = rfc2217
        # K-line: both sides hear the bus echo, and a break (or, on a raw
        # socket, the first byte after idle) is a fast init.
        self.kline = kline
        self.realtime = realtime
        self._tester_baud = tester_baud
        self.s.serial_echo = kline
        self._listener: Optional[socket.socket] = None
        self._client: Optional[socket.socket] = None
        self._manager = None
        self._port = _Port(self)
        self._stop = False
        # Tester events waiting for their simulated time: (cycle, kind, data)
        self._pending: List[Tuple[int, str, object]] = []
        self._last_event_wall: Optional[float] = None
        self._last_event_cycle = 0
        self._break_started: Optional[float] = None
        self._wake_pending: Optional[float] = None
        self._last_activity = time.monotonic()
        self.stats = {"to_ecu": 0, "from_ecu": 0, "dropped": 0, "breaks": 0}

    # ---- setup
    def listen(self) -> int:
        """Opens the listening socket; returns the port (useful with port 0)."""
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind((self.host, self.port))
        ls.listen(1)
        self._listener = ls
        self.port = ls.getsockname()[1]
        return self.port

    @property
    def url(self) -> str:
        return "{}://{}:{}".format("rfc2217" if self.rfc2217 else "socket", self.host, self.port)

    def stop(self) -> None:
        self._stop = True

    @property
    def tester_baud(self) -> int:
        if self._tester_baud:
            return self._tester_baud
        return int(self._port.baudrate) if self.rfc2217 else 10400

    # ---- tester events, placed in simulated time
    def _clock(self) -> int:
        return int(simmod._lib.oc166_sim_clock_hz(self.s._h))

    def _event_cycle(self) -> int:
        """The simulated cycle for a tester event happening now: at least now,
        and as far after the previous event as it was in real time."""
        now_wall = time.monotonic()
        cycle = self.s.cycles
        if self._last_event_wall is not None:
            gap = int((now_wall - self._last_event_wall) * self._clock())
            cycle = max(cycle, self._last_event_cycle + gap)
        self._last_event_wall = now_wall
        self._last_event_cycle = cycle
        return cycle

    def _on_break(self, on: bool) -> None:
        if on:
            self._break_started = time.monotonic()
            self._event_cycle()
            return
        started = self._break_started or time.monotonic()
        measured = time.monotonic() - started
        self._break_started = None
        self.stats["breaks"] += 1
        if self.kline and measured < FAST_INIT_MAX:
            # A fast init: placed in front of the bytes that follow it.
            self._wake_pending = time.monotonic()
            logger.debug("tester: fast init (break of %.1f ms)", measured * 1e3)
        else:
            self._pending.append((self._event_cycle(), "low", measured))
            logger.debug("tester: break of %.1f ms", measured * 1e3)

    def _from_tester(self, data: bytes) -> None:
        if not data:
            return
        idle = time.monotonic() - self._last_activity
        wake = self.kline and (self._wake_pending is not None or (not self.rfc2217 and idle >= IDLE_WAKE))
        self._wake_pending = None
        if wake:
            # The fast init's low phase, then the high phase, then the bytes.
            # (On a raw socket there is no break: it is made up after idle.)
            cycle = self._event_cycle()
            self._pending.append((cycle, "low", WAKE_LOW))
            cycle += int(WAKE_HIGH * self._clock())  # the low event marks the end of the low phase
            self._last_event_cycle = cycle
            self._pending.append((cycle, "bytes", data))
        else:
            self._pending.append((self._event_cycle(), "bytes", data))
        self._last_activity = time.monotonic()
        if self.kline:
            self._to_tester(data)

    def _to_tester(self, data: bytes) -> None:
        if not data or self._client is None:
            return
        if self.rfc2217:
            data = b"".join(self._manager.escape(data))
        try:
            self._client.sendall(data)
        except OSError:
            pass

    def _expire_wake(self) -> None:
        # A fast init with no bytes after it still wakes the ECU.
        if self._wake_pending is not None and time.monotonic() - self._wake_pending > 0.3:
            self._wake_pending = None
            self._pending.append((self._event_cycle(), "low", WAKE_LOW))

    def _deliver_due(self) -> None:
        """Hands the tester events whose time has come to the simulator."""
        now = self.s.cycles
        while self._pending and self._pending[0][0] <= now:
            _, kind, payload = self._pending.pop(0)
            logger.debug("%9.4f s  -> ECU %s", self.s.time, "low %.1f ms" % (float(payload) * 1e3) if kind == "low" else payload.hex(" "))  # type: ignore[union-attr]
            if kind == "low":
                self.s.serial_break(float(payload))
            else:
                self.s.serial_send(payload)  # type: ignore[arg-type]
                self.stats["to_ecu"] += len(payload)  # type: ignore[arg-type]

    # ---- main loop
    def serve_forever(self) -> None:
        if self._listener is None:
            self.listen()
        while not self._stop:
            # The ECU runs on between clients.
            r, _, _ = select.select([self._listener], [], [], 0)
            if r:
                client, addr = self._listener.accept()
                logger.info("tester connected from %s", addr)
                self._serve(client)
                logger.info("tester disconnected")
                continue
            self._run_slice(None)

    def _serve(self, client: socket.socket) -> None:
        client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._client = client
        self._port = _Port(self)
        if self.rfc2217:
            import serial.rfc2217

            self._manager = serial.rfc2217.PortManager(self._port, _Connection(client))
        self._last_event_wall = None
        self.s.serial_receive()  # what the ECU said while nobody listened
        try:
            while not self._stop:
                if not self._run_slice(client):
                    break
        finally:
            self._client = None
            self._manager = None
            client.close()

    def _run_slice(self, client: Optional[socket.socket]) -> bool:
        """Runs one slice of simulated time and moves data both ways.
        Returns False when the client has gone."""
        start_wall = time.monotonic()
        start_sim = self.s.time
        if client is not None:
            r, _, _ = select.select([client], [], [], 0)
            if r:
                try:
                    data = client.recv(4096)
                except OSError:
                    data = b""
                if not data:
                    return False
                if self.rfc2217:
                    data = b"".join(self._manager.filter(data))
                self._from_tester(data)
        self._expire_wake()
        self._deliver_due()
        target = self.s.cycles + int(SLICE * self._clock())
        if self._pending:
            target = min(target, max(self._pending[0][0], self.s.cycles + 1))
        stop = self.s.run_until_cycle(target)
        if stop.reason not in ("none", "watchpoint", "breakpoint"):
            logger.debug("simulator stopped: %s", stop)
        out = self.s.serial_receive_baud()
        if out:
            baud = self.tester_baud
            good = bytes(b for b, rate in out if abs(rate - baud) <= baud * 0.03)
            self.stats["dropped"] += len(out) - len(good)
            self.stats["from_ecu"] += len(good)
            logger.debug("%9.4f s  <- ECU %s%s", self.s.time, good.hex(" "),
                         "" if len(good) == len(out) else " (dropped {} at other baud rates)".format(len(out) - len(good)))
            if good:
                self._last_activity = time.monotonic()
                self._to_tester(good)
        if self.realtime:
            ahead = (self.s.time - start_sim) - (time.monotonic() - start_wall)
            if ahead > 0:
                time.sleep(ahead)
        return True
