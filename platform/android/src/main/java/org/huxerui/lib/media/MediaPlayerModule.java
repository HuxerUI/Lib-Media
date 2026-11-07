package org.huxerui.lib.media;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;

import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.MediaItem;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.common.VideoSize;
import androidx.media3.exoplayer.ExoPlayer;

import org.huxerui.HuxerUIFileReference;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.lang.ref.WeakReference;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

public final class MediaPlayerModule implements HuxerUIPlatformModule {
    private static final Map<Long, WeakReference<MediaPlayerModule>> sessions = new HashMap<>();
    private final Context context;
    private final HuxerUIPlatformChannel.Events events;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private long identity;
    private long generation;
    private long seekId;
    private boolean ready;
    private boolean disposed;
    private double volume = 1;
    private boolean muted;
    private HuxerUIFileReference reference;
    ExoPlayer player;
    VideoSurfaceView surface;

    public static final class Factory implements HuxerUIPlatformModule.Factory {
        public Factory() {}
        @Override
        public HuxerUIPlatformModule create(
                Context context, PlatformPayload options, HuxerUIPlatformChannel.Events events) {
            options.requireNull();
            return new MediaPlayerModule(context, events);
        }
    }

    private MediaPlayerModule(Context context, HuxerUIPlatformChannel.Events events) {
        this.context = context;
        this.events = events;
    }

    static MediaPlayerModule find(long identity) {
        WeakReference<MediaPlayerModule> reference = sessions.get(identity);
        return reference == null ? null : reference.get();
    }

    @Override
    public HuxerUIPlatformChannel.Cancellation invoke(
            String method, PlatformPayload args, HuxerUIPlatformChannel.Result result) {
        if (disposed) {
            result.fail("media/disposed", "HuxerUI media session is disposed", PlatformPayload.nullValue());
            return null;
        }
        long commandGeneration = args.requireField("generation").requireInt64();
        if (!method.equals("initialize") && !method.equals("load") && !method.equals("volume")
                && commandGeneration != generation) {
            result.complete(PlatformPayload.nullValue());
            return null;
        }
        try {
            switch (method) {
                case "initialize":
                    identity = args.requireField("identity").requireInt64();
                    sessions.put(identity, new WeakReference<>(this));
                    break;
                case "load":
                    load(args, commandGeneration);
                    break;
                case "clear":
                    clear();
                    break;
                case "play":
                    if (player != null)
                        player.play();
                    break;
                case "pause":
                    if (player != null) {
                        player.pause();
                        send("paused");
                    }
                    break;
                case "seek":
                    if (player != null) {
                        seekId = args.requireField("seekId").requireInt64();
                        double milliseconds = args.requireField("position").requireDouble() * 1000;
                        if (milliseconds > Long.MAX_VALUE) {
                            error("seekFailed", "Seek exceeds the platform range", "", false);
                            break;
                        }
                        player.seekTo((long) milliseconds);
                        final ExoPlayer current = player;
                        handler.post(() -> {
                            if (player == current && !disposed)
                                completeSeek();
                        });
                    }
                    break;
                case "volume":
                    volume = args.requireField("volume").requireDouble();
                    muted = args.requireField("muted").requireBoolean();
                    if (player != null)
                        player.setVolume(muted ? 0 : (float) volume);
                    break;
                case "poll":
                    if (ready)
                        send("update");
                    break;
                default:
                    throw new IllegalArgumentException("Unknown media command");
            }
            result.complete(PlatformPayload.nullValue());
        } catch (SecurityException exception) {
            error("permissionDenied", "Access to the media source was denied", "", true);
            result.complete(PlatformPayload.nullValue());
        } catch (RuntimeException exception) {
            error("output", "The platform media command failed", exception.getClass().getSimpleName(), true);
            result.complete(PlatformPayload.nullValue());
        }
        return null;
    }

