/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.AndroidVersions;
import com.genymobile.scrcpy.AsyncProcessor;
import com.genymobile.scrcpy.FakeContext;
import com.genymobile.scrcpy.util.Ln;

import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Build;
import android.os.SystemClock;

import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Play the microphone stream a client sends.
 *
 * The stream is raw PCM, 16 kHz, 32-bit little-endian, two channels, in blocks of 10 ms: the two
 * slots of the host's PDM line, of which the microphone drives one, so the channels are summed
 * into one 16-bit sample, keeping the 16 most significant bits. What is done with it is the mode the client
 * sets through a control message: played on the device's speaker, fed to the applications
 * recording from the device's microphone, or dropped. The stream is read whatever the mode, so
 * that the client is never held back and a mode change takes effect on the next block.
 */
public final class MicPlayer implements AsyncProcessor {

    public static final int MODE_OFF = 0;
    public static final int MODE_SPEAKER = 1;
    public static final int MODE_INJECT = 2;

    private static final int SAMPLE_RATE = 16000;
    private static final int CHANNELS = 2;
    private static final int BLOCK_SAMPLES = SAMPLE_RATE / 100;
    // Recording apps capture at 48 kHz and the capture path does not upsample from the
    // injection mix, so the mix runs at 48 kHz and the stream is upsampled for it
    private static final int INJECT_RATE = 48000;
    private static final int INJECT_FACTOR = INJECT_RATE / SAMPLE_RATE;
    // An injected microphone gets no automatic gain control, so the far end of a call
    // hears the board's microphone soft without a gain of its own
    private static final int INJECT_GAIN = 2;

    private final FileDescriptor fd;
    private volatile int mode = MODE_SPEAKER;
    private volatile boolean stopped;
    private Thread thread;

    private AudioTrack speaker;
    private AudioInjector injector;

    public MicPlayer(FileDescriptor fd) {
        this.fd = fd;
    }

    public void setMode(int mode) {
        if (mode < MODE_OFF || mode > MODE_INJECT) {
            Ln.w("Unknown microphone mode " + mode);
            return;
        }
        Ln.i("Microphone mode: " + modeName(mode));
        this.mode = mode;
    }

    private static String modeName(int mode) {
        switch (mode) {
            case MODE_SPEAKER:
                return "speaker";
            case MODE_INJECT:
                return "inject";
            default:
                return "off";
        }
    }

    private static boolean readFully(InputStream in, byte[] buf) throws IOException {
        int off = 0;
        while (off < buf.length) {
            int n = in.read(buf, off, buf.length - off);
            if (n < 0) {
                return false;
            }
            off += n;
        }
        return true;
    }

