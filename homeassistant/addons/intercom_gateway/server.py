import asyncio
import struct
import time
from aiohttp import web

try:
    from speexdsp import EchoCanceller
except ImportError as exc:
    raise ImportError(
        "The 'speexdsp' library (Python binding for libspeexdsp) was not found. "
        "This gateway requires AEC to be active — there is no mode without echo "
        "cancellation. Check that the add-on's Dockerfile installed "
        "'speexdsp-dev'/'swig'/the build toolchain and successfully ran "
        "'pip install speexdsp'."
    ) from exc

# ---------------------------------------------------------------------------
# Central gateway for the Smart Intercom via Home Assistant — conference mixer.
#
# Any number of participants can be "in the call" at the same time — the
# outdoor module (A) and N internal modules (B, C, D...). There's no fixed
# routing or "route" concept: every participant that's streaming audio
# (session open on the ESP32, or browser/app connected) receives the MIX of
# every other active participant, and drops out of the conference as soon as
# it stops streaming (ACTIVE_TIMEOUT).
#
# Home Assistant still decides WHEN a module is streaming (via each module's
# switch.audio_enabled) — the gateway has no notion of "call state", it just
# mixes whoever is currently sending audio. This keeps the same separation
# of responsibilities as the rest of the project: HA = logic, gateway =
# signal processing.
# ---------------------------------------------------------------------------

UDP_IN = 6055
WS_PORT = 8099

SAMPLE_RATE = 8000
FRAME_MS = 20
FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000        # 160 amostras
FRAME_BYTES = FRAME_SAMPLES * 2                       # 320 bytes (int16)
FRAME_FMT = f"<{FRAME_SAMPLES}h"

FILTER_TAIL_MS = 250
FILTER_LENGTH = SAMPLE_RATE * FILTER_TAIL_MS // 1000

# A participant "drops out" of the conference if it doesn't send a new frame
# within this window. Needs to be loose enough to tolerate network jitter
# (frames arrive every 20ms), but short enough to remove someone from the
# call quickly once their module stops streaming.
ACTIVE_TIMEOUT_S = 0.15

WEB_ID = "WEB"  # virtual participant: browser/app over WebSocket

esp_addr = {}          # endpoint -> ip (learned from the first packet received)
last_frame = {}         # endpoint -> (pcm_bytes, monotonic_ts)
last_sent_to = {}        # endpoint -> last mix sent (AEC far-end reference)
echo_cancellers = {}     # endpoint -> EchoCanceller (doesn't apply to WEB)

ws_clients = set()
udp_proto = None


def port_for(endpoint: str) -> int:
    """A=6056, B=6057, C=6058, D=6059... same formula used by the firmware
    (intercom_remote computes local_rx_port_ = 6056 + (endpoint - 'A'))."""
    return 6056 + (ord(endpoint) - ord("A"))


def get_echo_canceller(ep: str):
    if ep not in echo_cancellers:
        echo_cancellers[ep] = EchoCanceller.create(FRAME_SAMPLES, FILTER_LENGTH, SAMPLE_RATE)
    return echo_cancellers[ep]


def mix(frames):
    """Sums several same-size mono int16 PCM buffers, with simple clipping.
    No numpy on purpose — the add-on runs on low-resource ARM boards and the
    data volume per tick (a handful of participants x 160 samples) is
    trivial in pure Python."""
    frames = [f for f in frames if len(f) == FRAME_BYTES]
    if not frames:
        return b"\x00" * FRAME_BYTES
    if len(frames) == 1:
        return frames[0]
    acc = [0] * FRAME_SAMPLES
    for f in frames:
        for i, s in enumerate(struct.unpack(FRAME_FMT, f)):
            acc[i] += s
    clipped = [32767 if v > 32767 else -32768 if v < -32768 else v for v in acc]
    return struct.pack(FRAME_FMT, *clipped)


class UDP(asyncio.DatagramProtocol):
    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if len(data) < 8 or data[:2] != b"IC":
            return
        endpoint = chr(data[2])
        pcm = data[8:]
        if len(pcm) != FRAME_BYTES:
            return  # unexpected frame size — drop it, don't break the mix
        esp_addr[endpoint] = addr[0]
        last_frame[endpoint] = (pcm, time.monotonic())


async def mixer_tick():
    """Runs every FRAME_MS. For each active participant: (1) clean the echo
    from their own mic using what was last sent to them (per-leg AEC, like
    on a telephony conference bridge), (2) build a custom mix of everyone
    EXCEPT themselves, (3) send it."""
    while True:
        await asyncio.sleep(FRAME_MS / 1000)
        now = time.monotonic()

        active = {ep: pcm for ep, (pcm, ts) in last_frame.items() if now - ts < ACTIVE_TIMEOUT_S}
        if len(active) < 2:
            continue  # nobody to conference with yet (0 or 1 participant)

        cleaned = {}
        for ep, pcm in active.items():
            if ep == WEB_ID:
                cleaned[ep] = pcm  # no AEC on the browser/app side (see README)
                continue
            far = last_sent_to.get(ep, b"\x00" * FRAME_BYTES)
            cleaned[ep] = get_echo_canceller(ep).process(pcm, far)

        for ep in active:
            mixed = mix([cleaned[o] for o in active if o != ep])
            last_sent_to[ep] = mixed

            if ep == WEB_ID:
                for ws in list(ws_clients):
                    asyncio.create_task(ws.send_bytes(mixed))
            elif ep in esp_addr:
                udp_proto.transport.sendto(mixed, (esp_addr[ep], port_for(ep)))


async def ws_handler(request):
    ws = web.WebSocketResponse(heartbeat=20)
    await ws.prepare(request)
    ws_clients.add(ws)
    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.BINARY and len(msg.data) == FRAME_BYTES:
                last_frame[WEB_ID] = (msg.data, time.monotonic())
    finally:
        ws_clients.discard(ws)
        # If nobody else is connected over WS, let the WEB participant drop
        # out of the conference on the next tick (natural timeout), without
        # needing an explicit "hangup" message from the client.
    return ws


async def health(request):
    now = time.monotonic()
    active = sorted(ep for ep, (_, ts) in last_frame.items() if now - ts < ACTIVE_TIMEOUT_S)
    return web.json_response({
        "ok": True,
        "active_participants": active,
        "known_endpoints": sorted(esp_addr.keys()),
        "browser_clients": len(ws_clients),
        "aec": "speexdsp",
        "aec_filter_tail_ms": FILTER_TAIL_MS,
    })


async def main():
    global udp_proto
    loop = asyncio.get_running_loop()
    _, udp_proto = await loop.create_datagram_endpoint(lambda: UDP(), local_addr=("0.0.0.0", UDP_IN))
    app = web.Application()
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/health", health)
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, "0.0.0.0", WS_PORT).start()
    asyncio.create_task(mixer_tick())
    print(f"Smart Intercom (central gateway) — UDP {UDP_IN}, WS {WS_PORT}, "
          f"AEC=speexdsp (tail={FILTER_TAIL_MS}ms), N-participant conference")
    await asyncio.Event().wait()


asyncio.run(main())
