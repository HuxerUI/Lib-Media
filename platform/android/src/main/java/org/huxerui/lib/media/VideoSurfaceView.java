package org.huxerui.lib.media;

import android.content.Context;
import android.graphics.Color;
import android.view.TextureView;
import android.view.View;
import android.widget.FrameLayout;

import androidx.media3.common.VideoSize;
import androidx.media3.exoplayer.ExoPlayer;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformView;
import org.huxerui.PlatformPayload;

public final class VideoSurfaceView implements HuxerUIPlatformView {
    MediaPlayerModule session;
    private final TextureView texture;
    private final View shutter;
    private final FrameLayout host;
    private ExoPlayer attached;
    private boolean hasFirstFrame;
    private String fit = "contain", horizontal = "center", vertical = "center";

    public static final class Factory implements HuxerUIPlatformView.Factory {
        public Factory() {}
        @Override
        public HuxerUIPlatformView create(
                Context context, PlatformPayload properties, HuxerUIPlatformChannel.Events events) {
            return new VideoSurfaceView(context, properties);
        }
    }

    private VideoSurfaceView(Context context, PlatformPayload properties) {
        texture = new TextureView(context);
        shutter = new View(context);
        shutter.setBackgroundColor(Color.BLACK);
        host = new FrameLayout(context) {
            @Override
            protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
                layoutVideo();
                shutter.layout(0, 0, host.getWidth(), host.getHeight());
            }
        };
        host.setBackgroundColor(Color.BLACK);
        host.setClipChildren(true);
        host.addView(texture, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        // The texture must stay attached to create a SurfaceTexture. Hide it with a shutter
        // instead of making it invisible while waiting for the first frame and video size.
        host.addView(shutter, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        texture.setVisibility(View.INVISIBLE);
        update(properties);
    }

    @Override
    public View getView() {
        return host;
    }
    @Override
    public void update(PlatformPayload properties) {
        fit = properties.requireField("fit").requireString();
        horizontal = properties.requireField("horizontal").requireString();
        vertical = properties.requireField("vertical").requireString();
        refresh();
    }
    void attachPlayer(ExoPlayer player) {
        if (attached == player) {
            refresh();
            return;
        }
        hasFirstFrame = false;
        shutter.setVisibility(View.VISIBLE);
        if (attached != null)
            attached.clearVideoTextureView(texture);
        attached = player;
        if (player != null)
            player.setVideoTextureView(texture);
        refresh();
    }
    void refresh() {
        // Apply the current video size synchronously before the next layout pass; otherwise a
        // cached first frame can be shown at the full host size for one frame.
        texture.setVisibility(attached != null ? View.VISIBLE : View.INVISIBLE);
        layoutVideo();
        host.requestLayout();
    }
    void firstFrameRendered(ExoPlayer player) {
        if (attached != player)
            return;
        hasFirstFrame = true;
        refresh();
    }
    private void layoutVideo() {
        VideoSize size = attached == null ? VideoSize.UNKNOWN : attached.getVideoSize();
        if (size.width <= 0 || size.height <= 0
                || host.getWidth() <= 0 || host.getHeight() <= 0) {
            shutter.setVisibility(View.VISIBLE);
            texture.layout(0, 0, host.getWidth(), host.getHeight());
            return;
        }
        float width = size.width * size.pixelWidthHeightRatio;
        float height = size.height;
        float sx = host.getWidth() / width, sy = host.getHeight() / height;
        float scale = 1;
        if (fit.equals("contain"))
            scale = Math.min(sx, sy);
        else if (fit.equals("cover"))
            scale = Math.max(sx, sy);
        else if (fit.equals("scale-down"))
            scale = Math.min(1, Math.min(sx, sy));
        width = fit.equals("fill") ? host.getWidth() : width * scale;
        height = fit.equals("fill") ? host.getHeight() : height * scale;
        int x = Math.round((host.getWidth() - width)
                * (horizontal.equals("start")              ? 0
                                : horizontal.equals("end") ? 1
                                                           : 0.5f));
        int y = Math.round((host.getHeight() - height)
                * (vertical.equals("start")              ? 0
                                : vertical.equals("end") ? 1
                                                         : 0.5f));
        texture.layout(x, y, x + Math.round(width), y + Math.round(height));
        // Reveal the picture only after the first frame, the real video size, and the host
        // dimensions are known, so no black, stale, or unscaled frame is shown.
        shutter.setVisibility(hasFirstFrame ? View.INVISIBLE : View.VISIBLE);
    }
    @Override
    public HuxerUIPlatformChannel.Cancellation invoke(
            String method, PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        if (method.equals("bind")) {
            final MediaPlayerModule wanted = MediaPlayerModule.find(args.requireInt64());
            // Re-binding the session that already owns this surface must not detach the texture
            // or reset the presented frame.
            if (session != wanted || session == null || session.surface != this) {
                unbind();
                session = wanted;
            }
            if (session == null) {
                result.fail("media/disconnected", "Media session is disconnected", PlatformPayload.nullValue());
                return null;
            }
            session.surface = this;
            attachPlayer(session.player);
        } else if (method.equals("unbind"))
            unbind();
        else {
            result.fail("media/unknown-command", "Unknown video command", PlatformPayload.nullValue());
            return null;
        }
        result.complete(PlatformPayload.nullValue());
        return null;
    }
    private void unbind() {
        attachPlayer(null);
        if (session != null && session.surface == this)
            session.surface = null;
        session = null;
    }
    @Override
    public void dispose() {
        unbind();
    }
}
