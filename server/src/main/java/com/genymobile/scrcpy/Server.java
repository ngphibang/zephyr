package com.genymobile.scrcpy;

import com.genymobile.scrcpy.audio.AudioCapture;
import com.genymobile.scrcpy.audio.AudioCodec;
import com.genymobile.scrcpy.audio.AudioDirectCapture;
import com.genymobile.scrcpy.audio.AudioEncoder;
import com.genymobile.scrcpy.audio.AudioPlaybackCapture;
import com.genymobile.scrcpy.audio.AudioRawRecorder;
import com.genymobile.scrcpy.audio.AudioSource;
import com.genymobile.scrcpy.audio.MicPlayer;
import com.genymobile.scrcpy.audio.SwitchableAudioRecorder;
import com.genymobile.scrcpy.control.ControlChannel;
import com.genymobile.scrcpy.control.Controller;
import com.genymobile.scrcpy.device.Connection;
import com.genymobile.scrcpy.device.DesktopConnection;
import com.genymobile.scrcpy.device.Device;
import com.genymobile.scrcpy.device.RemoteDirectConnection;
import com.genymobile.scrcpy.device.Streamer;
import com.genymobile.scrcpy.model.ConfigurationException;
import com.genymobile.scrcpy.model.NewDisplay;
import com.genymobile.scrcpy.opengl.OpenGLRunner;
import com.genymobile.scrcpy.util.Ln;
import com.genymobile.scrcpy.util.LogUtils;
import com.genymobile.scrcpy.video.CameraCapture;
import com.genymobile.scrcpy.video.JpegCompressor;
import com.genymobile.scrcpy.video.NewDisplayCapture;
import com.genymobile.scrcpy.video.ScreenCapture;
import com.genymobile.scrcpy.video.SurfaceCapture;
import com.genymobile.scrcpy.video.SurfaceEncoder;
import com.genymobile.scrcpy.video.VideoCodec;
import com.genymobile.scrcpy.video.VideoSource;

import android.annotation.SuppressLint;
import android.os.Build;
import android.os.Looper;
import android.system.Os;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

public final class Server {

    public static final String SERVER_PATH;

    static {
        String[] classPaths = System.getProperty("java.class.path").split(File.pathSeparator);
        // By convention, scrcpy is always executed with the absolute path of scrcpy-server.jar as the first item in the classpath
        SERVER_PATH = classPaths[0];
    }

    private static class Completion {
        // The looper of the session's own thread: a processor of one session must never
        // quit the looper of the next one, which is what the main looper may be by then
        private final Looper looper = Looper.myLooper();
        private int running;
        private boolean fatalError;

        Completion(int running) {
            this.running = running;
        }

        synchronized void addCompleted(boolean fatalError) {
            --running;
            if (fatalError) {
                this.fatalError = true;
            }
            if (running == 0 || this.fatalError) {
                looper.quitSafely();
            }
        }
    }

    // Set when the remote host asks the server to exit instead of reconnecting
    private static volatile boolean quitRequested;

    private Server() {
        // not instantiable
    }

