[← Backend](03-Backend.md) · **Frontend** · [Next: Streaming & Transports →](05-Streaming-and-Transports.md)

---

# 4. Frontend (Vanilla JS)

The frontend is a **framework-less, build-less** web app served directly by the backend. ES6 modules provide structure; there is no bundler, no transpiler, no `node_modules` at runtime. Tooling (Prettier, ESLint, Vitest, tsc in advisory `checkJs` mode) exists only for development.

## 4.1 File architecture

```
frontend/
├── index.html                  # single page shell: header + #main-content + footer, PWA meta
├── manifest.webmanifest        # PWA manifest (installable, icons)
│                               # /version.json is synthesised by the backend from
│                               # the app version — VersionGuard polls it to force-reload stale PWAs
├── css/
│   ├── tokens.css              # design tokens (Cyberpunk-2077-inspired theme)
│   ├── base.css / layout.css / components.css / stream.css
│   └── views/                  # per-view styles: admin, apps, hosts, login, settings
├── js/
│   ├── app.js                  # entry point, navigation, view/overlay orchestration
│   ├── api/
│   │   ├── BackendClient.js    # REST client (auth-aware: reloads on 401)
│   │   ├── WebRtcDataChannel.js# webrtc-dc transport client (frame assembly, IDR, stats)
│   │   └── WebRtcMedia.js      # webrtc-media transport client (RTP tracks, <video>)
│   ├── audio/
│   │   ├── AudioPipeline.js    # WebCodecs Opus decode → AudioWorklet playback
│   │   ├── audio-processor.js  # AudioWorkletProcessor: adaptive jitter buffer + WSOLA
│   │   ├── audio-decode-worker.js / opusWasm.js  # WASM Opus fallback decode path
│   │   └── iosAudioUnlock.js   # iOS silent-switch workaround (looping silent <audio>)
│   ├── stream/
│   │   ├── VideoDecodeWorker.js# OffscreenCanvas decode+render worker (video_worker: auto|on|off)
│   │   ├── JitterController.js # adaptive jitterBufferTarget (webrtc-media), AIMD control law
│   │   ├── FramePacer.js      # adaptive presentation reserve (DataChannel paths, mw_pacing)
│   │   ├── GamepadManager.js   # Gamepad API → input DC, read at 250 Hz (mapped pads, single-pad guests, rumble)
│   │   ├── gamepadMapping.js   # non-standard pads: user / built-in / Android / SDL DB resolution, device kind, remap capture
│   │   ├── gamepadDb.js        # GENERATED from SDL_GameControllerDB (npm run gamepad-db), loaded on demand
│   │   ├── LatencyProbe.js     # click→photon probe (debug builds, mwLatency.run())
│   │   └── renderers/          # VideoRenderer base + Canvas2D / WebGl / WebGpu / VideoElement + factory
│   ├── ui/
│   │   ├── HostListView.js     # main view: host boxes with inline app grids
│   │   ├── StreamView.js       # the streaming overlay (largest module: decode, render, input, stats)
│   │   ├── StreamViewKeyboard.js / StreamViewTouch.js / StreamViewFullscreen.js
│   │   ├── AdminView.js / SettingsView.js / SetupView.js / LoginView.js
│   │   ├── GamepadRemapDialog.js / GamepadArt.js / DeviceArt.js  # Test & Remap: wizard, drawn pad, radio, flight stick, wheel
│   │   ├── PairDialog.js / Toast.js / icons.js
│   ├── models/                 # Host.js, App.js
│   ├── util/                   # Mp4Muxer (NAL/avcC/hvcC/codec strings), Av1Utils (OBU parse),
│   │                           # SdpUtils (forceOpusStereo), AutoBitrate (recommended kbps),
│   │                           # BrowserDetect, VersionGuard, escapeHtml
│   ├── i18n/i18n.js            # homemade runtime i18n (en/fr/zh, Tolgee-compatible JSON)
│   └── vendor/opus-decoder.min.js
├── locales/{en,fr,zh}.json     # ~430 keys, nested namespaces
├── test/                       # Vitest suites (jsdom) for the pure-logic modules
└── package.json                # dev tooling only (prettier, eslint, vitest, typescript)
```

## 4.2 Navigation model

From the `app.js` header comment — one main view plus overlays:

- **Main view** (with a history entry): `hosts` at `/` — host boxes carry their own app grids; apps launch directly from a host box.
- **Overlays** (no persistent history entry, guard `pushState`):
  - `admin` → URL `/admin` (survives refresh)
  - `settings` → URL `/settings`
  - `streaming` → no URL change (fullscreen `StreamView`)
