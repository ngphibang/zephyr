/*
 * Copyright 2024-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package com.genymobile.scrcpy.video;

import com.genymobile.scrcpy.AsyncProcessor;
import com.genymobile.scrcpy.Options;
import com.genymobile.scrcpy.device.Streamer;
import com.genymobile.scrcpy.model.CodecOption;
import com.genymobile.scrcpy.model.ConfigurationException;
import com.genymobile.scrcpy.model.Size;
import com.genymobile.scrcpy.util.IO;
import com.genymobile.scrcpy.util.Ln;

import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Compress each captured frame to a JPEG image and write it to the stream.
 *
 * For a receiver that decodes in software, which no MediaCodec encoder can serve. The frames
 * are read from the capture surface through an ImageReader, scaled to the size the receiver
 * displays when the codec options say so, and compressed with the platform's JPEG encoder:
 *
 *   video_codec=mjpeg video_codec_options=width=720,height=1280,quality=80
 *
 * Without a width and height the frames keep the capture size; the quality defaults to 80.
 */
public class JpegCompressor implements AsyncProcessor {

    private static final int DEFAULT_QUALITY = 80;
    private static final int RESET_POLL_MS = 5;
    // The capture only delivers a frame when the screen changes; a still screen would leave
    // the receiver with nothing new to show or draw on, so the last frame is repeated at this
    // period while nothing changes
    private static final long REPEAT_MS = 500;

    private final SurfaceCapture capture;
    private final Streamer streamer;
    private final int maxSize;
    private final float maxFps;
    private final int targetWidth;
    private final int targetHeight;
    private final int quality;

    private final CaptureControl captureControl = new CaptureControl();
    private final AtomicBoolean stopped = new AtomicBoolean();
    private Thread thread;

    private HandlerThread imageThread;
    // Held while a frame is read from the reader and while the reader is closed, so that a
    // reset never closes the reader under the listener (the ImageReader crashes natively)
    private final Object readerLock = new Object();
    private ImageReader currentReader;
    private final ByteArrayOutputStream jpeg = new ByteArrayOutputStream(256 * 1024);
    private long lastFrameMs;
    private IOException streamError;

    // The last frame sent, repeated while the screen is still; touched on the image thread only
    private byte[] lastJpeg;
    private long lastSentMs;
    private Handler imageHandler;
    private final Runnable repeater = this::repeatLastFrame;

    public JpegCompressor(SurfaceCapture capture, Streamer streamer, Options options) {
        this.capture = capture;
        this.streamer = streamer;
        this.maxSize = options.getMaxSize();
        this.maxFps = options.getMaxFps();

        int width = 0;
        int height = 0;
        int q = DEFAULT_QUALITY;
        List<CodecOption> codecOptions = options.getVideoCodecOptions();
        if (codecOptions != null) {
            for (CodecOption option : codecOptions) {
                Object value = option.getValue();
                if (!(value instanceof Integer)) {
                    Ln.w("Ignoring non-integer MJPEG option " + option.getKey());
                    continue;
                }
                switch (option.getKey()) {
                    case "width":
                        width = (Integer) value;
                        break;
                    case "height":
                        height = (Integer) value;
                        break;
                    case "quality":
                        q = (Integer) value;
                        break;
                    default:
                        Ln.w("Unknown MJPEG option " + option.getKey());
                        break;
                }
            }
        }
        this.targetWidth = width;
        this.targetHeight = height;
        this.quality = q;
    }

    private void onImageAvailable(ImageReader reader, Size captureSize) {
        synchronized (readerLock) {
            if (reader != currentReader) {
                // The reader was closed by a reset; whatever it still signals is stale
                return;
            }
            handleImage(reader, captureSize);
        }
    }

