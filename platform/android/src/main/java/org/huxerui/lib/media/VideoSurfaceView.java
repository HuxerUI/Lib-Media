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
    private final FrameLayout host;
    private ExoPlayer attached;
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
        host = new FrameLayout(context) {
            @Override
            protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
                layoutVideo();
            }
        };
        host.setBackgroundColor(Color.BLACK);
        host.setClipChildren(true);
        host.addView(texture, new FrameLayout.LayoutParams(
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
        host.requestLayout();
    }
    void attachPlayer(ExoPlayer player) {
        if (attached != null)
            attached.clearVideoTextureView(texture);
        attached = player;
        if (player != null)
            player.setVideoTextureView(texture);
        refresh();
    }
    void refresh() {
        // Video size may arrive only after rendering, so surface creation must not wait for it.
        texture.setVisibility(attached != null ? View.VISIBLE : View.INVISIBLE);
        host.requestLayout();
    }
    private void layoutVideo() {
        VideoSize size = attached == null ? VideoSize.UNKNOWN : attached.getVideoSize();
        if (size.width <= 0 || size.height <= 0) {
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
    }
    @Override
    public HuxerUIPlatformChannel.Cancellation invoke(
            String method, PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        if (method.equals("bind")) {
            unbind();
            session = MediaPlayerModule.find(args.requireInt64());
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
