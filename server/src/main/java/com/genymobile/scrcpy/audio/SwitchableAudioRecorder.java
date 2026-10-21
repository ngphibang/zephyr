/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.AsyncProcessor;
import com.genymobile.scrcpy.device.Streamer;
import com.genymobile.scrcpy.util.Ln;

/**
 * A raw audio recorder whose capture the client chooses while it runs.
 *
 * The stream to the client stays the same; what feeds it is either the device's playback,
 * the mirror, or the device's microphone with its echo cancelling, the intercom. A switch
 * stops the current recorder, which releases its capture, and starts another one on the same
 * streamer. The client only ever sees a short gap in the stream.
 */
public final class SwitchableAudioRecorder implements AsyncProcessor {

    public static final int SOURCE_PLAYBACK = 0;
    public static final int SOURCE_MIC = 1;

    private final boolean audioDup;
    private final Streamer streamer;

    // The source the host calls "playback": whichever playback capture it was launched with
    private final AudioSource playbackSource;
    private AudioSource source;
    private AudioRawRecorder recorder;
    private TerminationListener listener;
    private boolean switching;
    private boolean stopped;

    public SwitchableAudioRecorder(AudioSource initialSource, boolean audioDup, Streamer streamer) {
        this.source = initialSource;
        this.playbackSource = initialSource.isDirect() && initialSource != AudioSource.OUTPUT ? AudioSource.OUTPUT : initialSource;
        this.audioDup = audioDup;
        this.streamer = streamer;
    }

    private AudioCapture createCapture(AudioSource audioSource) {
        if (audioSource.isDirect()) {
            return new AudioDirectCapture(audioSource);
        }
        return new AudioPlaybackCapture(audioDup);
    }

    private AudioSource toAudioSource(int code) {
        switch (code) {
            case SOURCE_PLAYBACK:
                return playbackSource;
            case SOURCE_MIC:
                return AudioSource.MIC_VOICE_COMMUNICATION;
            default:
                return null;
        }
    }

    /* Start a recorder for the current source; its end is reported upward unless we caused it */
    private synchronized void startRecorder() {
        recorder = new AudioRawRecorder(createCapture(source), streamer);
        recorder.start(fatalError -> {
            boolean ours;
            synchronized (this) {
                ours = switching;
            }
            if (!ours && listener != null) {
                listener.onTerminated(fatalError);
            }
        });
    }

    public void setSource(int code) {
        AudioSource newSource = toAudioSource(code);
        if (newSource == null) {
            Ln.w("Unknown audio source " + code);
            return;
        }

        AudioRawRecorder old;
        synchronized (this) {
            if (stopped || newSource == source) {
                return;
            }
            Ln.i("Audio source: " + newSource.name());
            source = newSource;
            old = recorder;
            switching = true;
        }

        if (old != null) {
            old.stop();
            try {
                old.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }

        synchronized (this) {
            switching = false;
            if (!stopped) {
                startRecorder();
            }
        }
    }

    @Override
    public void start(TerminationListener listener) {
        synchronized (this) {
            this.listener = listener;
        }
        startRecorder();
    }

    @Override
    public void stop() {
        AudioRawRecorder current;
        synchronized (this) {
            stopped = true;
            current = recorder;
        }
        if (current != null) {
            current.stop();
        }
    }

    @Override
    public void join() throws InterruptedException {
        AudioRawRecorder current;
        synchronized (this) {
            current = recorder;
        }
        if (current != null) {
            current.join();
        }
    }
}
