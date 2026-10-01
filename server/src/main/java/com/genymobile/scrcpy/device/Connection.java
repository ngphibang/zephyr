/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package com.genymobile.scrcpy.device;

import com.genymobile.scrcpy.control.ControlChannel;

import java.io.Closeable;
import java.io.FileDescriptor;
import java.io.IOException;

/**
 * The streams the server serves a client over: a video stream, an audio stream, a control channel and,
 * for a client that has one, a microphone stream coming back from the client.
 *
 * The desktop client reaches the server over local sockets through adb, a remote host over TCP; the
 * streaming code does not need to know which.
 */
public interface Connection extends Closeable {

    FileDescriptor getVideoFd();

    FileDescriptor getAudioFd();

    /** The microphone stream the client sends, or null when the client sends none. */
    FileDescriptor getMicFd();

    ControlChannel getControlChannel();

    void sendDeviceMeta(String deviceName) throws IOException;

    void shutdown() throws IOException;
}