    private void handleImage(ImageReader reader, Size captureSize) {
        Image image = reader.acquireLatestImage();
        if (image == null) {
            return;
        }

        try {
            if (stopped.get() || streamError != null) {
                return;
            }

            // Drop the frames above the receiver's rate before compressing them
            long now = SystemClock.uptimeMillis();
            if (maxFps > 0 && now - lastFrameMs < 1000 / maxFps) {
                return;
            }
            lastFrameMs = now;

            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer pixels = plane.getBuffer();
            int pixelStride = plane.getPixelStride();
            int rowStride = plane.getRowStride();
            int width = captureSize.getWidth();
            int height = captureSize.getHeight();

            // The rows may be padded: read the padded width, then keep the visible part
            Bitmap bitmap = Bitmap.createBitmap(rowStride / pixelStride, height, Bitmap.Config.ARGB_8888);
            bitmap.copyPixelsFromBuffer(pixels);
            if (bitmap.getWidth() != width) {
                bitmap = Bitmap.createBitmap(bitmap, 0, 0, width, height);
            }

            if (targetWidth > 0 && targetHeight > 0 && (targetWidth != width || targetHeight != height)) {
                bitmap = Bitmap.createScaledBitmap(bitmap, targetWidth, targetHeight, true);
            }

            jpeg.reset();
            bitmap.compress(Bitmap.CompressFormat.JPEG, quality, jpeg);

            long ptsUs = image.getTimestamp() / 1000;
            lastJpeg = jpeg.toByteArray();
            lastSentMs = now;
            streamer.writePacket(ByteBuffer.wrap(lastJpeg), ptsUs, false, true);
        } catch (IOException e) {
            // The stream is gone: end the capture, the error is reported from the capture thread
            streamError = e;
            stop();
        } finally {
            image.close();
        }
    }

    /* Runs on the image thread every REPEAT_MS: send the last frame again if nothing newer went out */
    private void repeatLastFrame() {
        if (stopped.get() || streamError != null) {
            return;
        }

        long now = SystemClock.uptimeMillis();
        if (lastJpeg != null && now - lastSentMs >= REPEAT_MS) {
            try {
                streamer.writePacket(ByteBuffer.wrap(lastJpeg), now * 1000, false, true);
                lastSentMs = now;
            } catch (IOException e) {
                streamError = e;
                stop();
                return;
            }
        }
        imageHandler.postDelayed(repeater, REPEAT_MS);
    }

    private void streamCapture() throws IOException, ConfigurationException {
        // No encoder constrains the size here, so no alignment either
        capture.init(captureControl, new VideoConstraints(maxSize, 1, null));

        imageThread = new HandlerThread("JpegCompressor");
        imageThread.start();
        imageHandler = new Handler(imageThread.getLooper());

        try {
            streamer.writeVideoHeader();

            boolean alive;
            do {
                int resetReasons = captureControl.consumeReset();
                if ((resetReasons & CaptureControl.RESET_REASON_TERMINATED) != 0) {
                    break;
                }

                capture.prepare();
                Size size = capture.getSize();

                ImageReader reader = ImageReader.newInstance(size.getWidth(), size.getHeight(), PixelFormat.RGBA_8888, 2);
                synchronized (readerLock) {
                    currentReader = reader;
                }
                reader.setOnImageAvailableListener(r -> onImageAvailable(r, size), imageHandler);
                Surface surface = reader.getSurface();
                try {
                    capture.start(surface);
                    imageHandler.postDelayed(repeater, REPEAT_MS);

                    // Capture until the display changes (a reset) or the stream stops
                    while (!captureControl.isResetRequested() && !stopped.get()) {
                        SystemClock.sleep(RESET_POLL_MS);
                    }
                    alive = !stopped.get() && !capture.isClosed();
                } finally {
                    capture.stop();
                    imageHandler.removeCallbacks(repeater);
                    // Wait for a frame in flight, then make sure no later one is taken
                    synchronized (readerLock) {
                        currentReader = null;
                        reader.setOnImageAvailableListener(null, null);
                        surface.release();
                        reader.close();
                    }
                }
            } while (alive);
        } finally {
            capture.release();
            imageThread.quitSafely();
        }

        if (streamError != null) {
            throw streamError;
        }
    }

    @Override
    public void start(TerminationListener listener) {
        thread = new Thread(() -> {
            try {
                streamCapture();
            } catch (ConfigurationException e) {
                // Do not print stack trace, a user-friendly error-message has already been logged
            } catch (IOException e) {
                // Broken pipe is expected on close, because the socket is closed by the client
                if (!IO.isBrokenPipe(e)) {
                    Ln.e("Video encoding error", e);
                }
            } finally {
                Ln.d("Screen streaming stopped");
                listener.onTerminated(true);
            }
        }, "video");
        thread.start();
    }

    @Override
    public void stop() {
        if (thread != null) {
            stopped.set(true);
            captureControl.reset(CaptureControl.RESET_REASON_TERMINATED);
        }
    }

    @Override
    public void join() throws InterruptedException {
        if (thread != null) {
            thread.join();
        }
    }
}
