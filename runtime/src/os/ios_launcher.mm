// The iPhone / iPad start screen (ios_launcher.h). The SwiftUI view (runtime/ios/Launcher.swift, class DRLauncher)
// is reached through the Objective-C runtime, so this file needs no generated Swift header.

#import <UIKit/UIKit.h>
#import <objc/message.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include "ios_launcher.h"
#include <os/logger.h>
#include <user/config.h>
#include <fmt/core.h>

bool Launcher::Run(const std::filesystem::path& documents, std::filesystem::path& gamePath)
{
    Class launcher = NSClassFromString(@"DRLauncher");
    if (launcher == nil)
        return false;

    NSDictionary* settings = @{
        @"frameRate": @(Config::FrameRate.load()),
        @"resolutionScale": @(double(Config::ResolutionScale.load())),
        @"msaa": @(Config::MSAA.load()),
        @"fxaa": @(Config::FXAA.load()),
        @"motionBlur": @(Config::MotionBlur.load()),
        @"vsync": @(Config::VSync.load()),
        @"showFps": @(Config::ShowFps.load()),
        @"screenScaling": @(Config::ScreenScaling.load()),
    };
    NSString* documentsPath = [NSString stringWithUTF8String:(const char*)documents.u8string().c_str()];

    __block NSDictionary* chosen = nil;
    void (^onPlay)(NSDictionary*) = ^(NSDictionary* values) { chosen = values; };
    UIViewController* controller = ((UIViewController* (*)(id, SEL, NSDictionary*, NSString*, id))objc_msgSend)(
        launcher, NSSelectorFromString(@"makeWithSettings:documents:onPlay:"), settings, documentsPath, onPlay);
    if (controller == nil)
        return false;

    // A window of its own above SDL's launch screen; the game's window replaces it after Play.
    UIWindow* window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    window.windowLevel = UIWindowLevelNormal + 1;
    window.rootViewController = controller;
    [window makeKeyAndVisible];
    LOGN("[launcher] start screen shown");

    // DRMain runs on the main thread from SDL's launch: turn the run loop here until Play.
    while (chosen == nil)
    {
        @autoreleasepool
        {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        }
    }

    Config::FrameRate = [chosen[@"frameRate"] intValue] >= 60 ? 60 : 30;
    Config::ResolutionScale = std::min(float([chosen[@"resolutionScale"] doubleValue]), Config::kMaxResolutionScale);
    Config::MSAA = [chosen[@"msaa"] boolValue];
    Config::FXAA = [chosen[@"fxaa"] boolValue];
    Config::MotionBlur = [chosen[@"motionBlur"] boolValue];
    Config::VSync = [chosen[@"vsync"] boolValue];
    Config::ShowFps = [chosen[@"showFps"] boolValue];
    Config::ScreenScaling = std::max(0, std::min(2, [chosen[@"screenScaling"] intValue]));
    if (NSString* folder = chosen[@"gameFolder"])
        gamePath = std::filesystem::path([folder UTF8String]);
    Config::Save();
    LOGN("[launcher] Play: settings saved");

    window.hidden = YES;
    window.rootViewController = nil;
    return true;
}
