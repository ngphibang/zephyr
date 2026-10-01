/*
 * Copyright 2024-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package com.genymobile.scrcpy.device;

import com.genymobile.scrcpy.control.ControlChannel;
import com.genymobile.scrcpy.util.IO;
import com.genymobile.scrcpy.util.Ln;
import com.genymobile.scrcpy.util.StringUtils;

import android.os.ParcelFileDescriptor;
import android.os.SystemClock;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

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

    private static Socket connect(InetSocketAddress address, boolean noDelay) throws IOException {
        Socket socket = new Socket();
        try {
            socket.setTcpNoDelay(noDelay);
            socket.connect(address, CONNECT_TIMEOUT_MS);
        } catch (IOException | RuntimeException e) {
            socket.close();
            throw e;
        }
        return socket;
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
    public static RemoteDirectConnection open(String host, int port, boolean video, boolean audio, boolean control, boolean mic,
            boolean sendDummyByte) throws IOException {
        InetSocketAddress address = new InetSocketAddress(host, port);

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

    private static void shutdown(Socket socket) throws IOException {
        if (socket != null && !socket.isClosed()) {
            socket.shutdownInput();
            socket.shutdownOutput();
        }
    }

    @Override
    public void shutdown() throws IOException {
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
