/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.Workarounds;
import com.genymobile.scrcpy.util.Ln;

import android.annotation.SuppressLint;
import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaRecorder;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

/**
 * Feed PCM to the applications recording from the device's microphone.
 *
 * An audio policy registered by the server, which runs as shell and holds MODIFY_AUDIO_ROUTING,
 * carries a loopback mix matching the microphone capture presets; what is written to the
 * track the policy creates for that mix is what those applications record, instead of the
 * real microphone. The policy classes are not public, so they are reached by reflection. The
 * policy registration follows scrcpy pull request 6439 "Forward client audio" by Cabbache
 * (Apache-2.0, https://github.com/Genymobile/scrcpy/pull/6439), which credits a comment in
 * scrcpy issue 3880. Available from Android 10; the mix only takes effect while an
 * application records.
 */
public final class AudioInjector {

    private static final int RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET = 1 << 1;
    private static final int ROUTE_FLAG_LOOP_BACK = 1 << 1;
    private static final int MIX_ROLE_INJECTOR = 1;

    private final AudioManager audioManager;
    private final Object audioPolicy;
    private final AudioTrack track;

    private AudioInjector(AudioManager audioManager, Object audioPolicy, AudioTrack track) {
        this.audioManager = audioManager;
        this.audioPolicy = audioPolicy;
        this.track = track;
    }

    public AudioTrack getTrack() {
        return track;
    }

    private static AudioAttributes capturePreset(int preset) throws ReflectiveOperationException {
        AudioAttributes.Builder builder = new AudioAttributes.Builder();
        Method setCapturePreset = builder.getClass().getDeclaredMethod("setCapturePreset", int.class);
        setCapturePreset.invoke(builder, preset);
        return builder.build();
    }

    /** Register the policy and start its track, or return null when the device refuses. */
    @SuppressLint("PrivateApi")
    public static AudioInjector open(int sampleRate) {
        try {
            Context context = Workarounds.getSystemContext();
            if (context == null) {
                Ln.e("Microphone injection: no system context");
                return null;
            }

            Class<?> ruleBuilderClass = Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
            Object ruleBuilder = ruleBuilderClass.getDeclaredConstructor().newInstance();
            try {
                // Android 13 and later name the role; earlier versions infer it from the rules
                Method setTargetMixRole = ruleBuilderClass.getDeclaredMethod("setTargetMixRole", int.class);
                setTargetMixRole.invoke(ruleBuilder, MIX_ROLE_INJECTOR);
            } catch (ReflectiveOperationException ignored) {
                // not available
            }
            Method addMixRule = ruleBuilderClass.getDeclaredMethod("addMixRule", int.class, Object.class);
            int[] presets = {
                    MediaRecorder.AudioSource.DEFAULT,
                    MediaRecorder.AudioSource.MIC,
                    MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                    MediaRecorder.AudioSource.VOICE_RECOGNITION,
                    MediaRecorder.AudioSource.UNPROCESSED,
            };
            for (int preset : presets) {
                addMixRule.invoke(ruleBuilder, RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET, capturePreset(preset));
            }
            Object rule = ruleBuilderClass.getDeclaredMethod("build").invoke(ruleBuilder);

            AudioFormat format = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(sampleRate)
                    .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                    .build();

            Class<?> mixBuilderClass = Class.forName("android.media.audiopolicy.AudioMix$Builder");
            Constructor<?> mixBuilderCtor = mixBuilderClass.getDeclaredConstructor(rule.getClass());
            Object mixBuilder = mixBuilderCtor.newInstance(rule);
            mixBuilderClass.getDeclaredMethod("setFormat", AudioFormat.class).invoke(mixBuilder, format);
            mixBuilderClass.getDeclaredMethod("setRouteFlags", int.class).invoke(mixBuilder, ROUTE_FLAG_LOOP_BACK);
            Object mix = mixBuilderClass.getDeclaredMethod("build").invoke(mixBuilder);

            Class<?> policyBuilderClass = Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
            Constructor<?> policyBuilderCtor = policyBuilderClass.getDeclaredConstructor(Context.class);
            Object policyBuilder = policyBuilderCtor.newInstance(context);
            policyBuilderClass.getDeclaredMethod("addMix", mix.getClass()).invoke(policyBuilder, mix);
            Object policy = policyBuilderClass.getDeclaredMethod("build").invoke(policyBuilder);

            AudioManager audioManager = context.getSystemService(AudioManager.class);
            Method register = audioManager.getClass().getDeclaredMethod("registerAudioPolicy", policy.getClass());
            int result = (Integer) register.invoke(audioManager, policy);
            if (result != 0) {
                Ln.e("Microphone injection: registerAudioPolicy failed (" + result + ")");
                return null;
            }

            Method createTrack = policy.getClass().getDeclaredMethod("createAudioTrackSource", mix.getClass());
            AudioTrack track = (AudioTrack) createTrack.invoke(policy, mix);
            if (track == null) {
                unregister(audioManager, policy);
                Ln.e("Microphone injection: no injection track");
                return null;
            }
            track.play();

            Ln.i("Microphone injection ready");
            return new AudioInjector(audioManager, policy, track);
        } catch (ReflectiveOperationException | RuntimeException e) {
            Ln.e("Microphone injection is not available on this device", e);
            return null;
        }
    }

    private static void unregister(AudioManager audioManager, Object policy) {
        try {
            Method unregister = audioManager.getClass().getDeclaredMethod("unregisterAudioPolicy", policy.getClass());
            unregister.invoke(audioManager, policy);
        } catch (ReflectiveOperationException | RuntimeException e) {
            Ln.w("Microphone injection: unregisterAudioPolicy failed", e);
        }
    }

    public void close() {
        track.stop();
        track.release();
        unregister(audioManager, audioPolicy);
    }
}
