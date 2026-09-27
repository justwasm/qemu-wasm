/*
 * QEMU WebSocket-multiplexed netdev (qemu-wasm).
 *
 * Bridges a vnet-style 4-byte-BE length-prefixed L2 stream between
 * a JavaScript helper registered on the qemu-wasm Module object and
 * the QEMU nic model. The helper exposes:
 *
 *   bridge.connect(url)             -> returns the bridge handle.
 *   bridge.send(handle, bytes)      -> emits a raw L2 frame.
 *   bridge.recv(handle, callback)   -> subscribes to incoming frames.
 *   bridge.close(handle)            -> tears down the connection.
 *
 * Outbound frames (guest -> bridge): net/wsmux.c wraps the raw L2
 * payload with a 4-byte big-endian length prefix when encode=qemu
 * (the default); encode=raw ships the bytes verbatim.
 *
 * Inbound frames (bridge -> guest): a coroutine awaits the JS-side
 * helper frame-by-frame via the ASYNCIFY-aware js_wsmux_recv entry
 * point and hands each frame to qemu_receive_packet() through the
 * standard NetClientState peer plumbing.
 *
 * See plugin/qemu-playground in the GearShell project for a working
 * vnet bridge.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/coroutine.h"
#include "net/net.h"
#include "clients.h"

/* ASYNCIFY-aware entry points declared by net/wsmux.js. The C side
 * only sees these symbols when the wasm is built with
 * --js-library=net/wsmux.js. On non-emscripten builds the qapi
 * `wsmux` driver is hidden at qapi generation time so this file
 * should not even be compiled; we guard the declarations defensively
 * to keep the source greppable.
 */
#if defined(__EMSCRIPTEN__)
extern int js_wsmux_open(const char *url, const char *bridge);
extern int js_wsmux_send(int handle, const uint8_t *buf, int len);
extern int js_wsmux_recv(int handle, uint8_t *buf, int len,
                         const char *bridge);
extern void js_wsmux_close(int handle);
#endif

#define WSMUX_RECV_BUF (64 * 1024)

typedef struct NetWsMuxState {
    NetClientState nc;
    Coroutine *co;
    char *url;
    char *bridge;
    int handle;
    bool closed;
    bool started;
    int encode; /* NetdevWsMuxEncode: 0 qemu, 1 raw */
} NetWsMuxState;

static bool wsmux_can_receive(NetClientState *ncs)
{
    NetWsMuxState *s = DO_UPCAST(NetWsMuxState, nc, ncs);
    return !s->closed && s->handle > 0;
}

static ssize_t wsmux_send_frame(NetClientState *ncs,
                                const uint8_t *buf, size_t size,
                                bool raw)
{
    NetWsMuxState *s = DO_UPCAST(NetWsMuxState, nc, ncs);
    uint32_t header_buf;
    int header_len = 0;

    if (s->closed || s->handle <= 0) {
        return -1;
    }

#if defined(__EMSCRIPTEN__)
    if (!raw) {
        header_buf = htonl((uint32_t)size);
        if (js_wsmux_send(s->handle, (const uint8_t *)&header_buf,
                          sizeof(header_buf)) < 0) {
            return -1;
        }
        header_len = sizeof(header_buf);
    }
    int ret = js_wsmux_send(s->handle, buf, size);
    if (ret < 0) {
        return ret;
    }
    return header_len + ret;
#else
    /* Non-emscripten: the JS bridge is unavailable, so we cannot
     * forward frames anywhere. Drop them rather than crash; this
     * keeps the binary compilable on host builds for tooling. */
    (void)header_buf;
    return size;
#endif
}

static ssize_t wsmux_receive(NetClientState *ncs,
                             const uint8_t *buf, size_t size)
{
    NetWsMuxState *s = DO_UPCAST(NetWsMuxState, nc, ncs);
    return wsmux_send_frame(ncs, buf, size, s->encode != 0);
}

