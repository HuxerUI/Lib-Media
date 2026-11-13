#import "media_library_browser.h"

#import <AVFoundation/AVFoundation.h>
#import <Photos/Photos.h>
#import <PhotosUI/PhotosUI.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <huxerui/ios/external_texture.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

NSString* const permissionError = @"media/permission";
NSString* const notFound = @"media/not-found";
NSString* const staleCursor = @"media/stale-cursor";
NSString* const ioError = @"media/io";

PHAuthorizationStatus ReadStatus() {
  return [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
}

bool CanRead(PHAuthorizationStatus status) {
  return status == PHAuthorizationStatusAuthorized || status == PHAuthorizationStatusLimited;
}

PHFetchOptions* AssetOptions(NSInteger filter) {
  PHFetchOptions* options = [[PHFetchOptions alloc] init];
  if (filter == 2) {
    options.predicate = [NSPredicate predicateWithFormat:@"mediaType IN %@", @[@(PHAssetMediaTypeImage), @(PHAssetMediaTypeVideo)]];
  } else {
    options.predicate = [NSPredicate predicateWithFormat:@"mediaType == %ld",
        static_cast<long>(filter == 0 ? PHAssetMediaTypeImage : PHAssetMediaTypeVideo)];
  }
  options.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"creationDate" ascending:NO]];
  return options;
}

HUXPlatformPayload* AssetPayload(PHAsset* asset) {
  NSDate* date = asset.creationDate;
  const double milliseconds = date.timeIntervalSince1970 * 1000;
  const bool video = asset.mediaType == PHAssetMediaTypeVideo;
  const double duration = asset.duration * 1000;
  constexpr double int64Limit = 0x1p63;
  return [HUXPlatformPayload objectValue:@{
    @"id": [HUXPlatformPayload stringValue:asset.localIdentifier],
    @"video": [HUXPlatformPayload booleanValue:video],
    @"date": date && std::isfinite(milliseconds) && std::abs(milliseconds) < int64Limit
        ? [HUXPlatformPayload integerValue:std::llround(milliseconds)] : [HUXPlatformPayload nullValue],
    @"duration": video && std::isfinite(duration) && duration >= 0 && duration < int64Limit
        ? [HUXPlatformPayload integerValue:std::llround(duration)] : [HUXPlatformPayload nullValue],
  }];
}

NSURL* TemporaryURL(NSString* extension) {
  NSString* name = [@"huxerui-media-" stringByAppendingString:NSUUID.UUID.UUIDString];
  if (extension.length > 0) name = [name stringByAppendingPathExtension:extension];
  return [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:name]];
}

HUXPlatformPayload* ImagePayload(UIImage* image, CGSize bounds) {
  CGSize size = CGSizeMake(image.size.width * image.scale, image.size.height * image.scale);
  if (size.width < 1 || size.height < 1) return nil;
  CGFloat ratio = std::min({1.0, bounds.width / size.width, bounds.height / size.height});
  size_t width = std::max<NSInteger>(1, std::lround(size.width * ratio));
  size_t height = std::max<NSInteger>(1, std::lround(size.height * ratio));
  CVPixelBufferRef buffer = nullptr;
  NSDictionary* attributes = @{(NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
      (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
      (NSString*)kCVPixelBufferCGBitmapContextCompatibilityKey: @YES};
  if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA,
                         (__bridge CFDictionaryRef)attributes, &buffer) != kCVReturnSuccess) return nil;
  if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
    CVPixelBufferRelease(buffer);
    return nil;
  }
  CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(buffer), width, height, 8,
      CVPixelBufferGetBytesPerRow(buffer), color,
      kCGBitmapByteOrder32Little | static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst));
  CGColorSpaceRelease(color);
  if (context) {
    CGRect rect = CGRectMake(0, 0, width, height);
    CGContextClearRect(context, rect);
    CGContextTranslateCTM(context, 0, height);
    CGContextScaleCTM(context, 1, -1);
    UIGraphicsPushContext(context);
    [image drawInRect:rect];
    UIGraphicsPopContext();
    CGContextRelease(context);
  }
  CVPixelBufferUnlockBaseAddress(buffer, 0);
  HUXPlatformPayload* payload = nil;
  if (context) {
    HUXPixelBufferTexture* texture = [[HUXPixelBufferTexture alloc] initWithIntrinsicSize:CGSizeMake(width, height)];
    [texture publishPixelBuffer:buffer];
    [texture finish];
    payload = [HUXPlatformPayload externalTextureValue:texture];
  }
  CVPixelBufferRelease(buffer);
  return payload;
}

} // namespace

