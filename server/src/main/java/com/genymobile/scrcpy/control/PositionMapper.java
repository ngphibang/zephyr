package com.genymobile.scrcpy.control;

import com.genymobile.scrcpy.model.Point;
import com.genymobile.scrcpy.model.Position;
import com.genymobile.scrcpy.model.Size;
import com.genymobile.scrcpy.util.AffineMatrix;

public final class PositionMapper {

    private final Size videoSize;
    private final AffineMatrix videoToDeviceMatrix;

    public PositionMapper(Size videoSize, AffineMatrix videoToDeviceMatrix) {
        this.videoSize = videoSize;
        this.videoToDeviceMatrix = videoToDeviceMatrix;
    }

    public static PositionMapper create(Size videoSize, AffineMatrix filterTransform, Size targetSize) {
        boolean convertToPixels = !videoSize.equals(targetSize) || filterTransform != null;
        AffineMatrix transform = filterTransform;
        if (convertToPixels) {
            AffineMatrix inputTransform = AffineMatrix.ndcFromPixels(videoSize);
            AffineMatrix outputTransform = AffineMatrix.ndcToPixels(targetSize);
            transform = outputTransform.multiply(transform).multiply(inputTransform);
        }

        return new PositionMapper(videoSize, transform);
    }

    public Size getVideoSize() {
        return videoSize;
    }

    public Point map(Position position) {
        Size clientVideoSize = position.getScreenSize();
        Point point = position.getPoint();
        if (!videoSize.equals(clientVideoSize)) {
            if (clientVideoSize.getWidth() == 0 || clientVideoSize.getHeight() == 0) {
                return null;
            }
            // The client displays the video scaled to its own size (the MJPEG stream is scaled to the
            // receiver's panel), so a position relative to that size is scaled back to the video size
            int x = point.getX() * videoSize.getWidth() / clientVideoSize.getWidth();
            int y = point.getY() * videoSize.getHeight() / clientVideoSize.getHeight();
            point = new Point(x, y);
        }

        if (videoToDeviceMatrix != null) {
            point = videoToDeviceMatrix.apply(point);
        }
        return point;
    }
}
