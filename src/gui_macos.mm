#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "gui_api.h"
#include "scale.hpp"

static const int kRangeStart = 36; // C2
static const int kRangeEnd   = 83; // B5

static inline bool isWhitePc(int pc) {
    switch (pc) {
        case 0: case 2: case 4: case 5: case 7: case 9: case 11: return true;
        default: return false;
    }
}

// ------------------------------------------------------------
// Piano keyboard subview
// ------------------------------------------------------------

@interface KeyboardView : NSView
@property(nonatomic, strong) NSMutableIndexSet* held;
@property(nonatomic, assign) BOOL chordRecognized;
@property(nonatomic, assign) uint16_t scaleMask; // bit per pitch class, 0 = no scale
@property(nonatomic, assign) int scaleRoot;
@end

@implementation KeyboardView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _held = [NSMutableIndexSet indexSet];
        _chordRecognized = NO;
        self.wantsLayer = YES;
        self.layer.backgroundColor =
            [NSColor colorWithCalibratedRed:0.09 green:0.09 blue:0.11 alpha:1.0].CGColor;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)updateHeld:(const uint8_t*)notes count:(NSUInteger)count chord:(BOOL)chord {
    NSMutableIndexSet* next = [NSMutableIndexSet indexSet];
    for (NSUInteger i = 0; i < count; ++i) [next addIndex:notes[i]];
    BOOL changed = ![next isEqual:self.held] || chord != self.chordRecognized;
    if (changed) {
        self.held = next;
        self.chordRecognized = chord;
        [self setNeedsDisplay:YES];
    }
}

- (void)updateScaleMask:(uint16_t)mask root:(int)root {
    if (mask == self.scaleMask && root == self.scaleRoot) return;
    self.scaleMask = mask;
    self.scaleRoot = root;
    [self setNeedsDisplay:YES];
}

- (BOOL)inScale:(int)pc {
    return (self.scaleMask >> pc) & 1;
}

