/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.AsyncProcessor;
import com.genymobile.scrcpy.util.Ln;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;

import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Play the microphone stream a client sends.
 *
 * The stream is raw PCM, 16 kHz, 32-bit little-endian, one channel, in blocks of 10 ms; the 16
 * most significant bits of each sample are kept. What is done with it is the mode the client
 * sets through a control message: played on the device's speaker, fed to the applications
 * recording from the device's microphone, or dropped. The stream is read whatever the mode, so
 * that the client is never held back and a mode change takes effect on the next block.
 */
public final class MicPlayer implements AsyncProcessor {

    public static final int MODE_OFF = 0;
    public static final int MODE_SPEAKER = 1;
    public static final int MODE_INJECT = 2;

    private static final int SAMPLE_RATE = 16000;
    private static final int BLOCK_SAMPLES = SAMPLE_RATE / 100;

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
            AudioAttributes attributes = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build();
            // Two blocks of room above the minimum: enough to ride a late block, not enough to lag
            int bufferSize = Math.max(minSize, 4 * BLOCK_SAMPLES * 2);
            speaker = new AudioTrack(attributes, format, bufferSize, AudioTrack.MODE_STREAM, AudioManager.AUDIO_SESSION_ID_GENERATE);
            speaker.play();
        }
        return speaker;
    }

    private AudioTrack injectionTrack() {
        if (injector == null) {
            injector = AudioInjector.open(SAMPLE_RATE);
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
            // Never block on a sink: an injection track with nobody recording does not drain
            track.write(pcm16, 0, pcm16.length, AudioTrack.WRITE_NON_BLOCKING);
        }
    }

    private void run() throws IOException {
        byte[] block = new byte[BLOCK_SAMPLES * 4];
        byte[] pcm16 = new byte[BLOCK_SAMPLES * 2];
        int lastMode = mode;

        try (InputStream in = new FileInputStream(fd)) {
            while (!stopped && readFully(in, block)) {
                for (int i = 0; i < BLOCK_SAMPLES; i++) {
                    // Little-endian 32-bit sample: its two high bytes are the 16-bit sample
                    pcm16[2 * i] = block[4 * i + 2];
                    pcm16[2 * i + 1] = block[4 * i + 3];
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