static void wsmux_link_status_changed(NetClientState *ncs)
{
    NetWsMuxState *s = DO_UPCAST(NetWsMuxState, nc, ncs);
    qemu_set_info_str(&s->nc, "wsmux: %s%s", s->url ? s->url : "",
                      s->nc.link_down ? " (link down)" : "");
}

static void wsmux_cleanup(NetClientState *ncs)
{
    NetWsMuxState *s = DO_UPCAST(NetWsMuxState, nc, ncs);

    s->closed = true;

#if defined(__EMSCRIPTEN__)
    if (s->handle > 0) {
        js_wsmux_close(s->handle);
        s->handle = 0;
    }
#endif

    g_free(s->url);
    s->url = NULL;
    g_free(s->bridge);
    s->bridge = NULL;
}

static void coroutine_fn wsmux_recv_co(void *opaque)
{
    NetWsMuxState *s = opaque;
    uint8_t *buf = NULL;

    buf = g_malloc(WSMUX_RECV_BUF);
    if (!buf) {
        return;
    }

    while (!s->closed && s->handle > 0) {
#if defined(__EMSCRIPTEN__)
        int n = js_wsmux_recv(s->handle, buf, WSMUX_RECV_BUF, s->bridge);
        if (n <= 0 || s->closed) {
            /* n == 0 is the JS bridge's signal that the handle has
             * been torn down (see net/wsmux.js wsmuxReadFrame). Treat
             * it the same as -1: leave the recv loop and let the
             * coroutine exit. */
            break;
        }
        /* Hand the frame to the peer nic. qemu_receive_packet walks
         * filters and the receiving nic's receive callback (e1000,
         * virtio, etc.). */
        qemu_receive_packet(&s->nc, buf, n);
#else
        break;
#endif
    }

    g_free(buf);
}

static NetClientInfo net_wsmux_info = {
    .type = NET_CLIENT_DRIVER_WSMUX,
    .size = sizeof(NetWsMuxState),
    .can_receive = wsmux_can_receive,
    .receive = wsmux_receive,
    .cleanup = wsmux_cleanup,
    .link_status_changed = wsmux_link_status_changed,
};

int net_init_wsmux(const Netdev *netdev, const char *name,
                   NetClientState *peer, Error **errp)
{
    const NetdevWsMuxOptions *opts;
    NetClientState *ncs;
    NetWsMuxState *s;
    int handle = 0;

    assert(netdev->type == NET_CLIENT_DRIVER_WSMUX);
    opts = &netdev->u.wsmux;

    if (!opts->url || !*opts->url) {
        error_setg(errp, "wsmux: url is required");
        return -1;
    }

    ncs = qemu_new_net_client(&net_wsmux_info, peer, name, netdev->id);
    if (!ncs) {
        return -1;
    }

    s = DO_UPCAST(NetWsMuxState, nc, ncs);
    s->closed = false;
    s->started = false;
    s->co = NULL;
    s->url = g_strdup(opts->url);
    s->bridge = g_strdup(opts->bridge ? opts->bridge : "wsmuxBridge");
    /* QAPI optional fields are exposed as `has_X` + `X`; default is
     * the first enum value (`qemu`, index 0). */
    s->encode = (opts->has_encode &&
                 opts->encode == NETDEV_WSMUX_ENCODE_RAW) ? 1 : 0;

    qemu_set_info_str(&s->nc, "wsmux: %s", s->url);

#if defined(__EMSCRIPTEN__)
    handle = js_wsmux_open(s->url, s->bridge);
    if (handle <= 0) {
        error_setg(errp, "wsmux: js bridge '%s' did not accept url '%s' "
                   "(handle=%d). Make sure Module['%s'] is registered "
                   "before the wasm starts.",
                   s->bridge, s->url, handle, s->bridge);
        qemu_del_net_client(ncs);
        return -1;
    }
    s->handle = handle;
#else
    s->handle = 0;
    warn_report("wsmux: built without emscripten JS bridge; "
                "netdev '%s' will silently drop frames", name);
#endif

    s->co = qemu_coroutine_create(wsmux_recv_co, s);
    if (s->co) {
        qemu_coroutine_enter(s->co);
        s->started = true;
    }

    return 0;
}