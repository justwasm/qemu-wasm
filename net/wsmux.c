/*
 * QEMU WebSocket-multiplexed netdev (qemu-wasm).
 *
 * Bridges a vnet-style 4-byte-BE length-prefixed L2 stream between
 * a JavaScript helper registered on the qemu-wasm Module object and
 * the QEMU nic model. The helper exposes:
 *
 *   bridge.connect(url)          -> returns the bridge handle.
 *   bridge.send(handle, bytes)   -> emits a raw L2 frame.
 *   bridge.recv(handle, callback) -> subscribes to incoming frames.
 *
 * This is the build-only stub. The host page is responsible for the
 * actual JavaScript helper. See plugin/qemu-playground in the
 * GearShell project for a working vnet bridge.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "net/net.h"
#include "clients.h"

typedef struct NetWsMuxState {
    bool closed;
} NetWsMuxState;

static bool wsmux_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t wsmux_receive(NetClientState *nc,
                            const uint8_t *buf,
                            size_t size)
{
    /* The actual WebSocket pump is wired up by the host page after the
     * QEMU runtime is initialised. The netdev is intentionally a no-op
     * stub so the build stays green until the JavaScript bridge is
     * integrated. */
    return size;
}

static void wsmux_link_status_changed(NetClientState *nc)
{
}

static void wsmux_cleanup(NetClientState *nc)
{
    NetWsMuxState *s = qemu_get_nic_opaque(nc);
    s->closed = true;
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
    NetClientState *nc;
    NetWsMuxState *s;

    nc = qemu_new_net_client(&net_wsmux_info, peer, name,
                             netdev->id);
    s = qemu_get_nic_opaque(nc);
    s->closed = false;
    return 0;
}
