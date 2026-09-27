/*
 * WebSocket-multiplexed netdev JS library for qemu-wasm.
 *
 * Exposes three C entry points to net/wsmux.c:
 *   js_wsmux_open(url, bridge) -> handle (uintptr_t)
 *   js_wsmux_send(handle, ptr, len)
 *   js_wsmux_recv(handle, ptr, len, ms) -> bytes read (negative on error)
 *   js_wsmux_close(handle)
 *
 * The actual WebSocket transport lives in a helper exposed on the
 * qemu-wasm Module object as Module.wsmuxBridge (configurable via
 * the qapi 'bridge' option; defaults to 'wsmuxBridge'). The helper
 * must implement:
 *   bridge.connect(url)           -> handle
 *   bridge.send(handle, bytes)    -> void
 *   bridge.recv(handle, callback) -> disposer  (called once a frame is
 *                                    ready; callback(bytes))
 *   bridge.close(handle)          -> void
 *
 * Frames are framed as "qemu" by default (4-byte big-endian length
 * prefix followed by raw L2 payload) but the helper may negotiate a
 * different wire format.
 */
mergeInto(LibraryManager.library, {

    $wsmuxBridges: {},
    $wsmuxHelpers: {},

    $wsmuxResolveBridge__deps: ['$wsmuxBridges', '$wsmuxHelpers'],
    $wsmuxResolveBridge: (name) => {
        const key = name || 'wsmuxBridge';
        if (wsmuxHelpers[key]) return wsmuxHelpers[key];
        const helper = Module[key];
        if (!helper || typeof helper.connect !== 'function') {
            return 0;
        }
        wsmuxHelpers[key] = helper;
        return helper;
    },

    $wsmuxReadFrame__deps: ['$wsmuxBridges', '$wsmuxResolveBridge'],
    $wsmuxReadFrame: (bridgeName, handle) => {
        const bridge = wsmuxResolveBridge(bridgeName);
        if (!bridge) return Promise.resolve(new Uint8Array(0));
        return new Promise((resolve) => {
            let settled = false;
            const settle = (frame) => {
                if (settled) return;
                settled = true;
                resolve(frame || new Uint8Array(0));
            };
            const dispose = bridge.recv(handle, (frame) => {
                settle(frame);
            });
            // Wrap the disposer: if the bridge tears the handle down
            // while we are still waiting, deliver an empty frame so
            // the wasm side can exit the recv loop.
            const wrappedDispose = () => {
                try { if (dispose) dispose(); } catch (e) { /* ignore */ }
                settle(new Uint8Array(0));
            };
            const pending = wsmuxBridges[handle] || (wsmuxBridges[handle] = {});
            pending.activeDispose = wrappedDispose;
        });
    },

    js_wsmux_open__async: true,
    js_wsmux_open__deps: ['$wsmuxResolveBridge'],
    js_wsmux_open: async (urlPtr, bridgeNamePtr) => {
        const url = UTF8ToString(urlPtr);
        const name = bridgeNamePtr ? UTF8ToString(bridgeNamePtr) : 'wsmuxBridge';
        const bridge = wsmuxResolveBridge(name);
        if (!bridge) return 0;
        const handle = await bridge.connect(url);
        wsmuxBridges[handle] = { pending: [] };
        return handle;
    },

    js_wsmux_send: (handle, ptr, len) => {
        const bridge = wsmuxResolveBridge('wsmuxBridge');
        if (!bridge) return -1;
        const bytes = HEAPU8.subarray(ptr, ptr + len);
        bridge.send(handle, Array.from(bytes));
        return len;
    },

    js_wsmux_recv__async: true,
    js_wsmux_recv__deps: ['$wsmuxReadFrame', '$wsmuxResolveBridge'],
    js_wsmux_recv: async (handle, ptr, len, bridgeNamePtr) => {
        const name = bridgeNamePtr ? UTF8ToString(bridgeNamePtr) : 'wsmuxBridge';
        const frame = await wsmuxReadFrame(name, handle);
        const slice = frame.slice(0, len);
        HEAPU8.set(slice, ptr);
        return slice.length;
    },

    js_wsmux_close: (handle) => {
        const bridge = wsmuxResolveBridge('wsmuxBridge');
        if (!bridge) return;
        const pending = wsmuxBridges[handle];
        if (pending && pending.activeDispose) {
            try { pending.activeDispose(); } catch (e) { /* ignore */ }
        }
        bridge.close(handle);
        delete wsmuxBridges[handle];
    },

});