- Switching overlays uses `replaceState`; the Back button/gesture closes an overlay instead of leaving the app.
- Guard states protect against accidental back-navigation during a stream.

Auth flow: `LoginView` is shown when the backend answers 401 (PIN entry or certificate-file upload); `BackendClient` force-reloads the page whenever a session expires or is revoked mid-use. On first run (macOS/Linux), `/setup` renders `SetupView` (config → live progress checklist → done).

## 4.3 StreamView — the streaming overlay

`ui/StreamView.js` owns the whole in-stream experience:

- **Transport client** — instantiates `WebRtcDataChannel` or `WebRtcMedia` per the `transport_chain` echoed by `/start`, and walks the chain (relaunch with `transport_index+1`) when a transport fails to establish — but *only* then: a launch that never returns a session is retried on the same index instead of costing a rung ([Streaming §5.1](05-Streaming-and-Transports.md)). WSS mode reuses the DC framing over one socket.
- **Two live views at once** — `app.js` owns a second `StreamView` (the standby stream slot) during a **seamless quality transition**: a quality/codec/transport change launches the other slot, and the display, input and audio are handed over on its first decoded frame, never through a loader. The outgoing view is retired with `quit({retire:true})` so the shared Sunshine app survives ([Streaming §5.6](05-Streaming-and-Transports.md#56-session-lifecycle--teardown-discipline)). The promoted view mounts its **own** `ShareMenu` in `activate()` — the retiring view carries its header away, and an owner who lost the Share button would be down to the host list's kebab to manage guests mid-stream.
- **Sharing** (`ui/ShareBoard.js`, launched by `ui/ShareMenu.js`) — one overlay, four rows (the owner and the three guests), reached from two places: the **Share** button in the stream header, and the **Share** entry in a host's kebab, where nothing is streaming and the board picks the app the invitation leads to. `ShareMenu` is only the button and its guest count. The board's overlay is listed in `StreamView.isLocalKeyboardTarget` — without that, typing a guest's name over a running stream would go to the host instead. While `hasActiveShare()` holds, the automatic quality ladder is suspended ([Backend §3.3](03-Backend.md)) and **Stop** first asks Leave (keep the app and the guests running, `keep_host_session`) or Stop everything. On a native host the board also carries the **guests' picture** — 720p, 1080p or 1440p, one for every row (`POST /api/share/feed`) — since its guests watch one shared feed; `PlayerJoinView` then states that height instead of offering its three quality buttons, and tests HEVC before joining (`hevcClientDecodes`, `util/BrowserDetect.js`): a browser without it joins in H.264 at once. A guest page already on the feed that gets a `feedcodec` notice (another guest made the feed H.264) leaves and rejoins through the codec fallback (`StreamView._followFeedCodec`).
- **Decode** — WebCodecs `VideoDecoder` fed Annex-B access units; `Mp4Muxer.js` extracts SPS/PPS(/VPS), builds `avcC`/`hvcC` descriptions and codec strings; `Av1Utils.js` does the AV1 sequence-header equivalent. Decode+render move to an OffscreenCanvas worker only when `video_worker` is `on` (`auto` resolves to off: measured no gain, and WebGPU inside a worker costs 22 ms of hand-off on macOS — [ch. 15](15-Client-Presentation-Benchmarks.md#152-decision-1--canvas2d-on-the-main-thread-is-the-default)).
- **Render** — via the renderer abstraction (next section), sized with HiDPI awareness, fullscreen through `StreamViewFullscreen` (header button + CSS fallback for iOS).
- **Input** — keyboard (`StreamViewKeyboard`: Escape forwarded as a key; shortcuts on `Ctrl/Cmd+Alt+Shift` combos — `Q` quit, `X` fullscreen, `Z` release, `M` mouse mode, `←`/`→` **move the host one desktop over**, translated into the chord that host's own OS uses for desktops (`HostOsProbe` tells us which), never forwarded as the arrow itself). The cheat-sheet itself lives in `util/shortcutsHelp.js`, shown twice: the slide that flashes over the stream for 5s, and the **last section of the settings page** — same rows, same device rule (touch → gestures, keyboard otherwise), so what the settings page teaches is what the stream obeys, mouse (pointer-lock "gaming mode" vs absolute; `_mouseFocused` model), touch (`StreamViewTouch`: trackpad model by default — 1 finger moves the cursor, 2 scroll, a 3-finger horizontal swipe at base zoom changes desktop on the host; **touch-screen mode** instead places the cursor where you touch and turns the 1-finger drag into an all-directions scroll, long press still grabs for drag & hold), gamepads (`GamepadManager`, polled per frame, change-only snapshots, rumble feedback; a pad without a standard layout is read through `gamepadMapping.js` — the user's own layout, Chrome Android's pre-sorted one, or SDL_GameControllerDB — and one nothing maps is offered the remap wizard, `GamepadRemapDialog` over the SVG pad of `GamepadArt`; a share guest forwards one pad, as controller 0), virtual keyboard on mobile (input-event diffing with a sentinel — the only reliable capture across iOS/Gboard), clipboard `paste` events.
- **Stats overlay** — FPS, bitrate, decode/render timings, RTT; **latency is displayed as a sum of measured legs** (host processing + ENet RTT/2 + client pipeline per frame) — never a cross-machine clock offset (which freezes on the offset error). Frame loss is split the way moonlight-qt splits it, because the two have different fixes: **network** (`frameId` gaps — frames that never arrived, i.e. packet loss, each costing an IDR round trip) vs **jitter** (frames that arrived and decoded, then lost their turn at the render stage — uneven arrival, not loss; this is what the `FramePacer` reserve drives down). Both are DataChannel/WSS only.
- **Degradation & resilience** — IDR request throttling with exponential backoff, frame-gap detection via `frameId`, decoder-error fallback, session teardown ordering (`webrtc.close()` **before** HTTP `/quit`; `_closed`/`_stopping` guards; 10 s grace period after a WS close when ICE is still connected — first launches often reconnect).

## 4.4 Renderers — why canvas *and* video

`stream/renderers/createRenderer.js` picks among four sinks behind one `VideoRenderer` interface. The choice is made in `StreamView` from three inputs — Enhancer on/off, HDR on/off, codec — and every default below was measured ([ch. 15](15-Client-Presentation-Benchmarks.md)):

| Renderer | Sink | When |
|---|---|---|
| `Canvas2DRenderer` | `<canvas>` (2D, `desynchronized`) | **The default**: SDR, Enhancer off. The fastest first impression on Mac and Windows, no GPU device created. Also the fallback when WebGL2/WebGPU are unavailable or fail to init (the WebGPU probe runs `requestAdapter/requestDevice` *before* `getContext('webgpu')`, leaving the canvas clean for 2D). |
| `WebGlRenderer` | `<canvas>` (WebGL2, `desynchronized`) | **SDR + Enhancer**: FSR1 (EASU+RCAS, 2 passes — `auto` on desktop), SGSR1 (1 pass — `auto` on mobile/tablet) or NIS, ported to fragment shaders; no GPU wait after the draw. |
| `WebGpuRenderer` | `<canvas>` (WebGPU) | **Every HDR path** (Chrome has no HDR surface for WebGL2 or Canvas2D), and the WebGPU twins of the three upscalers (debug menu). Pass 0 imports the frame (`importExternalTexture`, or `copyTo` of the raw PQ planes for HDR), then the upscaler. `draw()` **must await `onSubmittedWorkDone()`** — without GPU backpressure the queue backlogs and latency explodes. |
| `VideoElementRenderer` | `<video>` via `MediaStreamTrackGenerator` | **HDR with HEVC only** (hardware decode, no software path for `copyTo`). The slowest presenter measured (+35–45 ms click-to-photon: compositor vsync plus the generator's queue), kept only where it is the sole HDR route. Same WebCodecs decode, only the sink swaps. |

The **webrtc-media transport** natively renders into a `<video>` element (RTP → browser decoder), which is why it is both the lowest-CPU path and incompatible with WebGPU enhancement — the transport chain reorders accordingly (see [Transports](05-Streaming-and-Transports.md)).

## 4.5 Audio pipeline

- moonlight-common-c delivers **encoded Opus** (never PCM); the backend forwards those bytes verbatim on every transport, so decode happens client-side.
- On **both WebRTC transport families** (`webrtc-dc` *and* `webrtc-media`) audio is a native RTP Opus track handed to an `<audio>` element via `pc.ontrack` — the browser owns the jitter buffer, FEC and PLC (this is what fixed the periodic micro-dropouts of the old ordered audio DataChannel). Nothing below applies there.
- **`wss` only**: `AudioPipeline` decodes with the WebCodecs `AudioDecoder` (WASM `opus-decoder` fallback in a worker) and transfers Float32 PCM to `audio-processor.js` (AudioWorklet, real-time thread).
- That worklet implements an **adaptive jitter buffer** (~60 ms base, grows to ~160 ms on underruns/near-underruns, decays slowly) and optional **WSOLA time-stretch** (`audio_time_stretch`, default on) to absorb clock drift without latency.
- Stereo is forced in SDP (`stereo=1` via `SdpUtils.forceOpusStereo`) — browsers default their Opus answer to mono. **No gain is ever applied in JS** (volume issues are host-side sink issues).
- iOS: sound on the built-in speaker with the silent switch on requires a looping silent `<audio>` element (`iosAudioUnlock.js`) to escape the *ambient* audio category.

## 4.6 i18n

`js/i18n/i18n.js` is a homemade ~zero-dependency runtime: loads `/locales/<lang>.json` (English always loaded as fallback), `t(key, vars)` with `{{var}}` interpolation, `applyDOM()` for `[data-i18n]`/`[data-i18n-attr]` static markup, language persisted in `localStorage` (`mw-lang`). The JSON is Tolgee-compatible (a self-hosted Tolgee docker-compose lives in `tolgee/`); `frontend/scripts/check-i18n.cjs` validates catalog completeness. See `docs/i18n.md`.

## 4.7 Quality tooling

| Tool | Role | Gate |
|---|---|---|
| Prettier + ESLint | format + lint (`npm run check`) | CI-blocking |
| Vitest (jsdom) | unit tests in `frontend/test/` | CI-blocking, **70% coverage gate scoped to pure-logic modules** |
| TypeScript (`tsconfig.json`, `checkJs`) | static analysis over JSDoc | advisory |
| `VersionGuard` | runtime: force-reloads a stale PWA after a deploy (never during a stream) | — |

## 4.8 Controller compatibility

How a pad is read (`gamepadMapping.js`, first match wins): the user's own layout → the browser's `standard` mapping → a profile built in for a device SDL does not know (desktop, by USB vendor:product: EdgeTX/OpenTX radios, Logitech's G923 for Xbox) → Chrome Android's pre-sorted layout → SDL_GameControllerDB (desktop, by USB vendor:product) → nothing, and the remap wizard is offered. A pad Windows reports under a generic name ("HID-compliant game controller", e.g. an Xbox pad over Bluetooth) is named from its USB ids.

**Tested on 24/09/2026** — Windows 11, Chrome, Settings → Controllers then a stream, buttons, sticks and triggers checked; Firefox used as a second opinion on the failing cases.

| Controller | Connection / mode | Driver model | Result |
|---|---|---|---|
| Xbox One S Controller | Bluetooth | HID + XInput layer | ✅ |
| Switch Pro Controller | USB‑C | Nintendo HID protocol | ✅ |
| Switch Pro Controller | Bluetooth | Nintendo HID protocol | ❌ |
| GameSir X2 Lightning | — | — | ✅ |
| 8BitDo SN30 Pro | USB‑C (Xbox 360) | XInput | ✅ |
| 8BitDo SN30 Pro | Bluetooth, Start+X (Xbox One S) | HID + XInput layer | ✅ |
| 8BitDo SN30 Pro | Bluetooth, Start+A (PS4) | DInput (HID) | ✅ |
| 8BitDo SN30 Pro | Bluetooth, Start+B (8BitDo) | DInput (HID) | ✅ |
| 8BitDo SN30 Pro | Bluetooth, Start+Y (Switch Pro) | Nintendo HID protocol | ❌ |

**The Switch protocol over Bluetooth fails in the browser, not in MoonlightWeb** — the genuine Pro Controller and the 8BitDo in Switch mode alike. Chrome drives Nintendo pads through its own driver, which must initialize the pad first; over Bluetooth on Windows that fails, and the pad never reaches `navigator.getGamepads()` (a gamepad tester page shows nothing either, `chrome://device-log` stays empty, closing Steam changes nothing). Firefox does list it (`057e-2009-Wireless Gamepad`), buttons working but not the sticks. Over USB the same pad works. Rewriting the Switch protocol (WebHID) was ruled out: use USB, or another mode of the pad — XInput first, for rumble and no layout to guess.

To tell whether a failing pad is ours or the browser's: open a gamepad tester page in the same browser. If it sees nothing, neither can we. If it sees nothing while Windows does (`joy.cpl`), restart the browser: a Chrome left open for days, its update pending, once stopped seeing every HID device (01/10/2026).

### RC radios, flight sticks and wheels

They reach the game as every pad does, as an **Xbox 360 controller**: two sticks of 16 bits, two triggers of 8 bits (256 steps), 15 buttons and the d-pad. What a device has beyond that is not sent, and no force feedback comes back. Recreating the device itself on the host is another track, the [HID passthrough study](../design/hid-passthrough-study.md).

| Device | Connection / mode | Laid out by | Read on |
|---|---|---|---|
| EdgeTX and OpenTX radios: Radiomaster TX12, TX16S, Zorro, Boxer, Pocket, MT12; Jumper; BetaFPV LiteRadio 3 Pro; FrSky and Flysky under EdgeTX | USB, "USB Joystick (HID)", **Classic** mode (the default) | a **built-in profile**, on plug-in (`1209:4f54`) | Radiomaster TX12, EdgeTX, Windows 11 + Chrome 154, 01/10/2026 |
| Logitech G923 for Xbox One and PC | USB, **G HUB** running on the client (it puts the wheel in its PC mode, `046d:c26e`) | a **built-in profile**, on plug-in | Windows 11 + Chrome 154, 02/10/2026 |
| Logitech G29 and G923 for PlayStation | USB (G29: selector on PS3) | the wizard, once, in a wheel's order (recognized as a wheel) | — |
| Flight sticks and HOTAS, other wheels and pedals; Ethos and DJI radios; EdgeTX in "Advanced" USB mode | USB | the wizard, once | — |

**The radio profile** reads Mode 2 in the default AETR channel order:

- yaw (CH4) → left stick X, throttle (CH3) → left stick Y, throttle up = stick up; roll (CH1) → right stick X, pitch (CH2) → right stick Y;
- CH5 → LT and CH6 → RT, over the whole axis: a switch at −100, 0, +100 gives 0, 128, 255. **A channel the radio's model leaves unmixed sits at 0, a trigger half pressed**: mix CH5 and CH6 to switches in the model, or lay them out again in the wizard;
- CH9-16 (the radio's buttons 0-7, pressed when the channel is above 0) → A, B, X, Y, LB, RB, Back, Start. CH7-8 go nowhere: an Xbox pad has six analog channels;
- Mode 1, another channel order or the Advanced mode: the wizard.

**The G923 for Xbox profile:**

- the rim → left stick X; accelerator → RT, brake → LT;
- right paddle → RB, left paddle → LB; A, B, X, Y, View → Back, Menu → Start, LSB, RSB, the Xbox button and the d-pad, where an Xbox pad has them;
- the clutch, + and −, the dial and its Enter, a shifter's gears go nowhere: an Xbox pad has no third trigger and no button left.

**Plugged in without G HUB, the G923 for Xbox stays an Xbox device** (`046d:c26d`) that neither the browser nor `joy.cpl` lists: install G HUB on the client.

**The device's kind** (gamepad, RC radio, flight stick, wheel) comes from the user's choice, then the USB ids, then the name (`edgetx`, `hotas`, `rudder`, `wheel`, `fanatec`…), a gamepad otherwise. Settings → Controllers shows it as an icon. **Test** and **Remap** draw that device, each control tagged with what the game gets (LT, A, LS…): a radio's gimbals and switches, a flight stick's grip, twist and throttle, a wheel's rim, paddles and pedals (the clutch greyed: it has no place on an Xbox pad). The wizard asks in the device's words and order (yaw, throttle, roll, pitch; steering, accelerator, brake, paddles). The dialog's **Type** selector corrects a wrong guess, and is saved on its own.

**An axis without a spring** (a throttle, a pedal) rests at an end stop. The wizard reads its direction from that rest, not from the move, so a throttle parked down never comes out inverted. The rest is taken when the wizard starts: start it with the throttle down.

**Settings in the game**, so that it passes the device's travel through untouched:

| Game | Settings |
|---|---|
| Liftoff | calibrate the "Xbox 360" controller in its input settings, dead zones at 0 |
| Assetto Corsa Competizione | steering filter 0, speed sensitivity 0, steering linearity 1 |
| Automobilista 2 | controller filtering off |
| Forza Motorsport | steering "Simulation", axis dead zone inside 0, outside 100 |

**On the client, Logitech G HUB** — where the wheel is plugged in; on the host it never sees the wheel. Set the wheel's rotation angle there (the game sees a stick: the rim's full turn is the stick's full travel), and turn on the centering spring: no force feedback comes back from the game, and without the spring the rim stays where it was left.

**What the game receives** shows on a Windows host in `joy.cpl`: a "Controller (XBOX 360 For Windows)", whose axes move with the rim, the pedals or the radio's sticks.

**Limits.** If the link goes silent for 3 s, or the wizard opens mid-game, sticks go back to the center and triggers to 0: a drone's throttle drops to mid-stick and the flight is lost, though arming on CH5 falls back too, which disarms; a wheel's pedals are released. A user's layout belongs to one browser (`localStorage`); built-in profiles need nothing.

---

[← Backend](03-Backend.md) · [Home](Home.md) · [Next: Streaming & Transports →](05-Streaming-and-Transports.md)