- (void)drawRootMarkerInRect:(NSRect)r color:(NSColor*)color {
    CGFloat d = std::min<CGFloat>(r.size.width * 0.32, 6.0);
    NSRect dot = NSMakeRect(NSMidX(r) - d * 0.5, NSMaxY(r) - d - 5, d, d);
    [color setFill];
    [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect b = self.bounds;

    int whiteCount = 0;
    for (int n = kRangeStart; n <= kRangeEnd; ++n) {
        if (isWhitePc(n % 12)) whiteCount++;
    }

    CGFloat whiteW = b.size.width / (CGFloat)whiteCount;
    CGFloat whiteH = b.size.height;
    CGFloat blackW = whiteW * 0.62;
    CGFloat blackH = whiteH * 0.62;

    NSColor* whiteFill  = [NSColor colorWithCalibratedRed:0.93 green:0.93 blue:0.94 alpha:1.0];
    NSColor* blackFill  = [NSColor colorWithCalibratedRed:0.09 green:0.09 blue:0.12 alpha:1.0];
    NSColor* separator  = [NSColor colorWithCalibratedWhite:0.35 alpha:1.0];
    NSColor* octaveMark = [NSColor colorWithCalibratedWhite:0.55 alpha:1.0];

    // Soft green tint for keys that belong to the selected scale.
    NSColor* whiteScale = [NSColor colorWithCalibratedRed:0.72 green:0.90 blue:0.78 alpha:1.0];
    NSColor* blackScale = [NSColor colorWithCalibratedRed:0.18 green:0.42 blue:0.30 alpha:1.0];
    NSColor* rootMarkOnWhite = [NSColor colorWithCalibratedRed:0.10 green:0.45 blue:0.25 alpha:1.0];
    NSColor* rootMarkOnBlack = [NSColor colorWithCalibratedRed:0.70 green:0.95 blue:0.80 alpha:1.0];

    // Blue when a chord is recognized, amber when it's just individual notes.
    NSColor* whiteHeld;
    NSColor* blackHeld;
    if (self.chordRecognized) {
        whiteHeld = [NSColor colorWithCalibratedRed:0.32 green:0.74 blue:1.00 alpha:1.0];
        blackHeld = [NSColor colorWithCalibratedRed:0.20 green:0.55 blue:0.85 alpha:1.0];
    } else {
        whiteHeld = [NSColor colorWithCalibratedRed:1.00 green:0.72 blue:0.24 alpha:1.0];
        blackHeld = [NSColor colorWithCalibratedRed:0.75 green:0.52 blue:0.14 alpha:1.0];
    }

    // White keys pass
    int wi = 0;
    for (int n = kRangeStart; n <= kRangeEnd; ++n) {
        int pc = n % 12;
        if (!isWhitePc(pc)) continue;

        NSRect r = NSMakeRect(wi * whiteW, 0, whiteW, whiteH);
        BOOL held = [self.held containsIndex:(NSUInteger)n];
        BOOL inScale = [self inScale:pc];
        [(held ? whiteHeld : inScale ? whiteScale : whiteFill) setFill];
        NSRectFill(r);
        if (inScale && pc == self.scaleRoot) {
            // Sit the marker above the octave label so they don't collide.
            NSRect markerArea = r;
            if (pc == 0) markerArea.size.height -= 12;
            [self drawRootMarkerInRect:markerArea color:rootMarkOnWhite];
        }

        // Subtle "C" marker under each C key
        if (pc == 0) {
            NSString* label = [NSString stringWithFormat:@"C%d", (n / 12) - 1];
            NSDictionary* attrs = @{
                NSFontAttributeName: [NSFont systemFontOfSize:8 weight:NSFontWeightMedium],
                NSForegroundColorAttributeName: octaveMark,
            };
            NSSize sz = [label sizeWithAttributes:attrs];
            NSPoint p = NSMakePoint(r.origin.x + (whiteW - sz.width) * 0.5,
                                    r.origin.y + whiteH - sz.height - 3);
            [label drawAtPoint:p withAttributes:attrs];
        }
        wi++;
    }

    // Thin separators between white keys, aligned to the pixel grid
    [separator setStroke];
    NSBezierPath* seps = [NSBezierPath bezierPath];
    [seps setLineWidth:1.0];
    for (int i = 1; i < whiteCount; ++i) {
        CGFloat x = std::round(i * whiteW) + 0.5;
        [seps moveToPoint:NSMakePoint(x, 0)];
        [seps lineToPoint:NSMakePoint(x, whiteH)];
    }
    [seps stroke];

    // Black keys pass (drawn on top)
    wi = 0;
    for (int n = kRangeStart; n <= kRangeEnd; ++n) {
        int pc = n % 12;
        if (isWhitePc(pc)) { wi++; continue; }

        CGFloat x = wi * whiteW - blackW * 0.5;
        NSRect r = NSMakeRect(x, 0, blackW, blackH);
        BOOL held = [self.held containsIndex:(NSUInteger)n];
        BOOL inScale = [self inScale:pc];
        [(held ? blackHeld : inScale ? blackScale : blackFill) setFill];
        NSRectFill(r);
        if (inScale && pc == self.scaleRoot) {
            [self drawRootMarkerInRect:r color:rootMarkOnBlack];
        }
    }
}

@end

// ------------------------------------------------------------
// Container view: label stack + keyboard + polling timer
// ------------------------------------------------------------

@interface ChordAnalyzerView : NSView
@property(nonatomic, strong) NSTextField* subtitleLabel;
@property(nonatomic, strong) NSTextField* chordLabel;
@property(nonatomic, strong) KeyboardView* keyboardView;
@property(nonatomic, strong) NSTextField* scaleLabel;
@property(nonatomic, strong) NSPopUpButton* rootPopup;
@property(nonatomic, strong) NSPopUpButton* scaleTypePopup;
@property(nonatomic, assign) ScaleChangedCallback scaleCallback;
@property(nonatomic, strong) NSTimer* pollTimer;
@property(nonatomic, copy)   NSString* lastChordText;
@property(nonatomic, assign) ChordSnapshotProvider provider;
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

- (BOOL)isFlipped { return YES; }

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        self.layer.backgroundColor =
            [NSColor colorWithCalibratedRed:0.09 green:0.09 blue:0.11 alpha:1.0].CGColor;
        // Popups should render in dark style regardless of the host's appearance.
        self.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

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

        _keyboardView = [[KeyboardView alloc] initWithFrame:NSZeroRect];
        [self addSubview:_keyboardView];

        _scaleLabel = [ChordAnalyzerView
            makeLabelWithFont:[NSFont systemFontOfSize:12 weight:NSFontWeightMedium]
                        color:[NSColor colorWithCalibratedWhite:0.55 alpha:1.0]];
        _scaleLabel.alignment = NSTextAlignmentLeft;
        _scaleLabel.stringValue = @"Scale";
        [self addSubview:_scaleLabel];

        _rootPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _rootPopup.controlSize = NSControlSizeSmall;
        _rootPopup.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
        for (const char* rootName : scale::kRootNames) {
            [_rootPopup addItemWithTitle:[NSString stringWithUTF8String:rootName]];
        }
        _rootPopup.target = self;
        _rootPopup.action = @selector(scaleSelectionChanged:);
        [self addSubview:_rootPopup];

        _scaleTypePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _scaleTypePopup.controlSize = NSControlSizeSmall;
        _scaleTypePopup.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
        for (const scale::Type& t : scale::kTypes) {
            [_scaleTypePopup addItemWithTitle:[NSString stringWithUTF8String:t.name]];
        }
        _scaleTypePopup.target = self;
        _scaleTypePopup.action = @selector(scaleSelectionChanged:);
        [self addSubview:_scaleTypePopup];

        [self applyScaleRoot:0 type:0];

        _lastChordText = @"—";
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
    CGFloat pad = 10;
    CGFloat kbH = 92;
    CGFloat rowH = 22;
    CGFloat rowGap = 8;

    self.subtitleLabel.frame = NSMakeRect(0, pad, b.size.width, 16);

    CGFloat chordY = pad + 22;
    CGFloat chordH = b.size.height - chordY - kbH - rowH - rowGap - pad * 2;
    if (chordH < 40) chordH = 40;
    self.chordLabel.frame = NSMakeRect(12, chordY, b.size.width - 24, chordH);
    [self resizeChordFontForText:self.chordLabel.stringValue];

    CGFloat kbY = b.size.height - kbH - pad;
    CGFloat rowY = kbY - rowGap - rowH;
    self.scaleLabel.frame     = NSMakeRect(pad, rowY + 3, 40, 16);
    self.rootPopup.frame      = NSMakeRect(pad + 40, rowY, 84, rowH);
    self.scaleTypePopup.frame = NSMakeRect(pad + 40 + 84 + 6, rowY, 140, rowH);

    self.keyboardView.frame = NSMakeRect(pad, kbY, b.size.width - pad * 2, kbH);
}

