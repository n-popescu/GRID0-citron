// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <iostream>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include "video_core/renderer_metal/metal_runtime.h"

int main() {
    @autoreleasepool {
        try {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            auto window = [[NSWindow alloc]
                initWithContentRect:NSMakeRect(0, 0, 640, 400)
                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                    NSWindowStyleMaskResizable
                            backing:NSBackingStoreBuffered
                              defer:NO];
            window.releasedWhenClosed = NO;
            window.title = @"Citrosis native Metal presentation test";
            auto layer = [CAMetalLayer layer];
            window.contentView.wantsLayer = YES;
            window.contentView.layer = layer;
            layer.framebufferOnly = YES;
            [window center];
            [window makeKeyAndOrderFront:nil];
            [NSApp finishLaunching];
            NativeMetal::Runtime runtime;
            auto texture = runtime.CreateTexture(256, 256, NativeMetal::PixelFormat::RGBA8,
                                                 NativeMetal::TextureUsage::Sample |
                                                     NativeMetal::TextureUsage::RenderTarget);
            const std::array attachments{
                NativeMetal::ColorAttachment{texture, true, {.1, .6, .8, 1}}};
            auto commands = runtime.BeginCommands("Native Metal presentation test pattern");
            commands.BeginRender(attachments);
            commands.Submit().Wait();
            size_t presented = 0;
            for (size_t frame = 0; frame < 90; ++frame) {
                NSEvent* event;
                while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                   untilDate:[NSDate distantPast]
                                                      inMode:NSDefaultRunLoopMode
                                                     dequeue:YES]))
                    [NSApp sendEvent:event];
                [NSApp updateWindows];
                layer.contentsScale = window.backingScaleFactor;
                layer.drawableSize =
                    CGSizeMake(window.contentView.bounds.size.width * layer.contentsScale,
                               window.contentView.bounds.size.height * layer.contentsScale);
                auto submission = runtime.Present((__bridge void*)layer, texture);
                if (submission) {
                    submission.Wait();
                    ++presented;
                }
            }
            [window close];
            if (presented != 90)
                throw std::runtime_error("Native Metal drawable acquisition failed");
            std::cout << "PASS: 90 CAMetalLayer drawable presentations on " << runtime.DeviceName()
                      << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
}