    private static void scrcpy(Options options, CleanUp cleanUp) throws IOException, ConfigurationException {
        if (Build.VERSION.SDK_INT < AndroidVersions.API_31_ANDROID_12 && options.getVideoSource() == VideoSource.CAMERA) {
            Ln.e("Camera mirroring is not supported before Android 12");
            throw new ConfigurationException("Camera mirroring is not supported");
        }

        if (Build.VERSION.SDK_INT < AndroidVersions.API_29_ANDROID_10) {
            if (options.getNewDisplay() != null) {
                Ln.e("New virtual display is not supported before Android 10");
                throw new ConfigurationException("New virtual display is not supported");
            }
            if (options.getDisplayImePolicy() != -1) {
                Ln.e("Display IME policy is not supported before Android 10");
                throw new ConfigurationException("Display IME policy is not supported");
            }
        }


        int scid = options.getScid();
        boolean tunnelForward = options.isTunnelForward();
        boolean control = options.getControl();
        boolean video = options.getVideo();
        boolean audio = options.getAudio();
        boolean mic = options.getMic();
        boolean sendDummyByte = options.getSendDummyByte();

        Workarounds.apply();

        List<AsyncProcessor> asyncProcessors = new ArrayList<>();

        Connection connection;
        if (options.getRemoteHost() != null) {
            connection = RemoteDirectConnection.open(options.getRemoteHost(), options.getRemotePort(), video, audio, control, mic,
                    sendDummyByte, options.getWifiSsid(), options.getWifiPsk());
        } else {
            connection = DesktopConnection.open(scid, tunnelForward, video, audio, control, sendDummyByte);
        }
        try {
            if (options.getSendDeviceMeta()) {
                connection.sendDeviceMeta(Device.getDeviceName());
            }

            Controller controller = null;

            if (control) {
                ControlChannel controlChannel = connection.getControlChannel();
                controller = new Controller(controlChannel, cleanUp, options);
                if (options.getRemoteHost() != null) {
                    // End this session and the loop around it
                    controller.setQuitListener(() -> {
                        quitRequested = true;
                        Looper.getMainLooper().quitSafely();
                    });
                }
                asyncProcessors.add(controller);
            }

            if (audio) {
                AudioCodec audioCodec = options.getAudioCodec();
                AudioSource audioSource = options.getAudioSource();
                AudioCapture audioCapture;
                if (audioSource.isDirect()) {
                    audioCapture = new AudioDirectCapture(audioSource);
                } else {
                    audioCapture = new AudioPlaybackCapture(options.getAudioDup());
                }

                Streamer audioStreamer = new Streamer(connection.getAudioFd(), audioCodec, options.getSendStreamMeta(), options.getSendFrameMeta());
                AsyncProcessor audioRecorder;
                if (audioCodec == AudioCodec.RAW && options.getRemoteHost() != null) {
                    // The remote host may ask for the playback or the microphone at any time
                    SwitchableAudioRecorder switchable = new SwitchableAudioRecorder(audioSource, options.getAudioDup(), audioStreamer);
                    if (controller != null) {
                        controller.setAudioRecorder(switchable);
                    }
                    audioRecorder = switchable;
                } else if (audioCodec == AudioCodec.RAW) {
                    audioRecorder = new AudioRawRecorder(audioCapture, audioStreamer);
                } else {
                    audioRecorder = new AudioEncoder(audioCapture, audioStreamer, options);
                }
                asyncProcessors.add(audioRecorder);
            }

            if (video) {
                Streamer videoStreamer = new Streamer(connection.getVideoFd(), options.getVideoCodec(), options.getSendStreamMeta(),
                        options.getSendFrameMeta());
                SurfaceCapture surfaceCapture;
                if (options.getVideoSource() == VideoSource.DISPLAY) {
                    NewDisplay newDisplay = options.getNewDisplay();
                    if (newDisplay != null) {
                        surfaceCapture = new NewDisplayCapture(controller, options);
                    } else {
                        assert options.getDisplayId() != Device.DISPLAY_ID_NONE;
                        surfaceCapture = new ScreenCapture(controller, options);
                    }
                } else {
                    surfaceCapture = new CameraCapture(options);
                }
                AsyncProcessor videoEncoder;
                if (options.getVideoCodec() == VideoCodec.MJPEG) {
                    videoEncoder = new JpegCompressor(surfaceCapture, videoStreamer, options);
                } else {
                    videoEncoder = new SurfaceEncoder(surfaceCapture, videoStreamer, options);
                }
                asyncProcessors.add(videoEncoder);

                if (controller != null) {
                    controller.setSurfaceCapture(surfaceCapture);
                }
            }

            if (mic) {
                FileDescriptor micFd = connection.getMicFd();
                if (micFd == null) {
                    Ln.w("No microphone stream on this connection");
                } else {
                    MicPlayer micPlayer = new MicPlayer(micFd);
                    asyncProcessors.add(micPlayer);
                    if (controller != null) {
                        controller.setMicPlayer(micPlayer);
                    }
                }
            }

            Completion completion = new Completion(asyncProcessors.size());
            for (AsyncProcessor asyncProcessor : asyncProcessors) {
                asyncProcessor.start((fatalError) -> {
                    completion.addCompleted(fatalError);
                });
            }

            Looper.loop(); // interrupted by the Completion implementation
        } finally {
            for (AsyncProcessor asyncProcessor : asyncProcessors) {
                asyncProcessor.stop();
            }

            connection.shutdown();

            try {
                for (AsyncProcessor asyncProcessor : asyncProcessors) {
                    asyncProcessor.join();
                }

                OpenGLRunner.shutdown();
            } catch (InterruptedException e) {
                // ignore
            }

            connection.close();
        }

    }

