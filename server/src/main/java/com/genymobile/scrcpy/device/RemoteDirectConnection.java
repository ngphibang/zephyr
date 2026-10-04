/*
 * Copyright 2024-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package com.genymobile.scrcpy.device;

import com.genymobile.scrcpy.FakeContext;
import com.genymobile.scrcpy.control.ControlChannel;
import com.genymobile.scrcpy.util.IO;
import com.genymobile.scrcpy.util.Ln;
import com.genymobile.scrcpy.util.StringUtils;

import android.net.ConnectivityManager;
import android.net.Network;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * A connection the server opens itself, to a host that accepts the streams on one TCP port.
 *
 * The host accepts the connections in the order they are opened here: video, then audio, then
 * control, then the microphone stream the host sends back. Each stream is exposed as a file
 * descriptor, so the streaming code uses it as it does the desktop client's local sockets.
 */
public final class RemoteDirectConnection implements Connection {

    private static final int DEVICE_NAME_FIELD_LENGTH = 64;
    private static final int CONNECT_TIMEOUT_MS = 3000;
    private static final int RETRY_DELAY_MS = 1000;

    // Linux socket options the Java API does not expose; a Wi-Fi drop closes nothing by itself,
    // so a silent host is probed and unacknowledged data gives up after this long
    private static final int TCP_KEEPIDLE = 4;
    private static final int TCP_KEEPINTVL = 5;
    private static final int TCP_KEEPCNT = 6;
    private static final int TCP_USER_TIMEOUT = 18;
    private static final int KEEPALIVE_IDLE_S = 5;
    private static final int KEEPALIVE_INTERVAL_S = 5;
    private static final int KEEPALIVE_COUNT = 3;
    private static final int USER_TIMEOUT_MS = 15000;
    // After this many failed attempts the device is asked to join the host's network again
    private static final int STEER_EVERY_ATTEMPTS = 3;

    private final Socket videoSocket;
    private final Socket audioSocket;
    private final Socket controlSocket;
    private final Socket micSocket;

    private final ParcelFileDescriptor videoPfd;
    private final ParcelFileDescriptor audioPfd;
    private final ParcelFileDescriptor micPfd;
    private final ControlChannel controlChannel;

    private RemoteDirectConnection(Socket videoSocket, Socket audioSocket, Socket controlSocket, Socket micSocket) throws IOException {
        this.videoSocket = videoSocket;
        this.audioSocket = audioSocket;
        this.controlSocket = controlSocket;
        this.micSocket = micSocket;

        videoPfd = videoSocket != null ? ParcelFileDescriptor.fromSocket(videoSocket) : null;
        audioPfd = audioSocket != null ? ParcelFileDescriptor.fromSocket(audioSocket) : null;
        micPfd = micSocket != null ? ParcelFileDescriptor.fromSocket(micSocket) : null;
        controlChannel = controlSocket != null ? new ControlChannel(controlSocket.getInputStream(), controlSocket.getOutputStream()) : null;
    }

    /**
     * Connect to the host over whichever network reaches it.
     *
     * The host is an access point with no route to the internet, and Android sends a plain
     * socket over its default network, which is whichever one has internet, typically mobile
     * data. Asking a network's capabilities is refused to the shell user, so the connection is
     * simply tried bound to each network in turn, the default route last.
     */
    private static Socket connect(InetSocketAddress address, boolean noDelay) throws IOException {
        List<Network> networks = new ArrayList<>();
        try {
            ConnectivityManager cm = FakeContext.get().getSystemService(ConnectivityManager.class);
            if (cm != null) {
                networks.addAll(Arrays.asList(cm.getAllNetworks()));
            }
        } catch (RuntimeException e) {
            Ln.w("Could not list the networks", e);
        }
        networks.add(null); // the default route

        IOException last = null;
        for (Network network : networks) {
            Socket socket = new Socket();
            try {
                if (network != null) {
                    network.bindSocket(socket);
                }
                socket.setTcpNoDelay(noDelay);
                socket.connect(address, CONNECT_TIMEOUT_MS);
                detectDeadPeer(socket);
                return socket;
            } catch (IOException | RuntimeException e) {
                closeQuietly(socket);
                last = (e instanceof IOException) ? (IOException) e : new IOException(e);
            }
        }
        throw last != null ? last : new IOException("no network");
    }

