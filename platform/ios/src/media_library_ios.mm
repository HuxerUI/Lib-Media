#import <Photos/Photos.h>
#import <PhotosUI/PhotosUI.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <huxerui/ios/platform_registry.h>
#include "library/backend.h"

@interface HUXMediaLibraryRequest : NSObject <HUXPlatformCancellation, PHPickerViewControllerDelegate,
    UIAdaptivePresentationControllerDelegate>
@property(nonatomic, strong) id<HUXPlatformResult> result;
@property(nonatomic, copy) void (^done)(HUXMediaLibraryRequest*);
@property(nonatomic, strong) HUXPresentationCoordinator* coordinator;
@property(nonatomic, strong) id<HUXPlatformCancellation> presentation;
@property(nonatomic, copy) void (^presentationFinished)(void);
@property(nonatomic, strong) PHPickerViewController* picker;
@property(nonatomic, strong) NSProgress* progress;
@property(nonatomic, strong) NSArray<PHPickerResult*>* selections;
@property(nonatomic, strong) NSMutableArray<HUXPlatformPayload*>* references;
@property(nonatomic, strong) HUXFileReference* saveReference;
@property(atomic) BOOL canceled;
@property(nonatomic) BOOL settled;
@property(nonatomic) BOOL dismissing;
@property(nonatomic) NSUInteger limit;
- (void)pickWithFilter:(NSInteger)filter limit:(NSUInteger)limit;
- (void)saveFile:(NSString*)path video:(BOOL)video;
- (void)fail:(NSString*)code message:(NSString*)message;
@end