@interface HUXMediaBrowseRequest : NSObject <HUXPlatformCancellation>
@property(nonatomic, strong) id<HUXPlatformResult> result;
@property(nonatomic, copy) void (^retired)(HUXMediaBrowseRequest*);
@property(nonatomic, copy) void (^cleanup)(void);
@property(nonatomic, copy) void (^progress)(double);
@property(nonatomic, copy) NSString* assetId;
@property(atomic) BOOL canceled;
- (void)finish:(HUXPlatformPayload*)payload code:(NSString*)code message:(NSString*)message;
@end

@implementation HUXMediaBrowseRequest
- (void)finish:(HUXPlatformPayload*)payload code:(NSString*)code message:(NSString*)message {
  dispatch_async(dispatch_get_main_queue(), ^{
    if (self.canceled || !self.result) return;
    if (code) [self.result failWithCode:code message:message ?: @"Photo library operation failed"
                               details:[HUXPlatformPayload nullValue]];
    else [self.result complete:payload];
    self.result = nil;
    self.cleanup = nil;
    self.progress = nil;
    if (self.retired) self.retired(self);
    self.retired = nil;
  });
}

- (void)cancel {
  if (self.canceled || !self.result) return;
  self.canceled = YES;
  if (self.cleanup) self.cleanup();
  self.cleanup = nil;
  self.result = nil;
  self.progress = nil;
  if (self.retired) self.retired(self);
  self.retired = nil;
}
@end

@interface HUXMediaAssetSnapshot : NSObject
@property(nonatomic, strong) PHFetchResult<PHAsset*>* assets;
@property(nonatomic, copy) NSString* token;
@end
@implementation HUXMediaAssetSnapshot
@end

@interface HUXMediaLibraryBrowser () <PHPhotoLibraryChangeObserver> {
  std::atomic_uint64_t _revision;
}
@property(nonatomic, strong) HUXPresentationCoordinator* coordinator;
@property(nonatomic, strong) id<HUXPlatformEventEmitter> events;
@property(nonatomic, strong) PHCachingImageManager* images;
@property(nonatomic, strong) NSMutableSet<HUXMediaBrowseRequest*>* requests;
@property(nonatomic, strong) NSCache<NSString*, HUXMediaAssetSnapshot*>* snapshots;
@property(nonatomic, strong) dispatch_queue_t queryQueue;
@property(nonatomic, strong) dispatch_queue_t imageQueue;
@property(nonatomic, strong) dispatch_queue_t fileQueue;
@property(nonatomic) BOOL observing;
@property(nonatomic) BOOL notificationPending;
@property(nonatomic) BOOL closed;
@end

@implementation HUXMediaLibraryBrowser
- (instancetype)initWithCoordinator:(HUXPresentationCoordinator*)coordinator
                             events:(id<HUXPlatformEventEmitter>)events {
  if (self = [super init]) {
    _revision = 0;
    _coordinator = coordinator;
    _events = events;
    _images = [[PHCachingImageManager alloc] init];
    _requests = [NSMutableSet set];
    _snapshots = [[NSCache alloc] init];
    _snapshots.countLimit = 12;
    _queryQueue = dispatch_queue_create("media.photos.query", DISPATCH_QUEUE_SERIAL);
    _imageQueue = dispatch_queue_create("media.photos.image", DISPATCH_QUEUE_SERIAL);
    _fileQueue = dispatch_queue_create("media.photos.file", DISPATCH_QUEUE_SERIAL);
  }
  return self;
}

- (void)observe {
  if (self.observing || !CanRead(ReadStatus())) return;
  [PHPhotoLibrary.sharedPhotoLibrary registerChangeObserver:self];
  self.observing = YES;
}

- (void)notifyChanged {
  if (self.closed || self.notificationPending) return;
  self.notificationPending = YES;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    self.notificationPending = NO;
    if (!self.closed) [self.events emit:@"changed" payload:[HUXPlatformPayload nullValue]];
  });
}

- (void)invalidate {
  ++_revision;
  [self.images stopCachingImagesForAllAssets];
  [self notifyChanged];
}

