/*
 * MoonlightWeb — bootstrap. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The service worker: what makes the application look like an ordinary website
 * when there is no website.
 *
 * The page is served from an address that belongs to the introduction server,
 * but the application it runs comes from the user's own machine and reaches the
 * browser over a data channel. Something has to stand between the two, because
 * the application's own code does the ordinary thing — <script src>, CSS url(),
 * fetch('/api/…') — and none of that knows about data channels.
 *
 * Two jobs, and the split matters:
 *
 *   static files   answered from the cache the bootstrap filled over the
 *                  channel. They are the same bytes for every visitor and they
 *                  never change between updates, so a copy is exactly right.
 *
 *   everything     handed to the page, which owns the live connection. A worker
 *   else           cannot hold one: WebRTC is not available in this context, and
 *                  that is not an oversight to work around — it is why the
 *                  connection lives in the page and only its RESULTS come here.
 *
 * A worker has no memory between wake-ups, so nothing is kept in a variable
 * here that matters. The client is looked up on every request.
 */

const SHELL_CACHE = 'mw-shell';

/** How long a request may wait for the page to answer before it is given up. */
const CLIENT_TIMEOUT_MS = 30000;

/**
 * A shell from before v0.3.1 never asks its host whether it is still current.
 *
 * It only learns of an update when the machine's link — /<id> — is opened,
 * because that is the one address that runs the bootstrap. Opened from a home
 * screen icon or from the bare root, it serves itself forever. The v0.3.1
 * application checks on every start; its VersionGuard carries this key, the
 * older one does not, and that is how the two are told apart.
 */
const SHELL_GUARD_FILE = '/js/util/VersionGuard.js';
const SHELL_GUARD_MARK = 'mw-shell-recheck';

/**
 * When this worker last sent a stale shell back through the link.
 *
 * Kept in a cache of its own, because a worker has no memory between wake-ups
 * and the bootstrap empties 'mw-shell' whenever it refills it. The note is what
 * stops a loop: a host still on the old version hands back the same old shell,
 * and the bootstrap then lands on the root again — which must now open it.
 */
const NOTES_CACHE = 'mw-sw-notes';
const STALE_NOTE = '/stale-shell-redirect';

/** One trip back through the link per stale shell per this long. */
const STALE_RETRY_MS = 60 * 60 * 1000;

self.addEventListener('install', () => {
    // Take over straight away. The bootstrap registers this worker and then
    // navigates, and waiting a lifecycle would mean that navigation is the one
    // request nobody serves.
    self.skipWaiting();
});

self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
});

/**
 * Ask the page that owns the tunnel to make this request for us.
 *
 * The client is resolved by id first — that is the page the request came from —
 * and only then by "any window", which covers a request from a worker or an
 * <img> whose client id the browser did not attach.
 */
async function askThePage(request, clientId) {
    let client = clientId ? await self.clients.get(clientId) : null;
    if (!client) {
        const windows = await self.clients.matchAll({ type: 'window' });
        client = windows[0];
    }
    if (!client) return new Response('no page holds the connection', { status: 503 });

    const body =
        request.method === 'GET' || request.method === 'HEAD'
            ? null
            : await request.clone().arrayBuffer();

    const headers = [];
    request.headers.forEach((value, name) => headers.push([name, value]));

    return new Promise((resolve) => {
        const channel = new MessageChannel();
        const timer = setTimeout(
            () => resolve(new Response('the connection did not answer', { status: 504 })),
            CLIENT_TIMEOUT_MS,
        );

        channel.port1.onmessage = (event) => {
            clearTimeout(timer);
            const answer = event.data;
            if (!answer || answer.error) {
                resolve(
                    new Response(answer?.error || 'the connection failed', {
                        status: 502,
                    }),
                );
                return;
            }
            resolve(
                new Response(answer.status === 204 || answer.status === 304 ? null : answer.body, {
                    status: answer.status,
                    headers: answer.headers,
                }),
            );
        };

        client.postMessage(
            {
                type: 'mw-tunnel-request',
                method: request.method,
                url: request.url,
                headers,
                body,
            },
            [channel.port2],
        );
    });
}

