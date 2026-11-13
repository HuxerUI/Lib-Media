package org.huxerui.lib.media;

import android.Manifest;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.Context;
import android.content.pm.PackageManager;
import android.database.ContentObserver;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageDecoder;
import android.graphics.Matrix;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.provider.MediaStore;
import android.util.Size;

import org.huxerui.HuxerUIFileReference;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.PlatformPayload;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;

final class MediaLibraryBrowser {
    private static final String ID = "_id";
    private static final String TAKEN = "datetaken";
    private static final String ADDED = "date_added";
    private static final String DURATION = "duration";
    private static final String BUCKET = "bucket_id";
    private static final String BUCKET_NAME = "bucket_display_name";
    private static final String HAS_DATE = TAKEN + " > 0";
    private static final String NO_DATE = "(" + TAKEN + " IS NULL OR " + TAKEN + " <= 0)";
    private static final Comparator<Asset> NEWEST = (left, right) -> {
        int byDate = Long.compare(right.date, left.date);
        if (byDate != 0) return byDate;
        int bySource = left.source.key.compareTo(right.source.key);
        return bySource != 0 ? bySource : Long.compare(right.row, left.row);
    };

    private final Context context;
    private final ContentResolver resolver;
    private final HuxerUIPlatformChannel.Events events;
    private final Handler main;
    private final AtomicLong revision = new AtomicLong();
    private final String session = UUID.randomUUID().toString();
    private final Runnable notifyChanged;
    private final ContentObserver observer;
    private boolean observing;
    private boolean notificationPending;

    MediaLibraryBrowser(Context context, HuxerUIPlatformChannel.Events events, Handler main) {
        this.context = context;
        this.resolver = context.getContentResolver();
        this.events = events;
        this.main = main;
        notifyChanged = () -> {
            notificationPending = false;
            if (observing) events.emit("changed");
        };
        observer = new ContentObserver(main) {
            @Override public void onChange(boolean selfChange) { invalidate(); }
        };
    }

    void observe() {
        if (observing) return;
        resolver.registerContentObserver(Uri.parse("content://" + MediaStore.AUTHORITY), true, observer);
        observing = true;
    }

    void invalidate() {
        revision.incrementAndGet();
        if (observing && !notificationPending) {
            notificationPending = true;
            main.postDelayed(notifyChanged, 40);
        }
    }

    void close() {
        main.removeCallbacks(notifyChanged);
        if (observing) resolver.unregisterContentObserver(observer);
        observing = false;
    }

    static final class Failure extends IOException {
        final String code;
        Failure(String code, String message) { super(message); this.code = code; }
    }

    PlatformPayload invoke(String method, PlatformPayload args, CancellationSignal signal) throws IOException {
        switch (method) {
            case "listAlbums": return albums(filter(args), signal);
            case "listAssets": return page(args, signal);
            case "findAssets": return find(args.requireField("ids").elements(), signal);
            case "loadImage": return image(args, signal);
            default: throw new Failure("media/unsupported", "Unknown media library operation");
        }
    }

    private static int filter(PlatformPayload args) { return (int) args.requireField("filter").requireInt64(); }