- (void)photoLibraryDidChange:(PHChange*)change {
  ++_revision;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (self.closed) return;
    [self.images stopCachingImagesForAllAssets];
    [self notifyChanged];
  });
}

- (void)dispose {
  self.closed = YES;
  if (self.observing) [PHPhotoLibrary.sharedPhotoLibrary unregisterChangeObserver:self];
  self.observing = NO;
  for (HUXMediaBrowseRequest* request in self.requests.allObjects) [request cancel];
  [self.images stopCachingImagesForAllAssets];
  [self.snapshots removeAllObjects];
}

- (HUXMediaBrowseRequest*)request:(id<HUXPlatformResult>)result arguments:(HUXPlatformPayload*)arguments {
  HUXMediaBrowseRequest* request = [[HUXMediaBrowseRequest alloc] init];
  request.result = result;
  __weak HUXMediaLibraryBrowser* weak = self;
  request.retired = ^(HUXMediaBrowseRequest* finished) { [weak.requests removeObject:finished]; };
  [self.requests addObject:request];
  if (arguments.kind == HUXPlatformPayloadKindObject) {
    HUXPlatformPayload* field = [arguments field:@"progressId"];
    if (field.integerValue > 0) {
      __weak HUXMediaBrowseRequest* weakRequest = request;
      request.progress = ^(double fraction) {
        dispatch_async(dispatch_get_main_queue(), ^{
          HUXMediaBrowseRequest* live = weakRequest;
          if (live && !live.canceled && live.result) {
            [weak.events emit:@"progress" payload:[HUXPlatformPayload objectValue:@{
              @"id": field, @"fraction": [HUXPlatformPayload doubleValue:fraction],
            }]];
          }
        });
      };
    }
  }
  return request;
}

- (id<HUXPlatformCancellation>)invoke:(NSString*)method arguments:(HUXPlatformPayload*)arguments
                              result:(id<HUXPlatformResult>)result {
  if ([method isEqualToString:@"observe"] || [method isEqualToString:@"invalidate"]) {
    [self observe];
    if ([method isEqualToString:@"invalidate"]) [self invalidate];
    [result complete:[HUXPlatformPayload nullValue]];
    return nil;
  }
  BOOL reportsProgress = [method isEqualToString:@"loadImage"] || [method isEqualToString:@"openAsset"];
  HUXMediaBrowseRequest* request = [self request:result arguments:reportsProgress ? arguments : nil];
  if (!CanRead(ReadStatus())) {
    BOOL continuing = [method isEqualToString:@"listAssets"] &&
        [arguments field:@"cursor"].kind != HUXPlatformPayloadKindNull;
    [request finish:nil code:continuing ? staleCursor : permissionError
             message:@"Allow access to Photos to browse the library"];
  } else if ([method isEqualToString:@"manageLimitedAccess"]) {
    [self manageLimitedAccess:request];
  } else {
    [self observe];
    dispatch_queue_t queue = [method isEqualToString:@"loadImage"] ? self.imageQueue : self.queryQueue;
    dispatch_async(queue, ^{
      if (request.canceled) return;
      if ([method isEqualToString:@"listAssets"]) [self page:arguments request:request];
      else if ([method isEqualToString:@"listAlbums"]) [self albums:arguments request:request];
      else if ([method isEqualToString:@"findAssets"]) [self find:arguments request:request];
      else if ([method isEqualToString:@"loadImage"] || [method isEqualToString:@"openAsset"])
        [self resolveAsset:arguments image:[method isEqualToString:@"loadImage"] request:request];
      else [request finish:nil code:@"media/unsupported" message:@"Unknown media library operation"];
    });
  }
  return request;
}

