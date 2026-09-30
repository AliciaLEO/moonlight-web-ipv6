// Injected into a stream page over DevTools, before the stream starts
// (owner_load.py). Records every `stats` message the host sends on the input
// DataChannel that carries the native engine's stage figures — the host's own
// window of present → acquire → convert → encode → queue → send, mean and tail —
// with the page's clock beside it. window.__mwS0.take() hands them over and
// empties the buffer, so one measurement window reads only its own.
// A guest's page also keeps what each of its joins was answered (the codec its
// worker streams: the shared feed's, S6) and the `feedcodec` notices it got —
// window.__mwS0.joins, window.__mwS0.notices.
// A prototype patch: nothing in the app changes, and a page this was never
// injected into pays nothing.
(() => {
    if (window.__mwS0) return 'already';
    const S = (window.__mwS0 = {
        stats: [],
        joins: [],
        notices: [],
        // The page's own word on its recoveries (keyframes asked, frame gaps,
        // decoder errors), newest 300.
        logs: [],
        take() {
            const out = this.stats;
            this.stats = [];
            return out;
        },
    });
    const desc = Object.getOwnPropertyDescriptor(RTCDataChannel.prototype, 'onmessage');
    if (!desc || !desc.set) return 'no onmessage accessor';
    Object.defineProperty(RTCDataChannel.prototype, 'onmessage', {
        configurable: true,
        enumerable: desc.enumerable,
        get: desc.get,
        set(fn) {
            const dc = this;
            const wrapped =
                typeof fn === 'function'
                    ? function (evt) {
                          try {
                              if (
                                  typeof evt.data === 'string' &&
                                  evt.data.indexOf('"feedcodec"') >= 0
                              ) {
                                  S.notices.push([performance.now(), JSON.parse(evt.data)]);
                              }
                              if (
                                  dc.label === 'input' &&
                                  typeof evt.data === 'string' &&
                                  evt.data.indexOf('"stages"') >= 0
                              ) {
                                  const msg = JSON.parse(evt.data);
                                  if (msg && msg.stages) S.stats.push([performance.now(), msg]);
                              }
                          } catch (e) {
                              // never in the app's way
                          }
                          return fn.call(this, evt);
                      }
                    : fn;
            desc.set.call(this, wrapped);
        },
    });
    const RECOVERY = /IDR|keyframe|gap|decoder|reference|fallback|feed/i;
    for (const level of ['log', 'warn', 'error']) {
        const orig = console[level];
        console[level] = function (...args) {
            try {
                const text = args.map((a) => (typeof a === 'string' ? a : String(a))).join(' ');
                if (RECOVERY.test(text)) {
                    S.logs.push([performance.now(), level, text.slice(0, 300)]);
                    if (S.logs.length > 300) S.logs.shift();
                }
            } catch (e) {
                // never in the app's way
            }
            return orig.apply(this, args);
        };
    }
    const fetch0 = window.fetch;
    window.fetch = async function (input, init) {
        const res = await fetch0.call(this, input, init);
        try {
            const url = typeof input === 'string' ? input : (input && input.url) || '';
            if (url.indexOf('/player/join') >= 0) {
                res.clone()
                    .json()
                    .then((j) => S.joins.push([performance.now(), j.videoCodec || j.error || '?']))
                    .catch(() => {});
            }
        } catch (e) {
            // never in the app's way
        }
        return res;
    };
    return 'hooked';
})();
