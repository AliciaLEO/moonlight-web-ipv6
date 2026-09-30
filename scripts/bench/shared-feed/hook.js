// Injected into a stream page over DevTools, before the stream starts
// (owner_load.py). Records every `stats` message the host sends on the input
// DataChannel that carries the native engine's stage figures — the host's own
// window of present → acquire → convert → encode → queue → send, mean and tail —
// with the page's clock beside it. window.__mwS0.take() hands them over and
// empties the buffer, so one measurement window reads only its own.
// A prototype patch: nothing in the app changes, and a page this was never
// injected into pays nothing.
(() => {
    if (window.__mwS0) return 'already';
    const S = (window.__mwS0 = {
        stats: [],
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
    return 'hooked';
})();
