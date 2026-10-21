#!/usr/bin/env bash
#
# Copyright 2024-2026 NXP
#
# SPDX-License-Identifier: Apache-2.0
#
# Build the server without Gradle, push it to the phone and launch it against the
# board. Needs an Android SDK (ANDROID_HOME, default ~/Android/Sdk) with one platform
# and one build-tools version, and a JDK (JAVA_HOME, or the one Android Studio ships).
#
#   REMOTE_HOST=192.0.2.1 AUDIO=true MIC=true ./build_launch_server.sh
#   BUILD=false ./build_launch_server.sh                        # relaunch only
#   AUDIO_SOURCE=mic-voice-communication ./build_launch_server.sh  # intercom: the phone's
#       microphone to the board instead of its playback; with the board in speaker mode the
#       two people talk both ways
#   KEEP_SAVED_WIFI=true ./build_launch_server.sh                # keep saved board entries
#
# Wi-Fi: the server joins the board's network by itself (WIFI_SSID/WIFI_PSK) with a
# local-only network request, so the phone keeps mobile data as its default route and no
# settings change. A saved entry for the same SSID would make Android treat it as a regular
# network and drop the internet, so the script forgets such entries before launching.
#
set -e

SCRCPY_VERSION=4.1
SERVER_DIR="$(cd "$(dirname "$0")/.." && pwd)"

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_PLATFORM="${ANDROID_PLATFORM:-$(ls "$ANDROID_HOME/platforms" | sed 's/^android-//' | sort -V | tail -1)}"
export ANDROID_BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-$(ls "$ANDROID_HOME/build-tools" | sort -V | tail -1)}"

if [ -z "$JAVA_HOME" ] && ! command -v javac >/dev/null 2>&1; then
    for d in "$HOME"/android-studio*/android-studio/jbr "$HOME"/android-studio/jbr /opt/android-studio/jbr; do
        if [ -x "$d/bin/javac" ]; then
            export JAVA_HOME="$d"
            break
        fi
    done
fi
if [ -n "$JAVA_HOME" ]; then
    export PATH="$JAVA_HOME/bin:$PATH"
fi

ADB="${ADB:-$ANDROID_HOME/platform-tools/adb}"
REMOTE_HOST="${REMOTE_HOST:-192.0.2.1}"
REMOTE_PORT="${REMOTE_PORT:-5000}"
AUDIO="${AUDIO:-true}"
MIC="${MIC:-true}"
# output = the phone's playback (mirror); mic-voice-communication = its microphone with echo
# cancelling (intercom); voice-call-downlink = the other party of a cellular call, if the
# device allows it
AUDIO_SOURCE="${AUDIO_SOURCE:-playback}"
# The board's network: the server asks the phone to rejoin it whenever the board is unreachable,
# since Android leaves a network without internet by itself
WIFI_SSID="${WIFI_SSID:-zephyr-link}"
WIFI_PSK="${WIFI_PSK:-zephyrdemo}"
MAX_FPS="${MAX_FPS:-15}"
# JPEG quality of the frames: the board decodes a 720x1280 frame in about 45 ms plus 0.3 ms
# per KB, so a lighter frame is a faster one; 70 keeps text readable at a quarter less data
JPEG_QUALITY="${JPEG_QUALITY:-70}"
BUILD_DIR="${BUILD_DIR:-$SERVER_DIR/build_manual}"

# The server deletes its jar from the phone when it exits (cleanup=true), so a
# relaunch always pushes; BUILD=false skips the build when nothing changed.
if [ "${BUILD:-true}" = true ]; then
    echo "===> Building the server (platform $ANDROID_PLATFORM, build-tools $ANDROID_BUILD_TOOLS)"
    BUILD_DIR="$BUILD_DIR" "$SERVER_DIR/build_without_gradle.sh"
fi

echo "===> Stopping a running server, if any"
for pid in $("$ADB" shell "ps -A | grep app_process" | awk '{print $2}'); do
    "$ADB" shell "kill -9 $pid" || true
done
# The old server's cleanup process deletes the jar when it notices the server is gone; wait
# for it, or it deletes the one pushed below
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -z "$("$ADB" shell "ps -A | grep app_process")" ] && break
    sleep 0.5
done

echo "===> Pushing the server"
"$ADB" push "$BUILD_DIR/scrcpy-server" /data/local/tmp/scrcpy-server.jar

echo "===> Launching the server against $REMOTE_HOST:$REMOTE_PORT (audio=$AUDIO source=$AUDIO_SOURCE mic=$MIC)"
# The board shows the stream on a 720x1280 portrait panel and decodes JPEG in software:
# fixed-size MJPEG frames, the capture locked to the natural orientation, raw streams
# with no headers, raw PCM audio, and the board's microphone coming back.
# The server holds the host's network itself (a local-only request: it never becomes the
# device's default network, so mobile data keeps carrying the internet). A saved entry for
# the same SSID would make Android join it as an ordinary network and take the internet away:
# forget any such entry (KEEP_SAVED_WIFI=true keeps them).
if [ "${KEEP_SAVED_WIFI:-false}" != "true" ]; then
    for id in $("$ADB" shell cmd wifi list-networks 2>/dev/null | awk -v s="$WIFI_SSID" '$2 == s {print $1}' | sort -u); do
        "$ADB" shell cmd wifi forget-network "$id" >/dev/null 2>&1
    done
fi

"$ADB" shell "CLASSPATH=/data/local/tmp/scrcpy-server.jar nohup app_process / com.genymobile.scrcpy.Server $SCRCPY_VERSION \
    remote_host=$REMOTE_HOST remote_port=$REMOTE_PORT \
    video=true video_source=display video_codec=mjpeg video_codec_options=width=720,height=1280,quality=$JPEG_QUALITY \
    capture_orientation=@0 max_fps=$MAX_FPS \
    audio=$AUDIO audio_source=$AUDIO_SOURCE audio_codec=raw mic=$MIC \
    wifi_ssid=$WIFI_SSID wifi_psk=$WIFI_PSK \
    control=true show_touches=true cleanup=true raw_stream=true >/dev/null 2>&1 &"

"$ADB" shell "ps -A | grep app_process" || true
