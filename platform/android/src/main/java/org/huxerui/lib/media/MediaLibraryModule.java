package org.huxerui.lib.media;

import android.app.Activity;
import android.content.ClipData;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.LabeledIntent;
import android.content.pm.PackageManager;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ext.SdkExtensions;
import android.provider.MediaStore;

import org.huxerui.HuxerUIActivityResultLauncher;
import org.huxerui.HuxerUIFileReference;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.HuxerUIView;
import org.huxerui.PlatformPayload;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Future;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** Platform selection and publication; no storage-read permission or document-picker fallback is requested. */
public final class MediaLibraryModule implements HuxerUIPlatformModule {
    public static final class Factory implements HuxerUIPlatformModule.WindowFactory {
        @Override public HuxerUIPlatformModule create(HuxerUIView view, PlatformPayload options,
                HuxerUIPlatformChannel.Events events) {
            return new MediaLibraryModule(view);
        }
    }

    private static final ThreadPoolExecutor workers = new ThreadPoolExecutor(2, 2, 30, TimeUnit.SECONDS,
            new ArrayBlockingQueue<>(32));
    static { workers.allowCoreThreadTimeOut(true); }
    private final Context context;
    private final HuxerUIActivityResultLauncher launcher;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Set<Request> pending = new HashSet<>();
    private boolean closed;

    private MediaLibraryModule(HuxerUIView view) {
        context = view.getContext().getApplicationContext();
        launcher = view.getActivityResultLauncher();
    }

    private boolean canResolve(Intent intent) {
        return context.getPackageManager().resolveActivity(intent, PackageManager.MATCH_DEFAULT_ONLY) != null;
    }

    private boolean photoPickerAvailable() {
        boolean platform = Build.VERSION.SDK_INT >= 33 ||
                (Build.VERSION.SDK_INT >= 30 && SdkExtensions.getExtensionVersion(30) >= 2);
        return platform && canResolve(new Intent(MediaStore.ACTION_PICK_IMAGES));
    }

    private Intent galleryIntent(boolean video) {
        return new Intent(Intent.ACTION_PICK).setDataAndType(
                video ? MediaStore.Video.Media.EXTERNAL_CONTENT_URI : MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                video ? "video/*" : "image/*");
    }

    private int maxSelectionCount() {
        if (photoPickerAvailable()) return MediaStore.getPickImagesMaxLimit();
        return canResolve(galleryIntent(false)) || canResolve(galleryIntent(true)) ? 1 : 0;
    }

