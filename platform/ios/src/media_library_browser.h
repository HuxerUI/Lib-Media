#pragma once

#import <huxerui/ios/platform_registry.h>

@interface HUXMediaLibraryBrowser : NSObject
- (instancetype)initWithCoordinator:(HUXPresentationCoordinator*)coordinator
                             events:(id<HUXPlatformEventEmitter>)events;
- (id<HUXPlatformCancellation>)invoke:(NSString*)method arguments:(HUXPlatformPayload*)arguments
                              result:(id<HUXPlatformResult>)result;
- (void)dispose;
@end
