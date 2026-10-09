# TurboMedia ACE/CNet 2.3 application

## Scope and authority

TurboMedia's TCP/TLS transport uses the existing Salts CNet Reactor/Proactor owner and its native **Connector / Service Handler** boundary. Each `turbo_transport_t` admits one stream at a time through the owner-local `cnet_manager` API, which is implemented within **Salts::CNet**. There is no second DSO, backend, poll thread, scheduler, Actor bridge or automatic reconnect. This is the first ACE/CNet migration slice, not a declaration that the WebRTC/SFU/IVR or Component layers are complete.

| Object | Owner / capacity | Lifetime |
| --- | --- | --- |
| `cnet_client` | TurboMedia instance if internally created; otherwise caller-owned borrowed client | Remains initialized through Manager destroy |
| `cnet_manager` | A single TurboMedia stream wrapper, exactly one record and one connection credit | Initialized once; destroyed only when drained on the CNet owner thread |
| `cnet_managed_connection` | Generation-checked value, no borrowed reference | Replaced on each successful reservation, invalidated after recycle |
| `cnet_observer.user` | Borrowed TurboMedia transport wrapper | Must outlive terminal callbacks **and** Manager recycle; never freed on failed shutdown |
| `cnet_connection` | CNet transport owner, not Manager's observer record | Explicit close, real terminal callback, bounded recycling |
| HTTP/WebSocket, UDP | CHttp or CNet datagram owners respectively | Not double-wrapped with another TCP manager |

### Bounded synchronous calls and data ownership

Connect, send and receive deadlines cover the whole operation across all CNet
poll turns, not one fresh timeout per empty poll. A connect timeout explicitly
closes only the newly admitted stream; the real terminal must still be observed
and Manager recycled before another generation can connect. A send timeout
returns failure but **does not** authorize application DATA replay: the
original CNet write may still be in flight, so further sends are rejected
until its real completion or terminal. The public signed-int byte count rejects
writes beyond `INT_MAX` before accessing the caller buffer.

Receive demand belongs to CNet from admission until its callback or terminal.
A read timeout does not revoke demand and the next caller cannot add a second
outstanding demand; data delivered while another API polls is buffered as an
owned copy, retained for the next `recv()` (including after the connection
terminal). Callback allocation failure is surfaced, not silently considered
success. HTTP/WebSocket retain their separate CHttp receive contracts.

The admission path is `manager_reserve → manager_connect → CNet poll → real terminal → manager_advance → manager_destroy` (or another bounded reservation after recycle). The Manager consumes a valid reservation even if connect is immediately rejected; no callback is fabricated. The wrapper advances its Manager **after** returning from CNet poll so callbacks cannot recursively run Manager cleanup. Reconnect is initiated only by an explicit caller request and only after the prior record is drained; raw `cnet_connect` is not a fallback.

## Shutdown and sharing

`turbo_transport_disconnect` closes only the transport's own connection. It never stops a caller-owned CNet client or unrelated active connections. `turbo_transport_destroy` returns an error while the Manager still has callback/record obligations, retains the original wrapper, and allows a later owner-thread retry. For an internally created client, Manager destruction precedes `cnet_client_stop/destroy`. HTTP and WebSocket continue to use CHttp's existing lifetime/lease semantics. UDP retains CNet datagram semantics.

## Next ACE/CMeta layers (not implemented by this slice)

The next phase audits the real owner topology for RTSP acceptors and WebRTC/SFU/IVR listeners, including bounded detached handoff and callback settlement. Only a named use case should introduce a CFlow mailbox, with backpressure, payload retention and terminal semantics defined by the media processing domain; CNet itself must remain Actor-neutral. Codec, recognition and service providers can later use canonical CMeta typed Interface/Component declarations with explicit `Salts::Component` graph activation and failure rollback. N→N+1 Plugin publication must fence an exclusive domain-owned listener, release all borrowed calls/objects before the Plugin lease, and preserve the old generation until quiescence. No new OSGi-style runtime is proposed.

## Qualification

Build/test on the **exact** unified Salts 2.3 installed SDK with no legacy fallback. Re-run external-owner shared connection, reconnect, terminal/recycle, TCP/TLS, and cross-platform link gates. Temporary dependency blockers are [Salts #1089](https://github.com/qigao/salts/issues/1089) (wrong floating candidate selected) and [SaltsNet #45](https://github.com/qigao/salts-net/issues/45) (old native ABI admission). Track the broader design in [TurboMedia #153](https://github.com/qigao/turbomedia/issues/153). No performance speedup is claimed without benchmarks.
