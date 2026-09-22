#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <cstring>

typedef void (*ChordProvider)(void* userData, char* buf, size_t bufSize);

@interface ChordAnalyzerView : NSView
@property(nonatomic, strong) NSTextField* subtitleLabel;
@property(nonatomic, strong) NSTextField* chordLabel;
@property(nonatomic, strong) NSTimer* pollTimer;
@property(nonatomic, copy)   NSString* lastText;
@property(nonatomic, assign) ChordProvider provider;
@property(nonatomic, assign) void* providerUserData;
@end

@implementation ChordAnalyzerView

+ (NSTextField*)makeLabelWithFont:(NSFont*)font color:(NSColor*)color {
    NSTextField* tf = [[NSTextField alloc] initWithFrame:NSZeroRect];
    tf.editable = NO;
    tf.selectable = NO;
    tf.bezeled = NO;
    tf.bordered = NO;
    tf.drawsBackground = NO;
    tf.alignment = NSTextAlignmentCenter;
    tf.font = font;
    tf.textColor = color;
    tf.usesSingleLineMode = YES;
    tf.cell.wraps = NO;
    tf.cell.scrollable = NO;
    tf.lineBreakMode = NSLineBreakByClipping;
    return tf;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        self.layer.backgroundColor =
            [NSColor colorWithCalibratedRed:0.09 green:0.09 blue:0.11 alpha:1.0].CGColor;

        _subtitleLabel = [ChordAnalyzerView
            makeLabelWithFont:[NSFont systemFontOfSize:12 weight:NSFontWeightMedium]
                        color:[NSColor colorWithCalibratedWhite:0.55 alpha:1.0]];
        _subtitleLabel.stringValue = @"Chord";
        [self addSubview:_subtitleLabel];

        _chordLabel = [ChordAnalyzerView
            makeLabelWithFont:[NSFont systemFontOfSize:64 weight:NSFontWeightBold]
                        color:[NSColor whiteColor]];
        _chordLabel.stringValue = @"—";
        [self addSubview:_chordLabel];

        _lastText = @"—";
        [self relayout];
    }
    return self;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self relayout];
}

- (void)relayout {
    NSRect b = self.bounds;
    self.subtitleLabel.frame = NSMakeRect(0, b.size.height - 32, b.size.width, 18);
    CGFloat chordH = 96;
    self.chordLabel.frame = NSMakeRect(12,
                                       (b.size.height - chordH) * 0.5,
                                       b.size.width - 24,
                                       chordH);
    [self resizeChordFontForText:self.chordLabel.stringValue];
}

- (void)resizeChordFontForText:(NSString*)text {
    CGFloat fontSize = 68.0;
    CGFloat maxW = self.chordLabel.frame.size.width - 8;
    NSFont* font = [NSFont systemFontOfSize:fontSize weight:NSFontWeightBold];
    NSSize sz = [text sizeWithAttributes:@{NSFontAttributeName: font}];
    while (sz.width > maxW && fontSize > 16) {
        fontSize -= 4;
        font = [NSFont systemFontOfSize:fontSize weight:NSFontWeightBold];
        sz = [text sizeWithAttributes:@{NSFontAttributeName: font}];
    }
    self.chordLabel.font = font;
}

- (void)startPollingWithProvider:(ChordProvider)provider userData:(void*)userData {
    self.provider = provider;
    self.providerUserData = userData;
    if (self.pollTimer) return;
    __weak ChordAnalyzerView* weakSelf = self;
    self.pollTimer = [NSTimer scheduledTimerWithTimeInterval:0.08
                                                     repeats:YES
                                                       block:^(NSTimer* _Nonnull t) {
        (void)t;
        [weakSelf pollTick];
    }];
}

- (void)pollTick {
    if (!self.provider) return;
    char buf[64] = {0};
    self.provider(self.providerUserData, buf, sizeof(buf));
    NSString* s = [NSString stringWithUTF8String:buf];
    if (s.length == 0) s = @"—";
    if ([s isEqualToString:self.lastText]) return;
    self.lastText = s;
    self.chordLabel.stringValue = s;
    [self resizeChordFontForText:s];
}

- (void)dealloc {
    [_pollTimer invalidate];
}

@end

extern "C" {

void* chordGuiCreate(uint32_t w, uint32_t h,
                     ChordProvider provider, void* userData) {
    @autoreleasepool {
        NSRect frame = NSMakeRect(0, 0, w, h);
        ChordAnalyzerView* view = [[ChordAnalyzerView alloc] initWithFrame:frame];
        [view startPollingWithProvider:provider userData:userData];
        return (__bridge_retained void*)view;
    }
}

void chordGuiDestroy(void* view) {
    if (!view) return;
    @autoreleasepool {
        ChordAnalyzerView* v = (__bridge_transfer ChordAnalyzerView*)view;
        [v.pollTimer invalidate];
        v.pollTimer = nil;
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
            [v relayout];
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

void chordGuiGetPreferredSize(uint32_t* w, uint32_t* h) {
    if (w) *w = 380;
    if (h) *h = 200;
}

} // extern "C"