- (void)manageLimitedAccess:(HUXMediaBrowseRequest*)request {
  if (ReadStatus() != PHAuthorizationStatusLimited) {
    [request finish:nil code:@"media/unavailable" message:@"Photos access is no longer limited"];
    return;
  }
  __weak HUXMediaBrowseRequest* weakRequest = request;
  __weak HUXMediaLibraryBrowser* weak = self;
  id<HUXPlatformCancellation> presentation = [self.coordinator enqueueWithStart:
      ^id<HUXPlatformCancellation>(UIViewController* presenter, void (^finished)(void)) {
    HUXMediaBrowseRequest* live = weakRequest;
    if (!live || live.canceled || ReadStatus() != PHAuthorizationStatusLimited) {
      [live finish:nil code:@"media/unavailable" message:@"Photos access is no longer limited"];
      finished();
      return nil;
    }
    [PHPhotoLibrary.sharedPhotoLibrary presentLimitedLibraryPickerFromViewController:presenter
        completionHandler:^(NSArray<NSString*>*) {
      dispatch_async(dispatch_get_main_queue(), ^{
        finished();
        [weak invalidate];
        [weakRequest finish:[HUXPlatformPayload nullValue] code:nil message:nil];
      });
    }];
    // UIKit owns the permission UI. Keep the presentation slot until its completion.
    return live;
  } failure:^(NSError* error) {
    [weakRequest finish:nil code:@"media/unavailable" message:error.localizedDescription];
  }];
  request.cleanup = ^{ [presentation cancel]; };
}

- (void)page:(HUXPlatformPayload*)args request:(HUXMediaBrowseRequest*)request {
  const auto revision = _revision.load();
  const auto access = ReadStatus();
  NSInteger filter = [args field:@"filter"].integerValue;
  NSUInteger limit = [args field:@"limit"].integerValue;
  HUXPlatformPayload* field = [args field:@"album"];
  NSString* album = field.kind == HUXPlatformPayloadKindNull ? @"" : field.stringValue;
  NSString* key = [NSString stringWithFormat:@"%llu:%ld:%ld:%lu:%@", static_cast<unsigned long long>(revision),
      static_cast<long>(access), static_cast<long>(filter), static_cast<unsigned long>(limit), album];
  HUXMediaAssetSnapshot* snapshot = [self.snapshots objectForKey:key];
  HUXPlatformPayload* cursor = [args field:@"cursor"];
  NSUInteger start = 0;
  if (cursor.kind != HUXPlatformPayloadKindNull) {
    NSArray<NSString*>* parts = [cursor.stringValue componentsSeparatedByString:@":"];
    long long offset = parts.count == 2 ? parts[1].longLongValue : 0;
    if (!snapshot || parts.count != 2 || ![snapshot.token isEqualToString:parts[0]] ||
        offset <= 0 || static_cast<NSUInteger>(offset) >= snapshot.assets.count ||
        ![parts[1] isEqualToString:[NSString stringWithFormat:@"%lld", offset]]) {
      [request finish:nil code:staleCursor message:@"Photo library page changed; restart from the first page"];
      return;
    }
    start = static_cast<NSUInteger>(offset);
  }
  if (!snapshot) {
    PHFetchOptions* options = AssetOptions(filter);
    PHFetchResult<PHAsset*>* assets;
    if (album.length == 0) {
      assets = [PHAsset fetchAssetsWithOptions:options];
    } else {
      PHAssetCollection* collection = [PHAssetCollection fetchAssetCollectionsWithLocalIdentifiers:@[album]
                                                                                         options:nil].firstObject;
      assets = collection ? [PHAsset fetchAssetsInAssetCollection:collection options:options] : nil;
      if (assets.count == 0) {
        [request finish:nil code:notFound message:@"Album has no visible matching media"];
        return;
      }
    }
    snapshot = [[HUXMediaAssetSnapshot alloc] init];
    snapshot.assets = assets;
    snapshot.token = NSUUID.UUID.UUIDString;
    [self.snapshots setObject:snapshot forKey:key];
  }
  NSMutableArray* assets = [NSMutableArray arrayWithCapacity:limit];
  NSUInteger end = std::min(start + limit, snapshot.assets.count);
  for (NSUInteger index = start; index < end && !request.canceled; ++index)
    [assets addObject:AssetPayload([snapshot.assets objectAtIndex:index])];
  if (revision != _revision.load() || access != ReadStatus()) {
    [request finish:nil code:staleCursor message:@"Photo library changed during the query"];
    return;
  }
  HUXPlatformPayload* next = end < snapshot.assets.count ? [HUXPlatformPayload stringValue:
      [NSString stringWithFormat:@"%@:%lu", snapshot.token, static_cast<unsigned long>(end)]]
      : [HUXPlatformPayload nullValue];
  [request finish:[HUXPlatformPayload objectValue:@{@"assets": [HUXPlatformPayload listValue:assets], @"next": next}]
             code:nil message:nil];
}