@implementation HUXMediaLibraryRequest
- (void)retire {
  self.result = nil;
  self.progress = nil;
  self.selections = nil;
  self.references = nil;
  if (self.done) self.done(self);
  self.done = nil;
}
- (void)complete:(HUXPlatformPayload*)payload {
  if (self.settled || self.canceled) return;
  self.settled = YES;
  [self.result complete:payload];
  [self retire];
}
- (void)fail:(NSString*)code message:(NSString*)message {
  if (self.settled || self.canceled) return;
  self.settled = YES;
  [self.result failWithCode:code message:message ?: @"Platform media operation failed"
                   details:[HUXPlatformPayload nullValue]];
  [self retire];
}
- (void)releasePresentation {
  self.picker.delegate = nil;
  self.picker.presentationController.delegate = nil;
  self.picker = nil;
  self.presentation = nil;
  void (^finished)(void) = self.presentationFinished;
  self.presentationFinished = nil;
  if (finished) finished();
}
- (void)dismiss:(void (^)(void))completion {
  if (self.dismissing) return;
  self.dismissing = YES;
  if (self.picker.presentingViewController == nil) {
    [self releasePresentation];
    if (completion) completion();
    return;
  }
  [self.picker dismissViewControllerAnimated:YES completion:^{
    [self releasePresentation];
    if (completion) completion();
  }];
}
- (void)cancel {
  if (self.canceled) return;
  self.canceled = YES;
  [self.progress cancel];
  id<HUXPlatformCancellation> presentation = self.presentation;
  self.presentation = nil;
  // The coordinator may call cancel again while forwarding cancellation to the installed platform UI.
  [presentation cancel];
  [self dismiss:nil];
  [self retire];
}
- (void)pickWithFilter:(NSInteger)filter limit:(NSUInteger)limit {
  self.limit = limit;
  __weak HUXMediaLibraryRequest* weak = self;
  self.presentation = [self.coordinator enqueueWithStart:
      ^id<HUXPlatformCancellation>(UIViewController* presenter, void (^finished)(void)) {
        HUXMediaLibraryRequest* request = weak;
        if (request == nil || request.canceled) { finished(); return nil; }
        request.presentationFinished = finished;
        PHPickerConfiguration* configuration = [[PHPickerConfiguration alloc] init];
        configuration.selectionLimit = limit;
        configuration.preferredAssetRepresentationMode = PHPickerConfigurationAssetRepresentationModeCurrent;
        configuration.filter = filter == 0 ? PHPickerFilter.imagesFilter : filter == 1 ? PHPickerFilter.videosFilter :
            [PHPickerFilter anyFilterMatchingSubfilters:@[PHPickerFilter.imagesFilter, PHPickerFilter.videosFilter]];
        PHPickerViewController* picker = [[PHPickerViewController alloc] initWithConfiguration:configuration];
        request.picker = picker;
        picker.delegate = request;
        [presenter presentViewController:picker animated:YES completion:nil];
        picker.presentationController.delegate = request;
        if (picker.presentingViewController == nil) {
          [request releasePresentation];
          [request fail:@"media/unavailable" message:@"The platform media picker could not be presented"];
          return nil;
        }
        return request;
      } failure:^(NSError* error) {
        [weak fail:@"media/unavailable" message:error.localizedDescription];
      }];
}
- (void)presentationControllerDidDismiss:(UIPresentationController*)controller {
  [self releasePresentation];
  [self complete:[HUXPlatformPayload nullValue]];
}
- (void)picker:(PHPickerViewController*)picker didFinishPicking:(NSArray<PHPickerResult*>*)results {
  if (self.canceled || self.settled || self.dismissing) return;
  self.selections = results;
  [self dismiss:^{
    if (self.canceled || self.settled) return;
    if (results.count == 0) [self complete:[HUXPlatformPayload nullValue]];
    else if (results.count > self.limit) [self fail:@"media/io" message:@"The picker exceeded the selection limit"];
    else {
      self.references = [NSMutableArray array];
      [self loadNext];
    }
  }];
}
- (void)loadNext {
  if (self.canceled || self.settled) return;
  if (self.references.count == self.selections.count) {
    [self complete:[HUXPlatformPayload listValue:self.references]];
    return;
  }
  NSItemProvider* provider = self.selections[self.references.count].itemProvider;
  NSString* type = [provider hasItemConformingToTypeIdentifier:UTTypeMovie.identifier] ? UTTypeMovie.identifier :
      ([provider hasItemConformingToTypeIdentifier:UTTypeImage.identifier] ? UTTypeImage.identifier : nil);
  if (type == nil) { [self fail:@"media/unsupported" message:@"The selected item has no image or video representation"]; return; }
  __weak HUXMediaLibraryRequest* weak = self;
  self.progress = [provider loadFileRepresentationForTypeIdentifier:type completionHandler:^(NSURL* url, NSError* error) {
    HUXMediaLibraryRequest* request = weak;
    if (request == nil || request.canceled) return;
    NSError* copyError = error;
    HUXFileReference* reference = url != nil && error == nil ? [HUXFileReference copyFromFileURL:url error:&copyError] : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (request.canceled || request.settled) return;
      request.progress = nil;
      if (reference == nil) {
        NSString* code = [copyError.domain isEqual:NSCocoaErrorDomain] && copyError.code == NSFileReadNoPermissionError ?
            @"media/permission" : @"media/io";
        [request fail:code message:copyError.localizedDescription ?: @"The selected media could not be read"];
      } else {
        [request.references addObject:[HUXPlatformPayload fileReferenceValue:reference]];
        [request loadNext];
      }
    });
  }];
}
- (void)saveFile:(NSString*)path video:(BOOL)video {
  NSString* usage = [NSBundle.mainBundle objectForInfoDictionaryKey:@"NSPhotoLibraryAddUsageDescription"];
  if (![usage isKindOfClass:NSString.class] || usage.length == 0) {
    [self fail:@"media/unavailable" message:@"NSPhotoLibraryAddUsageDescription is required to save media"];
    return;
  }
  PHAuthorizationStatus status = [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelAddOnly];
  if (status == PHAuthorizationStatusNotDetermined) {
    __weak HUXMediaLibraryRequest* weak = self;
    [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelAddOnly handler:^(PHAuthorizationStatus granted) {
      dispatch_async(dispatch_get_main_queue(), ^{ [weak saveAuthorized:granted path:path video:video]; });
    }];
  } else [self saveAuthorized:status path:path video:video];
}
- (void)saveAuthorized:(PHAuthorizationStatus)status path:(NSString*)path video:(BOOL)video {
  if (self.canceled || self.settled) return;
  if (status != PHAuthorizationStatusAuthorized && status != PHAuthorizationStatusLimited) {
    [self fail:@"media/permission" message:@"Permission to add media to Photos was denied"];
    return;
  }
  __weak HUXMediaLibraryRequest* weak = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    HUXMediaLibraryRequest* request = weak;
    if (request == nil || request.canceled) return;
    NSError* error = nil;
    HUXFileReference* reference = [HUXFileReference copyFromFileURL:[NSURL fileURLWithPath:path] error:&error];
    dispatch_async(dispatch_get_main_queue(), ^{
      if (request.canceled || request.settled) return;
      if (reference == nil) {
        [request fail:([error.domain isEqual:NSCocoaErrorDomain] && error.code == NSFileReadNoPermissionError ?
            @"media/permission" : @"media/invalid-source") message:error.localizedDescription];
        return;
      }
      request.saveReference = reference;
      // The request retains the staged source until Photos completes, including after Task cancellation.
      [PHPhotoLibrary.sharedPhotoLibrary performChanges:^{
        PHAssetCreationRequest* asset = [PHAssetCreationRequest creationRequestForAsset];
        PHAssetResourceCreationOptions* options = [[PHAssetResourceCreationOptions alloc] init];
        options.shouldMoveFile = NO;
        [asset addResourceWithType:video ? PHAssetResourceTypeVideo : PHAssetResourceTypePhoto
                          fileURL:reference.fileURL options:options];
      } completionHandler:^(BOOL success, NSError* saveError) {
        dispatch_async(dispatch_get_main_queue(), ^{
          request.saveReference = nil;
          if (success) [request complete:[HUXPlatformPayload nullValue]];
          else [request fail:@"media/io" message:saveError.localizedDescription ?: @"Photos did not publish the media"];
        });
      }];
    });
  });
}
@end