/**
 * Fall back to the bootstrap when the cache holds nothing.
 *
 * Fetched from the network rather than written out here, so there is one entry
 * page rather than a second, uglier one that drifts. `?mw=pick` keeps this
 * request out of the branch above and stops it looping back into the cache.
 */
async function bootstrapPage() {
    try {
        return await fetch('/?mw=pick', { cache: 'no-store' });
    } catch {
        return new Response('Open your machine’s link again — it ends in 26 characters.', {
            status: 200,
            headers: { 'Content-Type': 'text/plain; charset=utf-8' },
        });
    }
}

/** Whether the cached application checks its own version when it starts. */
async function shellChecksItself(cache) {
    const guard = await cache.match(SHELL_GUARD_FILE);
    if (!guard) return false;
    return (await guard.text()).includes(SHELL_GUARD_MARK);
}

/**
 * Whether a stale shell should be sent back through the link now.
 *
 * Writes the note as it answers yes, so the navigation the bootstrap ends with
 * finds it and opens the shell instead of turning round again.
 */
async function timeForStaleRedirect() {
    const notes = await caches.open(NOTES_CACHE);
    const note = await notes.match(STALE_NOTE);
    const last = note ? Number(await note.text()) : 0;
    if (last && Date.now() - last < STALE_RETRY_MS) return false;
    await notes.put(STALE_NOTE, new Response(String(Date.now())));
    return true;
}

/**
 * A page that goes to the machine's link, which the bootstrap answers.
 *
 * Only the page can read which machine that is: the old bootstrap left the
 * identifier in localStorage, which a worker cannot see. With none there, the
 * reload finds the note written and opens the shell as before.
 */
function backThroughTheLink() {
    const html =
        '<!doctype html><meta charset="utf-8"><script>' +
        'var id=null;' +
        "try{id=(JSON.parse(localStorage.getItem('mw-shell-stamp')||'null')||{}).hostId}catch(e){}" +
        "if(typeof id==='string'&&/^[0-9a-z]{26}$/.test(id))location.replace('/'+id);" +
        'else location.reload();' +
        '</script>';
    return new Response(html, {
        status: 200,
        headers: { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' },
    });
}

self.addEventListener('fetch', (event) => {
    const request = event.request;
    const url = new URL(request.url);

    // Anything not on this origin is none of our business.
    if (url.origin !== self.location.origin) return;

    // The bootstrap's own files stay on the network. They are the pinned,
    // published set that a watchdog compares against a reference copy, and
    // serving them from a cache filled over the tunnel would quietly move them
    // out from under that check.
    //
    // They live under /v1/ (the protocol shape they speak — a /v2/ would sit
    // beside them, hence the pattern rather than a list), and the root names
    // stay for the application: it imports /tunnel.js by that name, and the
    // server answers it with the v1 file.
    //
    // `?mw=pick` joins them: it is how a page that found no machine to talk to
    // asks for the entry page back. Answering that from the cache would return
    // the very application it just gave up on.
    if (
        url.pathname === '/sw.js' ||
        url.pathname === '/boot.js' ||
        url.pathname === '/tunnel.js' ||
        url.pathname === '/pairing.js' ||
        url.pathname === '/frame-guard.js' ||
        /^\/v\d+\//.test(url.pathname) ||
        url.searchParams.get('mw') === 'pick' ||
        /^\/[0-9a-z]{26}\/?$/.test(url.pathname)
    )
        return;

    event.respondWith(
        (async () => {
            const cache = await caches.open(SHELL_CACHE);

            if (request.mode === 'navigate') {
                const shell = await cache.match('/index.html');
                if (!shell) return await bootstrapPage();
                if (!(await shellChecksItself(cache)) && (await timeForStaleRedirect())) {
                    return backThroughTheLink();
                }
                return shell;
            }

            // /version.json is how the application notices the host was updated,
            // so it must never come from the copy it is checking.
            if (url.pathname !== '/version.json') {
                const hit = await cache.match(url.pathname);
                if (hit) return hit;
            }

            return askThePage(request, event.clientId);
        })(),
    );
});