- (void)albums:(HUXPlatformPayload*)args request:(HUXMediaBrowseRequest*)request {
  const auto revision = _revision.load();
  const auto access = ReadStatus();
  PHFetchOptions* options = AssetOptions([args field:@"filter"].integerValue);
  options.fetchLimit = 1;
  NSMutableArray<NSDictionary*>* albums = [NSMutableArray array];
  for (NSNumber* type in @[@(PHAssetCollectionTypeAlbum), @(PHAssetCollectionTypeSmartAlbum)]) {
    PHFetchResult<PHAssetCollection*>* collections = [PHAssetCollection fetchAssetCollectionsWithType:
        static_cast<PHAssetCollectionType>(type.integerValue) subtype:PHAssetCollectionSubtypeAny options:nil];
    [collections enumerateObjectsUsingBlock:^(PHAssetCollection* collection, NSUInteger, BOOL* stop) {
      if (request.canceled) { *stop = YES; return; }
      if (collection.assetCollectionSubtype == PHAssetCollectionSubtypeSmartAlbumUserLibrary ||
          collection.assetCollectionSubtype == PHAssetCollectionSubtypeSmartAlbumAllHidden) return;
      PHAsset* cover = [PHAsset fetchAssetsInAssetCollection:collection options:options].firstObject;
      if (cover) [albums addObject:@{@"id": collection.localIdentifier, @"name": collection.localizedTitle ?: @"Other",
          @"cover": cover.localIdentifier, @"date": cover.creationDate ?: NSDate.distantPast}];
    }];
  }
  if (revision != _revision.load() || access != ReadStatus()) {
    [request finish:nil code:staleCursor message:@"Photo library changed during the album query"];
    return;
  }
  [albums sortUsingComparator:^NSComparisonResult(NSDictionary* left, NSDictionary* right) {
    NSComparisonResult result = [right[@"date"] compare:left[@"date"]];
    return result == NSOrderedSame ? [left[@"id"] compare:right[@"id"]] : result;
  }];
  NSMutableArray* values = [NSMutableArray arrayWithCapacity:albums.count];
  for (NSDictionary* album in albums) {
    [values addObject:[HUXPlatformPayload objectValue:@{@"id": [HUXPlatformPayload stringValue:album[@"id"]],
        @"name": [HUXPlatformPayload stringValue:album[@"name"]], @"cover": [HUXPlatformPayload stringValue:album[@"cover"]]}]];
  }
  [request finish:[HUXPlatformPayload listValue:values] code:nil message:nil];
}

- (void)find:(HUXPlatformPayload*)args request:(HUXMediaBrowseRequest*)request {
  const auto access = ReadStatus();
  const auto revision = _revision.load();
  NSUInteger count = [args field:@"count"].integerValue;
  HUXPlatformPayload* ids = [args field:@"ids"];
  NSMutableArray<NSString*>* ordered = [NSMutableArray arrayWithCapacity:count];
  for (NSUInteger index = 0; index < count; ++index) [ordered addObject:[ids elementAtIndex:index].stringValue];
  NSMutableDictionary<NSString*, HUXPlatformPayload*>* found = [NSMutableDictionary dictionary];
  [[PHAsset fetchAssetsWithLocalIdentifiers:ordered options:AssetOptions(2)]
      enumerateObjectsUsingBlock:^(PHAsset* asset, NSUInteger, BOOL* stop) {
    if (request.canceled) { *stop = YES; return; }
    found[asset.localIdentifier] = AssetPayload(asset);
  }];
  if (access != ReadStatus() || revision != _revision.load()) {
    [request finish:nil code:@"media/unavailable" message:@"Photo library changed during lookup"];
    return;
  }
  NSMutableArray* result = [NSMutableArray arrayWithCapacity:count];
  for (NSString* key in ordered) if (found[key]) [result addObject:found[key]];
  [request finish:[HUXPlatformPayload listValue:result] code:nil message:nil];
}

- (void)resolveAsset:(HUXPlatformPayload*)args image:(BOOL)image request:(HUXMediaBrowseRequest*)request {
  const auto revision = _revision.load();
  PHAsset* asset = [PHAsset fetchAssetsWithLocalIdentifiers:@[[args field:@"id"].stringValue]
                                                   options:AssetOptions(2)].firstObject;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (request.canceled) return;
    if (!CanRead(ReadStatus())) [request finish:nil code:permissionError message:@"Photos access was revoked"];
    else if (revision != _revision.load())
      [request finish:nil code:@"media/unavailable" message:@"Photo library changed while resolving the asset"];
    else if (!asset) [request finish:nil code:notFound message:@"Media asset is no longer visible"];
    else if (image) [self loadImage:asset arguments:args request:request];
    else {
      request.assetId = asset.localIdentifier;
      [self openAsset:asset request:request];
    }
  });
}

