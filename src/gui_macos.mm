#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <cstring>

@interface ChordAnalyzerView : NSView
@property(nonatomic, strong) NSString* chordText;
@end

@implementation ChordAnalyzerView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _chordText = @"—";
        self.wantsLayer = YES;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque  { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    NSRect b = self.bounds;

    [[NSColor colorWithCalibratedRed:0.09 green:0.09 blue:0.11 alpha:1.0] setFill];
    NSRectFill(b);

    NSMutableParagraphStyle* centered = [[NSMutableParagraphStyle alloc] init];
    centered.alignment = NSTextAlignmentCenter;

    NSDictionary* labelAttrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.55 alpha:1.0],
        NSParagraphStyleAttributeName: centered,
    };
    NSRect labelRect = NSMakeRect(0, 16, b.size.width, 18);
    [@"Chord" drawInRect:labelRect withAttributes:labelAttrs];

    CGFloat fontSize = 68.0;
    NSFont* chordFont = [NSFont systemFontOfSize:fontSize weight:NSFontWeightBold];

    NSString* text = self.chordText.length ? self.chordText : @"—";

    NSDictionary* chordAttrs = @{
        NSFontAttributeName: chordFont,
        NSForegroundColorAttributeName: [NSColor whiteColor],
        NSParagraphStyleAttributeName: centered,
    };

    NSSize textSize = [text sizeWithAttributes:chordAttrs];
    while (textSize.width > b.size.width - 24 && fontSize > 18) {
        fontSize -= 4;
        chordFont = [NSFont systemFontOfSize:fontSize weight:NSFontWeightBold];
        chordAttrs = @{
            NSFontAttributeName: chordFont,
            NSForegroundColorAttributeName: [NSColor whiteColor],
            NSParagraphStyleAttributeName: centered,
        };
        textSize = [text sizeWithAttributes:chordAttrs];
    }

    NSRect textRect = NSMakeRect(12,
                                 (b.size.height - textSize.height) * 0.5 + 8,
                                 b.size.width - 24,
                                 textSize.height);
    [text drawInRect:textRect withAttributes:chordAttrs];
}

- (void)updateChord:(NSString*)newText {
    if (![newText isEqualToString:self.chordText]) {
        self.chordText = [newText copy];
        [self setNeedsDisplay:YES];
    }
}

@end

extern "C" {

void* chordGuiCreate(uint32_t w, uint32_t h) {
    @autoreleasepool {
        NSRect frame = NSMakeRect(0, 0, w, h);
        ChordAnalyzerView* view = [[ChordAnalyzerView alloc] initWithFrame:frame];
        return (__bridge_retained void*)view;
    }
}

void chordGuiDestroy(void* view) {
    if (!view) return;
    @autoreleasepool {
        ChordAnalyzerView* v = (__bridge_transfer ChordAnalyzerView*)view;
        [v removeFromSuperview];
    }
}

bool chordGuiSetParent(void* view, void* parentNSView) {
    if (!view || !parentNSView) return false;
    @autoreleasepool {
        ChordAnalyzerView* v = (__bridge ChordAnalyzerView*)view;
        NSView* parent = (__bridge NSView*)parentNSView;

        void (^attach)(void) = ^{
            if (v.superview != parent) {
                [v removeFromSuperview];
                v.frame = parent.bounds;
                v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
                [parent addSubview:v];
            }
            [v setNeedsDisplay:YES];
        };

        if ([NSThread isMainThread]) {
            attach();
        } else {
            dispatch_sync(dispatch_get_main_queue(), attach);
        }
        return true;
    }
}

void chordGuiSetText(void* view, const char* text) {
    if (!view) return;
    @autoreleasepool {
        ChordAnalyzerView* v = (__bridge ChordAnalyzerView*)view;
        NSString* s = [NSString stringWithUTF8String:(text ? text : "—")];
        if ([NSThread isMainThread]) {
            [v updateChord:s];
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{
                [v updateChord:s];
            });
        }
    }
}

void chordGuiGetPreferredSize(uint32_t* w, uint32_t* h) {
    if (w) *w = 380;
    if (h) *h = 200;
}

} // extern "C"