    /**
     * Serve a remote host for as long as the server lives.
     *
     * The host is a fixed station and the device comes and goes: whenever a session ends, the
     * server connects again rather than exit, so no relaunch is needed after the host restarts
     * or the device left its network. The processors' termination flag cannot tell a lost host
     * from a failure, since the video and control streams raise it on any end, so only the
     * host's quit message and a configuration error stop the server. Each session runs on a
     * thread of its own with its own main looper, since a looper cannot be reused once quit.
     */
    private static void serveRemote(Options options, CleanUp cleanUp) throws ConfigurationException, InterruptedException {
        while (true) {
            final Exception[] failure = {null};

            Thread session = new Thread(() -> {
                prepareMainLooper();
                try {
                    scrcpy(options, cleanUp);
                } catch (ConfigurationException | IOException e) {
                    failure[0] = e;
                }
            }, "session");
            session.start();
            session.join();

            if (failure[0] instanceof ConfigurationException) {
                throw (ConfigurationException) failure[0];
            }
            if (quitRequested) {
                Ln.i("Exiting as the host asked");
                return;
            }
            if (failure[0] != null) {
                Ln.w("Session failed", failure[0]);
            }

            Ln.i("Host gone, reconnecting");
            Thread.sleep(1000);
        }
    }

    private static void prepareMainLooper() {
        // Like Looper.prepareMainLooper(), but with quitAllowed set to true
        Looper.prepare();
        synchronized (Looper.class) {
            try {
                @SuppressLint("DiscouragedPrivateApi")
                Field field = Looper.class.getDeclaredField("sMainLooper");
                field.setAccessible(true);
                field.set(null, Looper.myLooper());
            } catch (ReflectiveOperationException e) {
                throw new AssertionError(e);
            }
        }
    }

    public static void main(String... args) {
        int status = 0;
        try {
            internalMain(args);
        } catch (Throwable t) {
            Ln.e(t.getMessage(), t);
            status = 1;
        } finally {
            // By default, the Java process exits when all non-daemon threads are terminated.
            // The Android SDK might start some non-daemon threads internally, preventing the scrcpy server to exit.
            // So force the process to exit explicitly.
            System.exit(status);
        }
    }

    private static void internalMain(String... args) throws Exception {
        Thread.UncaughtExceptionHandler defaultHandler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            Ln.e("Exception on thread " + t, e);
            if (defaultHandler != null) {
                defaultHandler.uncaughtException(t, e);
            }
        });

        dropRootPrivileges();

        Options options = Options.parse(args);
        boolean remote = options.getRemoteHost() != null;
        if (!remote) {
            prepareMainLooper();
        }

        Ln.disableSystemStreams();
        Ln.initLogLevel(options.getLogLevel());

        Ln.i("Device: [" + Build.MANUFACTURER + "] " + Build.BRAND + " " + Build.MODEL + " (Android " + Build.VERSION.RELEASE + ")");

        if (options.getList()) {
            if (options.getCleanup()) {
                CleanUp.unlinkSelf();
            }

            if (options.getListEncoders()) {
                Ln.i(LogUtils.buildVideoEncoderListMessage());
                Ln.i(LogUtils.buildAudioEncoderListMessage());
            }
            if (options.getListDisplays()) {
                Ln.i(LogUtils.buildDisplayListMessage());
            }
            if (options.getListCameras() || options.getListCameraSizes()) {
                Workarounds.apply();
                Ln.i(LogUtils.buildCameraListMessage(options.getListCameraSizes()));
            }
            if (options.getListApps()) {
                Workarounds.apply();
                Ln.i("Processing Android apps... (this may take some time)");
                Ln.i(LogUtils.buildAppListMessage());
            }
            // Just print the requested data, do not mirror
            return;
        }

        CleanUp cleanUp = null;
        if (options.getCleanup()) {
            cleanUp = CleanUp.start(options);
        }

        try {
            if (remote) {
                serveRemote(options, cleanUp);
            } else {
                scrcpy(options, cleanUp);
            }
        } catch (ConfigurationException e) {
            // Do not print stack trace, a user-friendly error-message has already been logged
        } finally {
            if (cleanUp != null) {
                cleanUp.interrupt();
                try {
                    cleanUp.join();
                } catch (InterruptedException e) {
                    // ignore
                }
            }
        }
    }

    @SuppressWarnings("deprecation")
    private static void dropRootPrivileges() {
        try {
            if (Os.getuid() == 0) {
                // Copy-paste does not work with root user
                // <https://github.com/Genymobile/scrcpy/issues/6224>
                Os.setuid(2000);
            }
        } catch (Exception e) {
            Ln.w("Cannot set UID", e);
        }
    }
}