- (void)finishFile:(HUXFileReference*)reference error:(NSError*)error request:(HUXMediaBrowseRequest*)request {
  if (request.canceled) return;
  if (!CanRead(ReadStatus())) {
    [request finish:nil code:permissionError message:@"Photos access was revoked"];
  } else if (![PHAsset fetchAssetsWithLocalIdentifiers:@[request.assetId] options:AssetOptions(2)].firstObject) {
    [request finish:nil code:notFound message:@"Media asset is no longer visible"];
  } else if (reference) {
    [request finish:[HUXPlatformPayload fileReferenceValue:reference] code:nil message:nil];
  } else {
    [request finish:nil code:ioError message:error.localizedDescription ?: @"Could not prepare a readable media file"];
  }
}

- (void)loadImage:(PHAsset*)asset arguments:(HUXPlatformPayload*)args request:(HUXMediaBrowseRequest*)request {
  CGSize size = CGSizeMake([args field:@"width"].integerValue, [args field:@"height"].integerValue);
  PHImageRequestOptions* options = [[PHImageRequestOptions alloc] init];
  options.networkAccessAllowed = [args field:@"allowNetwork"].booleanValue;
  options.resizeMode = PHImageRequestOptionsResizeModeFast;
  options.deliveryMode = options.networkAccessAllowed ? PHImageRequestOptionsDeliveryModeHighQualityFormat
                                                       : PHImageRequestOptionsDeliveryModeFastFormat;
  void (^progress)(double) = request.progress;
  if (progress) options.progressHandler = ^(double value, NSError*, BOOL*, NSDictionary*) { progress(value); };
  const auto revision = _revision.load();
  PHImageRequestID token = [self.images requestImageForAsset:asset targetSize:size contentMode:PHImageContentModeAspectFit
      options:options resultHandler:^(UIImage* image, NSDictionary* info) {
    if (request.canceled) return;
    if ([info[PHImageCancelledKey] boolValue]) {
      [request finish:nil code:@"media/unavailable" message:@"Photos canceled the image request"];
      return;
    }
    if (options.networkAccessAllowed && [info[PHImageResultIsDegradedKey] boolValue]) return;
    NSError* error = info[PHImageErrorKey];
    if (!image) {
      if (!options.networkAccessAllowed && [info[PHImageResultIsInCloudKey] boolValue])
        [request finish:[HUXPlatformPayload nullValue] code:nil message:nil];
      else if (error) [request finish:nil code:ioError message:error.localizedDescription];
      else [request finish:[HUXPlatformPayload nullValue] code:nil message:nil];
      return;
    }
    dispatch_async(self.imageQueue, ^{
      if (request.canceled) return;
      if (revision != _revision.load() || !CanRead(ReadStatus())) {
        [request finish:nil code:@"media/unavailable" message:@"Photo library changed during image loading"];
        return;
      }
      HUXPlatformPayload* payload = ImagePayload(image, size);
      [request finish:payload code:payload ? nil : ioError message:@"Could not prepare the photo preview"];
    });
  }];
  PHCachingImageManager* manager = self.images;
  request.cleanup = ^{ [manager cancelImageRequest:token]; };
}

- (void)copyFile:(NSURL*)url deleteSource:(BOOL)remove request:(HUXMediaBrowseRequest*)request {
  dispatch_async(self.fileQueue, ^{
    NSError* error = nil;
    HUXFileReference* reference = !request.canceled && CanRead(ReadStatus())
        ? [HUXFileReference copyFromFileURL:url error:&error] : nil;
    if (remove) [NSFileManager.defaultManager removeItemAtURL:url error:nil];
    [self finishFile:reference error:error request:request];
  });
}