@interface HUXMediaLibraryModule : NSObject <HUXPlatformModule>
@property(nonatomic, strong) HUXPresentationCoordinator* coordinator;
@property(nonatomic, strong) NSMutableSet<HUXMediaLibraryRequest*>* requests;
@property(nonatomic) BOOL closed;
@end

@implementation HUXMediaLibraryModule
- (id<HUXPlatformCancellation>)invoke:(NSString*)method arguments:(HUXPlatformPayload*)arguments
                              result:(id<HUXPlatformResult>)result {
  if (self.closed) {
    [result failWithCode:@"media/unavailable" message:@"Media library session is closed" details:[HUXPlatformPayload nullValue]];
    return nil;
  }
  if ([method isEqual:@"capabilities"]) {
    [result complete:[HUXPlatformPayload objectValue:@{
      @"maxSelectionCount": [HUXPlatformPayload integerValue:INT32_MAX],
      @"canSave": [HUXPlatformPayload booleanValue:YES],
    }]];
    return nil;
  }
  HUXMediaLibraryRequest* request = [[HUXMediaLibraryRequest alloc] init];
  request.result = result;
  request.coordinator = self.coordinator;
  __weak HUXMediaLibraryModule* weak = self;
  request.done = ^(HUXMediaLibraryRequest* finished) { [weak.requests removeObject:finished]; };
  [self.requests addObject:request];
  if ([method isEqual:@"pick"]) {
    int64_t limit = [arguments field:@"limit"].integerValue;
    if (limit <= 0 || limit > INT32_MAX) [request fail:@"media/unsupported" message:@"Unsupported selection limit"];
    else [request pickWithFilter:[arguments field:@"filter"].integerValue limit:static_cast<NSUInteger>(limit)];
  } else if ([method isEqual:@"save"]) {
    [request saveFile:[arguments field:@"path"].stringValue video:[arguments field:@"video"].booleanValue];
  } else [request fail:@"media/unsupported" message:@"Unknown media library operation"];
  return request;
}
- (void)dispose {
  self.closed = YES;
  for (HUXMediaLibraryRequest* request in self.requests.allObjects) [request cancel];
  [self.requests removeAllObjects];
}
@end

@interface HUXMediaLibraryFactory : NSObject <HUXUIKitWindowPlatformModuleFactory>
@end
@implementation HUXMediaLibraryFactory
- (id<HUXPlatformModule>)createWithViewController:(UIViewController*)owner options:(HUXPlatformPayload*)options
                                         events:(id<HUXPlatformEventEmitter>)events {
  HUXMediaLibraryModule* module = [[HUXMediaLibraryModule alloc] init];
  module.coordinator = HUXGetPresentationCoordinator(owner);
  module.requests = [NSMutableSet set];
  return module;
}
@end

namespace huxerui::media::detail {
void InstallMediaLibrary(ApplicationContext& context) {
  ios::ObjectiveCWindowPlatformModuleFactory<PlatformChannel> factory;
  factory.factory = [[HUXMediaLibraryFactory alloc] init];
  factory.create = [](PlatformChannel channel) { return channel; };
  context.RegisterPlatformModule<PlatformChannel>(media_library_type, std::move(factory));
}
} // namespace huxerui::media::detail
