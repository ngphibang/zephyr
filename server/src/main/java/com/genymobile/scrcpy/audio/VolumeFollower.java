package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.util.Ln;

import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.SystemClock;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.ShortBuffer;

/**
 * Apply the device's volume to captured playback.
 *
 * A policy mix routes tracks away from the device's output, so the volume keys, which act
 * on the stream's index for that output, no longer reach what the host hears. Scale the
 * samples with the device's own volume curve instead: the media volume normally, the call
 * volume while a call is in progress, so the keys behave as they do on the device.
 */
public final class VolumeFollower {

    private static final long REFRESH_MS = 200;

    private final AudioManager audioManager;
    private long nextRefresh;
    private float gain = 1f;
    private int lastStream = -1;
    private int lastIndex = -1;

    public VolumeFollower(AudioManager audioManager) {
        this.audioManager = audioManager;
    }

    private void refresh() {
        long now = SystemClock.uptimeMillis();
        if (now < nextRefresh) {
            return;
        }
        nextRefresh = now + REFRESH_MS;

        int mode = audioManager.getMode();
        boolean inCall = mode == AudioManager.MODE_IN_CALL || mode == AudioManager.MODE_IN_COMMUNICATION;
        int stream = inCall ? AudioManager.STREAM_VOICE_CALL : AudioManager.STREAM_MUSIC;
        int index = audioManager.getStreamVolume(stream);
        if (stream == lastStream && index == lastIndex) {
            return;
        }
        lastStream = stream;
        lastIndex = index;

        if (index <= 0) {
            gain = 0f;
        } else {
            float db = audioManager.getStreamVolumeDb(stream, index, AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
            gain = (float) Math.pow(10, db / 20);
        }
        Ln.d("Volume: " + (inCall ? "call" : "media") + " " + index + "/" + audioManager.getStreamMaxVolume(stream) + ", gain " + gain);
    }

    /** Scale the 16-bit samples in [0, length) of the buffer in place. */
    public void apply(ByteBuffer buffer, int length) {
        refresh();
        if (gain == 1f) {
            return;
        }
        ByteBuffer view = buffer.duplicate();
        view.position(0);
        view.limit(length);
        ShortBuffer samples = view.order(ByteOrder.LITTLE_ENDIAN).asShortBuffer();
        int count = samples.remaining();
        for (int i = 0; i < count; i++) {
            int scaled = Math.round(samples.get(i) * gain);
            if (scaled > Short.MAX_VALUE) {
                scaled = Short.MAX_VALUE;
            } else if (scaled < Short.MIN_VALUE) {
                scaled = Short.MIN_VALUE;
            }
            samples.put(i, (short) scaled);
        }
    }
}