    private boolean granted(String permission) {
        return context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    private int access() {
        if (Build.VERSION.SDK_INT < 33 || context.getApplicationInfo().targetSdkVersion < 33)
            return granted(Manifest.permission.READ_EXTERNAL_STORAGE) ? 3 : 0;
        int mask = (granted(Manifest.permission.READ_MEDIA_IMAGES) ? 1 : 0)
                | (granted(Manifest.permission.READ_MEDIA_VIDEO) ? 2 : 0);
        if (Build.VERSION.SDK_INT >= 34 && granted(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)) mask |= 4;
        return mask;
    }

    private static boolean readable(int access, boolean video) {
        return (access & (video ? 6 : 5)) != 0;
    }

    private static final class Source {
        final String volume;
        final boolean video;
        final String key;
        final Uri uri;

        Source(String volume, boolean video) {
            this.volume = volume;
            this.video = video;
            key = volume + ":" + (video ? "v" : "i");
            uri = video ? MediaStore.Video.Media.getContentUri(volume) : MediaStore.Images.Media.getContentUri(volume);
        }
    }

    private List<Source> sources(int filter, int access) throws Failure {
        List<String> volumes = Build.VERSION.SDK_INT >= 29
                ? new ArrayList<>(MediaStore.getExternalVolumeNames(context))
                : Collections.singletonList("external");
        if (volumes.size() > 1) Collections.sort(volumes);
        boolean images = filter != 1 && readable(access, false);
        boolean videos = filter != 0 && readable(access, true);
        if (!images && !videos) throw new Failure("media/permission", "Allow access to photos or videos to browse the library");
        List<Source> result = new ArrayList<>();
        for (String volume : volumes) {
            if (images) result.add(new Source(volume, false));
            if (videos) result.add(new Source(volume, true));
        }
        return result;
    }

    private String stamp(List<Source> sources, int access) {
        StringBuilder stamp = new StringBuilder().append(revision.get()).append(':').append(access);
        String last = null;
        for (Source source : sources) {
            if (source.volume.equals(last)) continue;
            last = source.volume;
            stamp.append(':').append(last);
            if (Build.VERSION.SDK_INT >= 30) {
                stamp.append(':').append(MediaStore.getVersion(context, last));
                stamp.append(':').append(MediaStore.getGeneration(context, last));
            }
        }
        return stamp.toString();
    }

    private static final class Asset {
        final Source source;
        final long row;
        final long date;
        final long duration;

        Asset(Source source, Cursor cursor) {
            this.source = source;
            row = cursor.getLong(0);
            long taken = cursor.getLong(1);
            long added = cursor.getLong(2);
            date = taken > 0 ? taken : added > 0 && added <= Long.MAX_VALUE / 1000 ? added * 1000 : 0;
            duration = source.video && !cursor.isNull(3) ? cursor.getLong(3) : -1;
        }

        String id() { return source.key + ":" + row; }
        Uri uri() { return ContentUris.withAppendedId(source.uri, row); }
        PlatformPayload payload() {
            Map<String, PlatformPayload> fields = new HashMap<>();
            fields.put("id", PlatformPayload.string(id()));
            fields.put("video", PlatformPayload.booleanValue(source.video));
            fields.put("date", date > 0 ? PlatformPayload.int64(date) : PlatformPayload.nullValue());
            fields.put("duration", duration >= 0 ? PlatformPayload.int64(duration) : PlatformPayload.nullValue());
            return PlatformPayload.object(fields);
        }
    }

    private static final class Album {
        final String id;
        final String name;
        Asset cover;

        Album(String id, String name, Asset cover) { this.id = id; this.name = name; this.cover = cover; }
        PlatformPayload payload() {
            Map<String, PlatformPayload> fields = new HashMap<>();
            fields.put("id", PlatformPayload.string(id));
            fields.put("name", PlatformPayload.string(name));
            fields.put("cover", PlatformPayload.string(cover.id()));
            return PlatformPayload.object(fields);
        }
    }

    private static String[] projection(Source source, boolean albums) {
        if (albums) return source.video ? new String[]{ID, TAKEN, ADDED, DURATION, BUCKET, BUCKET_NAME}
                : new String[]{ID, TAKEN, ADDED, BUCKET, BUCKET_NAME};
        return source.video ? new String[]{ID, TAKEN, ADDED, DURATION} : new String[]{ID, TAKEN, ADDED};
    }

    private Cursor query(Uri uri, String[] columns, String where, String[] values, String sort,
                         int limit, CancellationSignal signal) throws IOException {
        Cursor cursor;
        if (limit > 0 && Build.VERSION.SDK_INT >= 26) {
            Bundle args = new Bundle();
            if (where != null) args.putString(ContentResolver.QUERY_ARG_SQL_SELECTION, where);
            if (values != null) args.putStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, values);
            args.putString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER, sort);
            args.putInt(ContentResolver.QUERY_ARG_LIMIT, limit);
            cursor = resolver.query(uri, columns, args, signal);
        } else {
            cursor = resolver.query(uri, columns, where, values, sort, signal);
        }
        if (cursor == null) throw new IOException("MediaStore did not return a cursor");
        return cursor;
    }

    private PlatformPayload albums(int filter, CancellationSignal signal) throws IOException {
        Map<String, Album> albums = new HashMap<>();
        for (Source source : sources(filter, access())) {
            try (Cursor cursor = query(source.uri, projection(source, true), null, null, null, 0, signal)) {
                int bucket = source.video ? 4 : 3;
                while (cursor.moveToNext()) {
                    signal.throwIfCanceled();
                    if (cursor.isNull(bucket)) continue;
                    String id = source.volume + ":" + cursor.getString(bucket);
                    Asset asset = new Asset(source, cursor);
                    Album album = albums.get(id);
                    if (album == null) {
                        String name = cursor.getString(bucket + 1);
                        albums.put(id, new Album(id, name == null || name.isEmpty() ? "Other" : name, asset));
                    } else if (NEWEST.compare(asset, album.cover) < 0) album.cover = asset;
                }
            }
        }
        ArrayList<Album> ordered = new ArrayList<>(albums.values());
        Collections.sort(ordered, (left, right) -> {
            int byCover = NEWEST.compare(left.cover, right.cover);
            return byCover != 0 ? byCover : left.id.compareTo(right.id);
        });
        List<PlatformPayload> result = new ArrayList<>();
        for (Album album : ordered) result.add(album.payload());
        return PlatformPayload.list(result);
    }

    private PlatformPayload page(PlatformPayload args, CancellationSignal signal) throws IOException {
        int filter = filter(args);
        int limit = (int) args.requireField("limit").requireInt64();
        PlatformPayload albumValue = args.requireField("album");
        String album = albumValue.isNull() ? "" : albumValue.requireString();
        int access = access();
        PlatformPayload cursor = args.requireField("cursor");
        List<Source> sources;
        try {
            sources = sources(filter, access);
        } catch (Failure error) {
            if (!cursor.isNull()) throw stale();
            throw error;
        }
        String stamp = stamp(sources, access);
        String query = session + "|" + stamp + "|" + filter + "|" + limit + "|" + album + "|";
        long date = 0;
        String sourceKey = "";
        long row = 0;
        if (!cursor.isNull()) {
            String token = cursor.requireString();
            if (!token.startsWith(query)) throw stale();
            String[] key = token.substring(query.length()).split("\\|", -1);
            if (key.length != 3) throw stale();
            try {
                date = Long.parseLong(key[0]);
                sourceKey = key[1];
                row = Long.parseLong(key[2]);
            } catch (NumberFormatException error) { throw stale(); }
        }

        ArrayList<Asset> assets = new ArrayList<>();
        for (Source source : sources) {
            if (!album.isEmpty() && !album.startsWith(source.volume + ":")) continue;
            String bucket = album.isEmpty() ? null : album.substring(source.volume.length() + 1);
            readLane(assets, source, bucket, TAKEN, true, date, sourceKey, row, limit + 1, signal);
            readLane(assets, source, bucket, ADDED, false, date, sourceKey, row, limit + 1, signal);
        }
        // Undated items cannot appear in this page when the dated results already fill it and its lookahead.
        if (assets.size() <= limit) {
            for (Source source : sources) {
                if (!album.isEmpty() && !album.startsWith(source.volume + ":")) continue;
                String bucket = album.isEmpty() ? null : album.substring(source.volume.length() + 1);
                readUndated(assets, source, bucket, date, sourceKey, row, limit + 1, signal);
            }
        }
        int currentAccess = access();
        if (currentAccess != access || !stamp.equals(stamp(sources(filter, currentAccess), currentAccess))) throw stale();
        if (!album.isEmpty() && cursor.isNull() && assets.isEmpty())
            throw new Failure("media/not-found", "Album has no visible matching media");
        Collections.sort(assets, NEWEST);
        List<PlatformPayload> values = new ArrayList<>();
        for (int index = 0; index < Math.min(limit, assets.size()); ++index) values.add(assets.get(index).payload());
        Map<String, PlatformPayload> result = new HashMap<>();
        result.put("assets", PlatformPayload.list(values));
        PlatformPayload next = PlatformPayload.nullValue();
        if (assets.size() > limit) {
            Asset last = assets.get(limit - 1);
            next = PlatformPayload.string(query + last.date + "|" + last.source.key + "|" + last.row);
        }
        result.put("next", next);
        return PlatformPayload.object(result);
    }

    private void readLane(List<Asset> assets, Source source, String bucket, String column, boolean taken,
                          long date, String sourceKey, long row, int limit, CancellationSignal signal) throws IOException {
        String where = taken ? HAS_DATE : NO_DATE + " AND " + ADDED + " > 0";
        List<String> values = new ArrayList<>();
        if (bucket != null) {
            where += " AND " + BUCKET + " = ?";
            values.add(bucket);
        }
        String sort = column + " DESC, " + ID + " DESC";
        if (!sourceKey.isEmpty()) {
            if (date == 0) return;
            int order = source.key.compareTo(sourceKey);
            long cutoff = taken ? date : date / 1000;
            boolean exact = taken || date % 1000 == 0;
            String less = column + (order > 0 || !exact ? " <= ?" : " < ?");
            values.add(Long.toString(cutoff));
            if (order == 0 && exact) {
                less += " OR (" + column + " = ? AND " + ID + " < ?)";
                values.add(Long.toString(cutoff));
                values.add(Long.toString(row));
            }
            where += " AND (" + less + ")";
        }
        try (Cursor cursor = query(source.uri, projection(source, false), where,
                values.toArray(new String[0]), sort, limit, signal)) {
            for (int count = 0; count < limit && cursor.moveToNext(); ++count) assets.add(new Asset(source, cursor));
        }
    }

    private void readUndated(List<Asset> assets, Source source, String bucket, long date, String sourceKey,
                             long row, int limit, CancellationSignal signal) throws IOException {
        String where = NO_DATE + " AND (" + ADDED + " IS NULL OR " + ADDED + " <= 0)";
        List<String> values = new ArrayList<>();
        if (bucket != null) {
            where += " AND " + BUCKET + " = ?";
            values.add(bucket);
        }
        if (date == 0 && !sourceKey.isEmpty()) {
            int order = source.key.compareTo(sourceKey);
            if (order < 0) return;
            if (order == 0) {
                where += " AND " + ID + " < ?";
                values.add(Long.toString(row));
            }
        }
        try (Cursor cursor = query(source.uri, projection(source, false), where, values.toArray(new String[0]),
                ID + " DESC", limit, signal)) {
            for (int count = 0; count < limit && cursor.moveToNext(); ++count) assets.add(new Asset(source, cursor));
        }
    }

    private static Failure stale() { return new Failure("media/stale-cursor", "Media library page changed; restart from the first page"); }

    private static Failure missing() { return new Failure("media/not-found", "Media asset is no longer visible"); }

    private Asset lookup(String id, int access, CancellationSignal signal) throws IOException {
        String[] parts = id.split(":", -1);
        if (parts.length != 3 || !parts[0].matches("[a-zA-Z0-9_-]+") ||
                !(parts[1].equals("v") || parts[1].equals("i"))) return null;
        boolean video = parts[1].equals("v");
        if (!readable(access, video)) return null;
        long row;
        try { row = Long.parseLong(parts[2]); }
        catch (NumberFormatException error) { return null; }
        if (row < 0 || !Long.toString(row).equals(parts[2])) return null;
        if (Build.VERSION.SDK_INT >= 29 && !MediaStore.getExternalVolumeNames(context).contains(parts[0])) return null;
        if (Build.VERSION.SDK_INT < 29 && !parts[0].equals("external")) return null;
        Source source = new Source(parts[0], video);
        try (Cursor cursor = query(ContentUris.withAppendedId(source.uri, row), projection(source, false),
                null, null, null, 0, signal)) {
            return cursor.moveToFirst() ? new Asset(source, cursor) : null;
        } catch (FileNotFoundException error) { return null; }
    }

    private PlatformPayload find(List<PlatformPayload> ids, CancellationSignal signal) throws IOException {
        int access = access();
        if (access == 0) throw new Failure("media/permission", "Media library read access is unavailable");
        List<PlatformPayload> result = new ArrayList<>();
        Map<String, Asset> found = new HashMap<>();
        for (PlatformPayload id : ids) {
            signal.throwIfCanceled();
            String key = id.requireString();
            if (!found.containsKey(key)) found.put(key, lookup(key, access, signal));
            Asset asset = found.get(key);
            if (asset != null) result.add(asset.payload());
        }
        if (access != access()) throw new Failure("media/permission", "Media authorization changed during lookup");
        return PlatformPayload.list(result);
    }

    private Asset requireAsset(PlatformPayload args, CancellationSignal signal) throws IOException {
        int access = access();
        if (access == 0) throw new Failure("media/permission", "Media library read access is unavailable");
        Asset asset = lookup(args.requireField("id").requireString(), access, signal);
        if (asset == null) throw missing();
        return asset;
    }

    interface Completion {
        void complete(PlatformPayload value);
    }

    void open(PlatformPayload args, CancellationSignal signal, Completion completion) throws IOException {
        Asset asset = requireAsset(args, signal);
        signal.throwIfCanceled();
        try (HuxerUIFileReference reference = HuxerUIFileReference.fromUri(context, asset.uri())) {
            signal.throwIfCanceled();
            // The platform result encodes the reference synchronously while its Java owner is open.
            completion.complete(PlatformPayload.fileReference(reference));
        } catch (FileNotFoundException error) { throw missing(); }
    }

    private PlatformPayload image(PlatformPayload args, CancellationSignal signal) throws IOException {
        Asset asset = requireAsset(args, signal);
        int width = (int) args.requireField("width").requireInt64();
        int height = (int) args.requireField("height").requireInt64();
        Bitmap bitmap;
        try {
            if (Build.VERSION.SDK_INT >= 29 && (asset.source.video || Math.max(width, height) <= 512)) {
                bitmap = resolver.loadThumbnail(asset.uri(), new Size(width, height), signal);
            } else if (asset.source.video) {
                bitmap = MediaStore.Video.Thumbnails.getThumbnail(resolver, asset.row,
                        MediaStore.Video.Thumbnails.MINI_KIND, null);
            } else {
                bitmap = photo(asset.uri(), width, height, signal);
            }
        } catch (IOException error) {
            if (lookup(asset.id(), access(), signal) == null) throw missing();
            if (error instanceof FileNotFoundException) return PlatformPayload.nullValue();
            throw error;
        }
        signal.throwIfCanceled();
        if (bitmap == null) return PlatformPayload.nullValue();
        float scale = Math.min(1f, Math.min((float) width / bitmap.getWidth(), (float) height / bitmap.getHeight()));
        if (scale < 1f) bitmap = Bitmap.createScaledBitmap(bitmap, Math.max(1, Math.round(bitmap.getWidth() * scale)),
                Math.max(1, Math.round(bitmap.getHeight() * scale)), true);
        return imagePayload(bitmap, bitmap.getWidth(), bitmap.getHeight());
    }

    private Bitmap photo(Uri uri, int width, int height, CancellationSignal signal) throws IOException {
        if (Build.VERSION.SDK_INT >= 28) {
            return ImageDecoder.decodeBitmap(ImageDecoder.createSource(resolver, uri), (decoder, info, source) -> {
                int originalWidth = info.getSize().getWidth();
                int originalHeight = info.getSize().getHeight();
                float scale = Math.min(1f, Math.min((float) width / originalWidth, (float) height / originalHeight));
                decoder.setTargetSize(Math.max(1, Math.round(originalWidth * scale)),
                        Math.max(1, Math.round(originalHeight * scale)));
                decoder.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
            });
        }
        int orientation = 0;
        try (Cursor cursor = query(uri, new String[]{"orientation"}, null, null, null, 0, signal)) {
            if (cursor.moveToFirst()) orientation = cursor.getInt(0);
        }
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inJustDecodeBounds = true;
        try (InputStream stream = resolver.openInputStream(uri)) { BitmapFactory.decodeStream(stream, null, options); }
        if (options.outWidth <= 0 || options.outHeight <= 0) return null;
        options.inJustDecodeBounds = false;
        int targetWidth = orientation % 180 == 0 ? width : height;
        int targetHeight = orientation % 180 == 0 ? height : width;
        options.inSampleSize = 1;
        while (options.outWidth / options.inSampleSize / 2 >= targetWidth ||
                options.outHeight / options.inSampleSize / 2 >= targetHeight) {
            options.inSampleSize *= 2;
        }
        signal.throwIfCanceled();
        try (InputStream stream = resolver.openInputStream(uri)) {
            Bitmap bitmap = BitmapFactory.decodeStream(stream, null, options);
            if (bitmap == null || orientation == 0) return bitmap;
            Matrix rotation = new Matrix();
            rotation.postRotate(orientation);
            return Bitmap.createBitmap(bitmap, 0, 0, bitmap.getWidth(), bitmap.getHeight(), rotation, true);
        }
    }

    private static native PlatformPayload imagePayload(Bitmap bitmap, int width, int height) throws IOException;
}