- (void)openAsset:(PHAsset*)asset request:(HUXMediaBrowseRequest*)request {
  void (^progress)(double) = request.progress;
  if (asset.mediaType == PHAssetMediaTypeImage) {
    PHImageRequestOptions* options = [[PHImageRequestOptions alloc] init];
    options.networkAccessAllowed = YES;
    options.version = PHImageRequestOptionsVersionCurrent;
    options.deliveryMode = PHImageRequestOptionsDeliveryModeHighQualityFormat;
    if (progress) options.progressHandler = ^(double value, NSError*, BOOL*, NSDictionary*) { progress(value); };
    PHImageRequestID token = [self.images requestImageDataAndOrientationForAsset:asset options:options
        resultHandler:^(NSData* data, NSString* type, CGImagePropertyOrientation, NSDictionary* info) {
      if (request.canceled || [info[PHImageResultIsDegradedKey] boolValue]) return;
      if ([info[PHImageCancelledKey] boolValue]) {
        [request finish:nil code:@"media/unavailable" message:@"Photos canceled the file request"];
        return;
      }
      if (!data) {
        [request finish:nil code:ioError message:[info[PHImageErrorKey] localizedDescription]];
        return;
      }
      dispatch_async(self.fileQueue, ^{
        if (request.canceled) return;
        NSURL* url = TemporaryURL(type ? [UTType typeWithIdentifier:type].preferredFilenameExtension : nil);
        NSError* error = nil;
        HUXFileReference* reference = [data writeToURL:url options:0 error:&error] && !request.canceled
            ? [HUXFileReference copyFromFileURL:url error:&error] : nil;
        [NSFileManager.defaultManager removeItemAtURL:url error:nil];
        [self finishFile:reference error:error request:request];
      });
    }];
    PHCachingImageManager* manager = self.images;
    request.cleanup = ^{ [manager cancelImageRequest:token]; };
    return;
  }

  PHVideoRequestOptions* options = [[PHVideoRequestOptions alloc] init];
  options.networkAccessAllowed = YES;
  options.version = PHVideoRequestOptionsVersionCurrent;
  options.deliveryMode = PHVideoRequestOptionsDeliveryModeHighQualityFormat;
  if (progress) options.progressHandler = ^(double value, NSError*, BOOL*, NSDictionary*) { progress(value); };
  PHImageRequestID token = [self.images requestAVAssetForVideo:asset options:options
      resultHandler:^(AVAsset* source, AVAudioMix* mix, NSDictionary* info) {
    if (request.canceled) return;
    if ([info[PHImageCancelledKey] boolValue]) {
      [request finish:nil code:@"media/unavailable" message:@"Photos canceled the video request"];
      return;
    }
    if ([source isKindOfClass:AVURLAsset.class] && ((AVURLAsset*)source).URL.isFileURL && !mix) {
      [self copyFile:((AVURLAsset*)source).URL deleteSource:NO request:request];
    } else if (source) {
      dispatch_async(dispatch_get_main_queue(), ^{ [self exportVideo:source mix:mix request:request]; });
    } else [request finish:nil code:ioError message:[info[PHImageErrorKey] localizedDescription]];
  }];
  PHCachingImageManager* manager = self.images;
  request.cleanup = ^{ [manager cancelImageRequest:token]; };
}

- (void)exportVideo:(AVAsset*)source mix:(AVAudioMix*)mix request:(HUXMediaBrowseRequest*)request {
  if (request.canceled) return;
  AVAssetExportSession* exporter = [[AVAssetExportSession alloc] initWithAsset:source
      presetName:mix ? AVAssetExportPresetHighestQuality : AVAssetExportPresetPassthrough];
  if (![exporter.supportedFileTypes containsObject:AVFileTypeQuickTimeMovie]) {
    exporter = [[AVAssetExportSession alloc] initWithAsset:source presetName:AVAssetExportPresetHighestQuality];
  }
  if (![exporter.supportedFileTypes containsObject:AVFileTypeQuickTimeMovie]) {
    [request finish:nil code:@"media/unsupported" message:@"Photos could not export this video representation"];
    return;
  }
  NSURL* url = TemporaryURL(@"mov");
  exporter.outputURL = url;
  exporter.outputFileType = AVFileTypeQuickTimeMovie;
  exporter.audioMix = mix;
  request.cleanup = ^{ [exporter cancelExport]; };
  [exporter exportAsynchronouslyWithCompletionHandler:^{
    if (exporter.status == AVAssetExportSessionStatusCompleted) [self copyFile:url deleteSource:YES request:request];
    else {
      [NSFileManager.defaultManager removeItemAtURL:url error:nil];
      [request finish:nil code:ioError message:exporter.error.localizedDescription];
    }
  }];
}
@end
