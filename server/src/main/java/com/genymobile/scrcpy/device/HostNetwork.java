package com.genymobile.scrcpy.device;

import com.genymobile.scrcpy.AndroidVersions;
import com.genymobile.scrcpy.util.Ln;
import com.genymobile.scrcpy.wrappers.ServiceManager;

import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;

/**
 * Hold a connection to the host's Wi-Fi network for as long as the server runs.
 *
 * The host is an access point with no route to the internet. Joined as an ordinary network,
 * Android either marks it "no internet" and leaves it for a network that has some, or, once
 * the user accepted it, makes it the device's default network and the device loses its
 * internet. A network request with a Wi-Fi specifier is the mechanism Android offers for a
 * device's own access point: the connection is local-only, it never becomes the default
 * network, and the device keeps using mobile data for everything else. The shell user holds
 * NETWORK_SETTINGS, which spares the approval dialog an ordinary application would get.
 *
 * Android serves such a request once: when the connection is lost, or could not be made in
 * time, the request is dropped and nothing reconnects. The request is therefore made again
 * after every loss, a few seconds later, until released.
 */
public final class HostNetwork {

    // A host that is off would otherwise be looked for every few seconds for hours, each
    // attempt costing the device a scan: back off up to a minute, and start over on a loss
    private static final long RETRY_DELAY_MS = 3000;
    private static final long RETRY_DELAY_MAX_MS = 60_000;
    private long retryDelayMs = RETRY_DELAY_MS;

    private final ConnectivityManager cm;
    private final NetworkRequest request;
    private final String ssid;
    private final HandlerThread thread;
    private final Handler handler;
    private ConnectivityManager.NetworkCallback callback;
    private volatile Network network;
    private boolean released;

    private HostNetwork(ConnectivityManager cm, NetworkRequest request, String ssid) {
        this.cm = cm;
        this.request = request;
        this.ssid = ssid;
        this.thread = new HandlerThread("host-network");
        this.thread.start();
        this.handler = new Handler(thread.getLooper());
    }

    /** Ask for the network; null when this Android cannot (before Android 10) or refuses. */
    public static HostNetwork request(String ssid, String psk) {
        if (ssid == null || Build.VERSION.SDK_INT < AndroidVersions.API_29_ANDROID_10) {
            return null;
        }
        try {
            ConnectivityManager cm = ServiceManager.getConnectivityManager();
            WifiNetworkSpecifier.Builder specifier = new WifiNetworkSpecifier.Builder().setSsid(ssid);
            if (psk != null && !psk.isEmpty()) {
                specifier.setWpa2Passphrase(psk);
            }
            NetworkRequest request = new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                    .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                    .setNetworkSpecifier(specifier.build())
                    .build();
            HostNetwork host = new HostNetwork(cm, request, ssid);
            host.handler.post(host::ask);
            return host;
        } catch (RuntimeException e) {
            Ln.w("Could not request the host's Wi-Fi network", e);
            return null;
        }
    }

    /* Runs on the handler thread */
    private void ask() {
        if (released) {
            return;
        }
        callback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network n) {
                network = n;
                retryDelayMs = RETRY_DELAY_MS;
                Ln.i("Host network " + ssid + " available");
            }

            @Override
            public void onLost(Network n) {
                network = null;
                Ln.i("Host network " + ssid + " lost, asking again");
                handler.post(() -> askAgain(this));
            }

            @Override
            public void onUnavailable() {
                Ln.i("Host network " + ssid + " not found, asking again in " + (retryDelayMs / 1000) + " s");
                handler.post(() -> askAgain(this));
            }
        };
        try {
            cm.requestNetwork(request, callback, handler);
            Ln.i("Asked Android to keep the device on Wi-Fi network " + ssid);
        } catch (RuntimeException e) {
            Ln.w("Could not request the host's Wi-Fi network", e);
            handler.postDelayed(this::ask, RETRY_DELAY_MS);
        }
    }

    /* Runs on the handler thread: drop the served request and make a new one later */
    private void askAgain(ConnectivityManager.NetworkCallback old) {
        if (released || old != callback) {
            return;
        }
        try {
            cm.unregisterNetworkCallback(old);
        } catch (RuntimeException e) {
            // already gone
        }
        callback = null;
        handler.postDelayed(this::ask, retryDelayMs);
        retryDelayMs = Math.min(retryDelayMs * 2, RETRY_DELAY_MAX_MS);
    }

    /** The network, or null while Android has not connected it. */
    public Network get() {
        return network;
    }

    public void release() {
        handler.post(() -> {
            released = true;
            if (callback != null) {
                try {
                    cm.unregisterNetworkCallback(callback);
                } catch (RuntimeException e) {
                    // already gone
                }
                callback = null;
            }
            thread.quitSafely();
        });
    }
}