    private AudioTrack speaker() {
        if (speaker == null) {
            int minSize = AudioTrack.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
            AudioFormat format = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build();
            // The voice communication usage is the only output the echo canceller of a call
            // app's microphone capture references: on any other usage the phone's microphone
            // sends this voice to the far end, whose echo of it comes back to the host through
            // the playback capture. The usage is in the playback capture's mix, so the track
            // opts out of capture and the capture excludes this uid (AudioPlaybackCapture).
            // A media, navigation, accessibility or assistant usage would go to the remote
            // submix instead of the speaker while the device's own audio is captured.
            AudioAttributes.Builder attributesBuilder = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH);
            if (Build.VERSION.SDK_INT >= AndroidVersions.API_29_ANDROID_10) {
                attributesBuilder.setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_NONE);
            }
            AudioAttributes attributes = attributesBuilder.build();
            // Two blocks of room above the minimum: enough to ride a late block, not enough to lag
            int bufferSize = Math.max(minSize, 4 * BLOCK_SAMPLES * 2);
            speaker = new AudioTrack(attributes, format, bufferSize, AudioTrack.MODE_STREAM, AudioManager.AUDIO_SESSION_ID_GENERATE);
            speaker.play();
            routeToSpeaker();
        }
        return speaker;
    }

    // The phone strategy plays on the earpiece outside a call, so the track gets the
    // loudspeaker as its own preferred device. A communication device set for this uid
    // would instead change the strategy's route, and Telecom starts an incoming call from
    // the current communication device: the far end then played on the speaker instead of
    // reaching the capture mix.
    private void routeToSpeaker() {
        AudioManager audioManager = FakeContext.get().getSystemService(AudioManager.class);
        int index = audioManager.getStreamVolume(AudioManager.STREAM_VOICE_CALL);
        int max = audioManager.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL);
        boolean routed = false;
        for (AudioDeviceInfo device : audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) {
                routed = speaker.setPreferredDevice(device);
                break;
            }
        }
        Ln.i("Microphone: " + (routed ? "routed to the speaker" : "speaker route refused") + ", call volume " + index + "/" + max);
    }

    private final byte[] upsampled = new byte[BLOCK_SAMPLES * INJECT_FACTOR * 2];
    private short lastSample;

    /* Gain, then linear interpolation from the block rate to the injection rate, continuous across blocks */
    private byte[] upsample(byte[] pcm16) {
        int o = 0;
        short prev = lastSample;
        for (int i = 0; i < BLOCK_SAMPLES; i++) {
            int raw = (short) ((pcm16[2 * i] & 0xFF) | (pcm16[2 * i + 1] << 8));
            short cur = (short) Math.max(Short.MIN_VALUE, Math.min(Short.MAX_VALUE, raw * INJECT_GAIN));
            for (int k = 1; k <= INJECT_FACTOR; k++) {
                int v = prev + (cur - prev) * k / INJECT_FACTOR;
                upsampled[o++] = (byte) v;
                upsampled[o++] = (byte) (v >> 8);
            }
            prev = cur;
        }
        lastSample = prev;
        return upsampled;
    }

    private AudioTrack injectionTrack() {
        if (injector == null) {
            injector = AudioInjector.open(INJECT_RATE);
            if (injector == null) {
                return null;
            }
        }
        return injector.getTrack();
    }

    /* Hand the block to the sink of the current mode, dropping what the sink cannot take now */
    private void play(byte[] pcm16, int lastMode) {
        AudioTrack track;
        switch (mode) {
            case MODE_SPEAKER:
                track = speaker();
                break;
            case MODE_INJECT:
                track = injectionTrack();
                break;
            default:
                track = null;
                break;
        }

        if (lastMode != mode) {
            // Drop what the previous sink still holds, so the switch is heard at once
            if (lastMode == MODE_SPEAKER && speaker != null) {
                speaker.pause();
                speaker.flush();
                speaker.play();
            }
        }

        if (track != null) {
            byte[] data = (mode == MODE_INJECT) ? upsample(pcm16) : pcm16;
            // Never block on a sink: an injection track with nobody recording does not drain
            track.write(data, 0, data.length, AudioTrack.WRITE_NON_BLOCKING);
        }
    }

    private void run() throws IOException {
        byte[] block = new byte[BLOCK_SAMPLES * CHANNELS * 4];
        byte[] pcm16 = new byte[BLOCK_SAMPLES * 2];
        int lastMode = mode;
        long statBlocks = 0;
        long statPeak = 0;
        long statT0 = SystemClock.uptimeMillis();

        try (InputStream in = new FileInputStream(fd)) {
            while (!stopped && readFully(in, block)) {
                for (int i = 0; i < BLOCK_SAMPLES; i++) {
                    // Little-endian 32-bit samples: the two high bytes of each are its 16 bits
                    int sum = 0;
                    for (int c = 0; c < CHANNELS; c++) {
                        int o = (i * CHANNELS + c) * 4;
                        sum += (short) ((block[o + 2] & 0xFF) | (block[o + 3] << 8));
                    }
                    int v = Math.max(Short.MIN_VALUE, Math.min(Short.MAX_VALUE, sum));
                    pcm16[2 * i] = (byte) v;
                    pcm16[2 * i + 1] = (byte) (v >> 8);
                    statPeak = Math.max(statPeak, Math.abs(v));
                }
                statBlocks++;
                long now = SystemClock.uptimeMillis();
                if (now - statT0 >= 1000) {
                    Ln.d("Microphone: " + statBlocks + " blocks/s, peak " + statPeak + "/32767, mode " + modeName(mode));
                    statBlocks = 0;
                    statPeak = 0;
                    statT0 = now;
                }
                play(pcm16, lastMode);
                lastMode = mode;
            }
        } finally {
            if (speaker != null) {
                speaker.stop();
                speaker.release();
                speaker = null;
            }
            if (injector != null) {
                injector.close();
                injector = null;
            }
        }
    }

    @Override
    public void start(TerminationListener listener) {
        thread = new Thread(() -> {
            boolean fatalError = false;
            try {
                run();
            } catch (IOException e) {
                // Expected when the connection is shut down
            } catch (Throwable t) {
                Ln.e("Microphone playback error", t);
                fatalError = true;
            } finally {
                Ln.d("Microphone player stopped");
                listener.onTerminated(fatalError);
            }
        }, "mic");
        thread.start();
    }

    @Override
    public void stop() {
        // The read returns once the connection is shut down
        stopped = true;
    }

    @Override
    public void join() throws InterruptedException {
        if (thread != null) {
            thread.join();
        }
    }
}