    /* Fail the socket within seconds once the host stops answering */
    private static void detectDeadPeer(Socket socket) {
        try {
            socket.setKeepAlive(true);
            try (ParcelFileDescriptor pfd = ParcelFileDescriptor.fromSocket(socket)) {
                FileDescriptor fd = pfd.getFileDescriptor();
                Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, TCP_KEEPIDLE, KEEPALIVE_IDLE_S);
                Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, TCP_KEEPINTVL, KEEPALIVE_INTERVAL_S);
                Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, TCP_KEEPCNT, KEEPALIVE_COUNT);
                Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, TCP_USER_TIMEOUT, USER_TIMEOUT_MS);
            }
        } catch (IOException | ErrnoException | RuntimeException e) {
            Ln.w("Could not set the socket's dead peer detection", e);
        }
    }

    private static void closeQuietly(Socket socket) {
        if (socket != null) {
            try {
                socket.close();
            } catch (IOException e) {
                // ignore
            }
        }
    }

    /**
     * Connect to the host, retrying until it answers.
     *
     * The host may come up after the server, so a refused or timed out connection is retried
     * every second rather than reported.
     */
    /**
     * Ask Android to join the host's Wi-Fi network.
     *
     * The host's access point has no internet, so Android leaves it for any network that has
     * some, the home Wi-Fi or mobile data, whenever it can, and the host becomes unreachable.
     * The server runs as the shell user and may use the Wi-Fi shell command to steer the device
     * back, as a user would from the settings.
     */
    private static void joinWifi(String ssid, String psk) {
        try {
            String[] cmd = (psk != null && !psk.isEmpty())
                    ? new String[] {"cmd", "wifi", "connect-network", ssid, "wpa2", psk}
                    : new String[] {"cmd", "wifi", "connect-network", ssid, "open"};
            Process process = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            process.waitFor();
            Ln.i("Asked to join Wi-Fi network " + ssid);
        } catch (IOException | RuntimeException e) {
            Ln.w("Could not ask to join the Wi-Fi network", e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    public static RemoteDirectConnection open(String host, int port, boolean video, boolean audio, boolean control, boolean mic,
            boolean sendDummyByte, String wifiSsid, String wifiPsk) throws IOException {
        InetSocketAddress address = new InetSocketAddress(host, port);
        int attempts = 0;

        Ln.i("Connecting to " + address + "...");
        while (true) {
            Socket videoSocket = null;
            Socket audioSocket = null;
            Socket controlSocket = null;
            Socket micSocket = null;
            boolean dummyByte = sendDummyByte;
            try {
                if (video) {
                    videoSocket = connect(address, false);
                    if (dummyByte) {
                        // send one byte so the client may read() to detect a connection error
                        videoSocket.getOutputStream().write(0);
                        dummyByte = false;
                    }
                }
                if (audio) {
                    audioSocket = connect(address, false);
                    if (dummyByte) {
                        audioSocket.getOutputStream().write(0);
                        dummyByte = false;
                    }
                }
                if (control) {
                    // Control messages are small and must not wait behind each other
                    controlSocket = connect(address, true);
                    if (dummyByte) {
                        controlSocket.getOutputStream().write(0);
                    }
                }
                if (mic) {
                    // Read only: the host writes its microphone to it
                    micSocket = connect(address, false);
                }
                Ln.i("Connected to " + address);
                return new RemoteDirectConnection(videoSocket, audioSocket, controlSocket, micSocket);
            } catch (IOException e) {
                closeQuietly(videoSocket);
                closeQuietly(audioSocket);
                closeQuietly(controlSocket);
                closeQuietly(micSocket);
                Ln.i("Connection to " + address + " failed (" + e.getMessage() + "), retrying");
                attempts++;
                if (wifiSsid != null && attempts % STEER_EVERY_ATTEMPTS == 0) {
                    joinWifi(wifiSsid, wifiPsk);
                }
                SystemClock.sleep(RETRY_DELAY_MS);
            }
        }
    }

    @Override
    public void sendDeviceMeta(String deviceName) throws IOException {
        byte[] buffer = new byte[DEVICE_NAME_FIELD_LENGTH];

        byte[] deviceNameBytes = deviceName.getBytes(StandardCharsets.UTF_8);
        int len = StringUtils.getUtf8TruncationIndex(deviceNameBytes, DEVICE_NAME_FIELD_LENGTH - 1);
        System.arraycopy(deviceNameBytes, 0, buffer, 0, len);
        // byte[] are always 0-initialized in java, no need to set '\0' explicitly

        if (videoPfd != null) {
            IO.writeFully(videoPfd.getFileDescriptor(), buffer, 0, buffer.length);
        } else if (audioPfd != null) {
            IO.writeFully(audioPfd.getFileDescriptor(), buffer, 0, buffer.length);
        } else if (controlSocket != null) {
            controlSocket.getOutputStream().write(buffer);
        }
    }

    @Override
    public FileDescriptor getVideoFd() {
        return videoPfd != null ? videoPfd.getFileDescriptor() : null;
    }

    @Override
    public FileDescriptor getAudioFd() {
        return audioPfd != null ? audioPfd.getFileDescriptor() : null;
    }

    @Override
    public FileDescriptor getMicFd() {
        return micPfd != null ? micPfd.getFileDescriptor() : null;
    }

    @Override
    public ControlChannel getControlChannel() {
        return controlChannel;
    }

    /**
     * Unblock the threads reading or writing a socket.
     *
     * A socket the host reset or timed out is no longer connected and refuses to be shut down,
     * which is no failure here: its threads are already failing out of their reads and
     * writes, and the socket closes with the connection.
     */
    private static void shutdown(Socket socket) {
        if (socket == null || socket.isClosed()) {
            return;
        }
        try {
            socket.shutdownInput();
        } catch (IOException e) {
            // already disconnected
        }
        try {
            socket.shutdownOutput();
        } catch (IOException e) {
            // already disconnected
        }
    }

    @Override
    public void shutdown() {
        shutdown(videoSocket);
        shutdown(audioSocket);
        shutdown(controlSocket);
        shutdown(micSocket);
    }

    @Override
    public void close() throws IOException {
        if (videoPfd != null) {
            videoPfd.close();
        }
        if (audioPfd != null) {
            audioPfd.close();
        }
        if (micPfd != null) {
            micPfd.close();
        }
        closeQuietly(videoSocket);
        closeQuietly(audioSocket);
        closeQuietly(controlSocket);
        closeQuietly(micSocket);
    }
}