    private void load(PlatformPayload args, long nextGeneration) {
        clear();
        generation = nextGeneration;
        if (!args.requireField("headers").fields().isEmpty()) {
            error("unsupported", "Custom HTTP headers are not supported by this media adapter", "", true);
            return;
        }
        Uri uri;
        if (args.requireField("kind").requireString().equals("reference")) {
            reference = args.requireField("value").requireFileReference();
            uri = reference.uri();
        } else {
            uri = Uri.parse(args.requireField("value").requireString());
        }
        final ExoPlayer current = new ExoPlayer.Builder(context).build();
        player = current;
        current.setAudioAttributes(new AudioAttributes.Builder()
                                           .setUsage(C.USAGE_MEDIA)
                                           .setContentType(C.AUDIO_CONTENT_TYPE_MOVIE)
                                           .build(),
                true);
        current.setHandleAudioBecomingNoisy(true);
        current.setVolume(muted ? 0 : (float) volume);
        current.addListener(new Player.Listener() {
            private boolean valid() {
                return !disposed && player == current;
            }
            @Override
            public void onPlaybackStateChanged(int state) {
                if (!valid())
                    return;
                if (state == Player.STATE_READY && !ready) {
                    ready = true;
                    send("ready");
                }
                if (state == Player.STATE_ENDED)
                    send("ended");
                else if (ready) {
                    completeSeek();
                    send("update");
                }
            }
            @Override
            public void onIsPlayingChanged(boolean playing) {
                if (!valid() || !ready)
                    return;
                if (playing)
                    send("playing");
                else if (!current.getPlayWhenReady() && current.getPlaybackState() != Player.STATE_ENDED)
                    send("paused");
            }
            @Override
            public void onPlaybackSuppressionReasonChanged(int reason) {
                if (valid() && reason != Player.PLAYBACK_SUPPRESSION_REASON_NONE)
                    send("interrupted");
            }
            @Override
            public void onPlayWhenReadyChanged(boolean value, int reason) {
                if (valid() && !value
                        && (reason == Player.PLAY_WHEN_READY_CHANGE_REASON_AUDIO_FOCUS_LOSS
                                || reason == Player.PLAY_WHEN_READY_CHANGE_REASON_AUDIO_BECOMING_NOISY))
                    send("interrupted");
            }
            @Override
            public void onVideoSizeChanged(VideoSize size) {
                if (!valid())
                    return;
                if (surface != null)
                    surface.refresh();
                if (ready)
                    send("update");
            }
            @Override
            public void onPlayerError(PlaybackException failure) {
                if (!valid())
                    return;
                String code = "unknown";
                switch (failure.errorCode) {
                    case PlaybackException.ERROR_CODE_IO_FILE_NOT_FOUND:
                        code = "notFound";
                        break;
                    case PlaybackException.ERROR_CODE_IO_NO_PERMISSION:
                        code = "permissionDenied";
                        break;
                    case PlaybackException.ERROR_CODE_IO_NETWORK_CONNECTION_FAILED:
                    case PlaybackException.ERROR_CODE_IO_NETWORK_CONNECTION_TIMEOUT:
                    case PlaybackException.ERROR_CODE_IO_BAD_HTTP_STATUS:
                        code = "network";
                        break;
                    case PlaybackException.ERROR_CODE_DECODING_FAILED:
                        code = "decode";
                        break;
                    case PlaybackException.ERROR_CODE_DECODING_FORMAT_UNSUPPORTED:
                    case PlaybackException.ERROR_CODE_PARSING_CONTAINER_UNSUPPORTED:
                        code = "unsupported";
                        break;
                }
                error(code, "The Android media engine could not play the source", Integer.toString(failure.errorCode),
                        true);
            }
        });
        if (surface != null)
            surface.attachPlayer(current);
        current.setMediaItem(MediaItem.fromUri(uri));
        current.prepare();
    }

    private void completeSeek() {
        if (seekId != 0 && player != null && player.getPlaybackState() == Player.STATE_READY) {
            send("seeked");
            seekId = 0;
        }
    }

    private void send(String kind) {
        if (disposed || player == null)
            return;
        Map<String, PlatformPayload> fields = new LinkedHashMap<>();
        long duration = player.getDuration();
        fields.put("kind", PlatformPayload.string(kind));
        fields.put("generation", PlatformPayload.int64(generation));
        fields.put("seekId", PlatformPayload.int64(seekId));
        fields.put("position", PlatformPayload.doubleValue(Math.max(0, player.getCurrentPosition()) / 1000.0));
        fields.put("duration",
                duration == C.TIME_UNSET || duration < 0 ? PlatformPayload.nullValue()
                                                         : PlatformPayload.doubleValue(duration / 1000.0));
        fields.put("seekable",
                ready ? PlatformPayload.booleanValue(player.isCurrentMediaItemSeekable() && duration != C.TIME_UNSET)
                      : PlatformPayload.nullValue());
        fields.put("audio",
                ready ? PlatformPayload.booleanValue(player.getCurrentTracks().isTypeSupported(C.TRACK_TYPE_AUDIO))
                      : PlatformPayload.nullValue());
        fields.put("video",
                ready ? PlatformPayload.booleanValue(player.getCurrentTracks().isTypeSupported(C.TRACK_TYPE_VIDEO))
                      : PlatformPayload.nullValue());
        VideoSize size = player.getVideoSize();
        fields.put("width",
                size.width > 0 ? PlatformPayload.doubleValue(size.width * (double) size.pixelWidthHeightRatio)
                               : PlatformPayload.nullValue());
        fields.put("height", size.height > 0 ? PlatformPayload.doubleValue(size.height) : PlatformPayload.nullValue());
        fields.put("buffering", PlatformPayload.booleanValue(player.getPlaybackState() == Player.STATE_BUFFERING));
        double position = Math.max(0, player.getCurrentPosition()) / 1000.0;
        double buffered = Math.max(0, player.getBufferedPosition()) / 1000.0;
        fields.put("buffered",
                PlatformPayload.list(buffered > position ? Collections.singletonList(PlatformPayload.list(
                                                                   Arrays.asList(PlatformPayload.doubleValue(position),
                                                                           PlatformPayload.doubleValue(buffered))))
                                                         : Collections.emptyList()));
        events.emit("media", PlatformPayload.object(fields));
    }

    private void error(String code, String message, String platformCode, boolean fatal) {
        Map<String, PlatformPayload> fields = new LinkedHashMap<>();
        fields.put("kind", PlatformPayload.string("error"));
        fields.put("generation", PlatformPayload.int64(generation));
        fields.put("seekId", PlatformPayload.int64(seekId));
        fields.put("code", PlatformPayload.string(code));
        fields.put("message", PlatformPayload.string("HuxerUI media: " + message));
        fields.put("platformCode", PlatformPayload.string(platformCode));
        fields.put("fatal", PlatformPayload.booleanValue(fatal));
        events.emit("media", PlatformPayload.object(fields));
    }

    private void clear() {
        ready = false;
        seekId = 0;
        ExoPlayer previous = player;
        player = null;
        if (surface != null)
            surface.attachPlayer(null);
        if (previous != null)
            previous.release();
        if (reference != null) {
            reference.close();
            reference = null;
        }
    }

    @Override
    public void dispose() {
        if (disposed)
            return;
        disposed = true;
        sessions.remove(identity);
        handler.removeCallbacksAndMessages(null);
        clear();
        if (surface != null) {
            surface.session = null;
            surface = null;
        }
    }
}