    @Override public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload arguments,
            HuxerUIPlatformChannel.Result result) {
        if (closed) {
            result.fail("media/unavailable", "Media library session is closed", PlatformPayload.nullValue());
            return null;
        }
        if (method.equals("capabilities")) {
            Map<String, PlatformPayload> fields = new HashMap<>();
            fields.put("maxSelectionCount", PlatformPayload.int64(maxSelectionCount()));
            fields.put("canSave", PlatformPayload.booleanValue(Build.VERSION.SDK_INT >= 29));
            result.complete(PlatformPayload.object(fields));
            return null;
        }
        Request request = new Request(result);
        pending.add(request);
        try {
            if (method.equals("pick")) {
                int filter = Math.toIntExact(arguments.requireField("filter").requireInt64());
                long limit = arguments.requireField("limit").requireInt64();
                if (limit > Integer.MAX_VALUE) request.fail("media/unsupported", "Unsupported selection limit");
                else request.pick(filter, (int) limit);
            } else if (method.equals("save")) {
                String path = arguments.requireField("path").requireString();
                boolean video = arguments.requireField("video").requireBoolean();
                if (Build.VERSION.SDK_INT < 29) request.fail("media/unsupported", "Saving requires Android 10 or later");
                else request.work(() -> request.save(new File(path), video));
            } else request.fail("media/unsupported", "Unknown media library operation");
        } catch (RuntimeException error) {
            request.fail("media/unknown", error.toString());
        }
        return request;
    }

    @Override public void dispose() {
        closed = true;
        for (Request request : new ArrayList<>(pending)) request.cancel();
        pending.clear();
    }

    private interface Work { void run() throws Exception; }

    private final class Request implements HuxerUIPlatformChannel.Cancellation {
        private final HuxerUIPlatformChannel.Result result;
        private final AtomicBoolean canceled = new AtomicBoolean();
        private final AtomicBoolean finished = new AtomicBoolean();
        private HuxerUIPlatformChannel.Cancellation presentation;
        private Future<?> future;

        Request(HuxerUIPlatformChannel.Result result) { this.result = result; }

        @Override public void cancel() {
            if (finished.get() || !canceled.compareAndSet(false, true)) return;
            if (presentation != null) presentation.cancel();
            if (future != null) future.cancel(true);
            pending.remove(this);
            result.close();
        }

        void complete(PlatformPayload value) {
            if (finished.compareAndSet(false, true)) {
                if (!canceled.get()) result.complete(value);
                main.post(() -> pending.remove(this));
            }
        }

        void fail(String code, String message) {
            if (finished.compareAndSet(false, true)) {
                if (!canceled.get()) result.fail(code, message, PlatformPayload.nullValue());
                main.post(() -> pending.remove(this));
            }
        }

        void work(Work work) {
            try {
                future = workers.submit(() -> {
                    try { if (!canceled.get()) work.run(); }
                    catch (SecurityException error) { fail("media/permission", error.toString()); }
                    catch (Exception error) { fail("media/io", error.toString()); }
                });
            } catch (RejectedExecutionException error) {
                fail("media/unavailable", "Media library worker queue is full");
            }
        }

        void pick(int filter, int limit) {
            boolean photoPicker = photoPickerAvailable();
            int maximum = photoPicker ? MediaStore.getPickImagesMaxLimit() : 1;
            if (limit < 1 || limit > maximum) {
                fail("media/unsupported", "The requested selection limit is unsupported");
                return;
            }
            Intent intent;
            if (photoPicker) {
                intent = new Intent(MediaStore.ACTION_PICK_IMAGES);
                if (filter == 0) intent.setType("image/*");
                else if (filter == 1) intent.setType("video/*");
                if (limit > 1) intent.putExtra(MediaStore.EXTRA_PICK_IMAGES_MAX, limit);
            } else {
                Intent images = galleryIntent(false);
                Intent videos = galleryIntent(true);
                if ((filter != 1 && !canResolve(images)) || (filter != 0 && !canResolve(videos))) {
                    fail("media/unsupported", "The platform gallery cannot select the requested media types");
                    return;
                }
                if (filter == 0) intent = images;
                else if (filter == 1) intent = videos;
                else {
                    // ACTION_PICK has no portable mixed-media filter or bounded multiple-selection contract.
                    intent = Intent.createChooser(new LabeledIntent(images, context.getPackageName(), "Images", 0),
                            "Choose an image or video");
                    intent.putExtra(Intent.EXTRA_INITIAL_INTENTS, new Intent[]{
                            new LabeledIntent(videos, context.getPackageName(), "Videos", 0)});
                }
            }
            presentation = launcher.launch(intent, new HuxerUIActivityResultLauncher.Callback() {
                @Override public void onResult(int code, Intent data) {
                    presentation = null;
                    if (canceled.get()) return;
                    if (code == Activity.RESULT_CANCELED) { complete(PlatformPayload.nullValue()); return; }
                    if (code != Activity.RESULT_OK || data == null) {
                        fail("media/io", "Media picker returned no readable result");
                        return;
                    }
                    LinkedHashSet<Uri> uris = new LinkedHashSet<>();
                    ClipData clips = data.getClipData();
                    if (clips != null) {
                        for (int i = 0; i < clips.getItemCount(); ++i) uris.add(clips.getItemAt(i).getUri());
                    } else if (data.getData() != null) uris.add(data.getData());
                    if (uris.isEmpty() || uris.size() > limit || uris.contains(null)) {
                        fail("media/io", "Media picker returned an invalid selection count");
                        return;
                    }
                    work(() -> loadReferences(uris, filter));
                }
                @Override public void onFailure(RuntimeException error) {
                    presentation = null;
                    fail(error instanceof SecurityException ? "media/permission" :
                            error instanceof ActivityNotFoundException ? "media/unsupported" : "media/unavailable", error.toString());
                }
            });
        }

        void loadReferences(Set<Uri> uris, int filter) throws IOException {
            ArrayList<HuxerUIFileReference> references = new ArrayList<>();
            try {
                ArrayList<PlatformPayload> values = new ArrayList<>();
                for (Uri uri : uris) {
                    if (canceled.get()) return;
                    String type = context.getContentResolver().getType(uri);
                    if (type == null || !(type.startsWith("image/") || type.startsWith("video/")) ||
                            (filter == 0 && !type.startsWith("image/")) || (filter == 1 && !type.startsWith("video/")))
                        throw new IOException("Media picker returned an unexpected media type");
                    HuxerUIFileReference reference = HuxerUIFileReference.fromUri(context, uri);
                    references.add(reference);
                    values.add(PlatformPayload.fileReference(reference));
                }
                complete(PlatformPayload.list(values));
            } finally {
                for (HuxerUIFileReference reference : references) reference.close();
            }
        }

        void save(File source, boolean video) throws Exception {
            if (!source.isFile() || source.length() == 0) {
                fail("media/invalid-source", "The source must be a nonempty readable file");
                return;
            }
            String mime;
            if (video) {
                MediaMetadataRetriever metadata = new MediaMetadataRetriever();
                try {
                    metadata.setDataSource(source.getAbsolutePath());
                    if (!"yes".equals(metadata.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_VIDEO))) {
                        fail("media/invalid-source", "The source contains no supported video track");
                        return;
                    }
                    mime = metadata.extractMetadata(MediaMetadataRetriever.METADATA_KEY_MIMETYPE);
                } catch (IllegalArgumentException error) {
                    fail("media/invalid-source", "The source is not a supported video");
                    return;
                } finally { metadata.release(); }
            } else {
                BitmapFactory.Options bounds = new BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                BitmapFactory.decodeFile(source.getAbsolutePath(), bounds);
                mime = bounds.outMimeType;
                if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
                    fail("media/invalid-source", "The source is not a supported image");
                    return;
                }
            }
            if (mime == null || !mime.startsWith(video ? "video/" : "image/")) {
                fail("media/unsupported", "The source media format cannot be published");
                return;
            }
            if (canceled.get()) return;
            ContentResolver resolver = context.getContentResolver();
            ContentValues values = new ContentValues();
            values.put(MediaStore.MediaColumns.DISPLAY_NAME, source.getName());
            values.put(MediaStore.MediaColumns.MIME_TYPE, mime);
            values.put(MediaStore.MediaColumns.RELATIVE_PATH,
                    video ? Environment.DIRECTORY_MOVIES : Environment.DIRECTORY_PICTURES);
            values.put(MediaStore.MediaColumns.IS_PENDING, 1);
            Uri collection = video ? MediaStore.Video.Media.EXTERNAL_CONTENT_URI : MediaStore.Images.Media.EXTERNAL_CONTENT_URI;
            Uri destination = resolver.insert(collection, values);
            if (destination == null) throw new IOException("MediaStore could not create a pending item");
            boolean published = false;
            try {
                try (FileInputStream input = new FileInputStream(source);
                        OutputStream output = resolver.openOutputStream(destination, "w")) {
                    if (output == null) throw new IOException("MediaStore output is unavailable");
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        if (canceled.get()) return;
                        output.write(buffer, 0, count);
                    }
                    output.flush();
                }
                if (canceled.get()) return;
                ContentValues publish = new ContentValues();
                publish.put(MediaStore.MediaColumns.IS_PENDING, 0);
                if (resolver.update(destination, publish, null, null) != 1)
                    throw new IOException("MediaStore could not publish the item");
                published = true;
                complete(PlatformPayload.nullValue());
            } finally {
                if (!published) resolver.delete(destination, null, null);
            }
        }
    }
}