- (void)applyScaleRoot:(int)root type:(int)type {
    if (self.rootPopup.indexOfSelectedItem != root) [self.rootPopup selectItemAtIndex:root];
    if (self.scaleTypePopup.indexOfSelectedItem != type) [self.scaleTypePopup selectItemAtIndex:type];
    self.rootPopup.enabled = type != 0;
    [self.keyboardView updateScaleMask:scale::pitchClassMask(root, type) root:root];
}

- (void)scaleSelectionChanged:(id)sender {
    (void)sender;
    int root = (int)self.rootPopup.indexOfSelectedItem;
    int type = (int)self.scaleTypePopup.indexOfSelectedItem;
    if (root < 0 || type < 0) return;
    [self applyScaleRoot:root type:type];
    if (self.scaleCallback) {
        self.scaleCallback(self.providerUserData, (uint8_t)root, (uint8_t)type);
    }
}

- (void)resizeChordFontForText:(NSString*)text {
    CGFloat fontSize = 72.0;
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

- (void)startPollingWithProvider:(ChordSnapshotProvider)provider userData:(void*)userData {
    self.provider = provider;
    self.providerUserData = userData;
    if (self.pollTimer) return;
    __weak ChordAnalyzerView* weakSelf = self;
    self.pollTimer = [NSTimer scheduledTimerWithTimeInterval:0.06
                                                     repeats:YES
                                                       block:^(NSTimer* _Nonnull t) {
        (void)t;
        [weakSelf pollTick];
    }];
}

- (void)pollTick {
    if (!self.provider) return;
    ChordSnapshot snap;
    std::memset(&snap, 0, sizeof(snap));
    self.provider(self.providerUserData, &snap);

    NSString* s = [NSString stringWithUTF8String:snap.name];
    if (s.length == 0) s = @"—";
    BOOL isChord = snap.isChord != 0;

    if (![s isEqualToString:self.lastChordText]) {
        self.lastChordText = s;
        self.chordLabel.stringValue = s;
        [self resizeChordFontForText:s];
    }

    if (snap.noteCount == 0) {
        self.chordLabel.textColor = [NSColor whiteColor];
    } else if (isChord) {
        self.chordLabel.textColor =
            [NSColor colorWithCalibratedRed:0.60 green:0.85 blue:1.00 alpha:1.0];
    } else {
        self.chordLabel.textColor =
            [NSColor colorWithCalibratedRed:1.00 green:0.82 blue:0.45 alpha:1.0];
    }

    [self.keyboardView updateHeld:snap.notes count:snap.noteCount chord:isChord];

    // Plugin state is the source of truth (e.g. after the host loads a project).
    if (snap.scaleRoot <= 11 && snap.scaleType < scale::kTypeCount) {
        [self applyScaleRoot:snap.scaleRoot type:snap.scaleType];
    }
}

- (void)dealloc {
    [_pollTimer invalidate];
}

@end

// ------------------------------------------------------------
// C entry points
// ------------------------------------------------------------

extern "C" {

void* chordGuiCreate(uint32_t w, uint32_t h,
                     ChordSnapshotProvider provider,
                     ScaleChangedCallback onScaleChanged,
                     void* userData) {
    @autoreleasepool {
        NSRect frame = NSMakeRect(0, 0, w, h);
        ChordAnalyzerView* view = [[ChordAnalyzerView alloc] initWithFrame:frame];
        view.scaleCallback = onScaleChanged;
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
    if (w) *w = 440;
    if (h) *h = 310;
}

} // extern "C"
