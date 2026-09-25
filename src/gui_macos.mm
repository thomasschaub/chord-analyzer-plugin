#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "gui_api.h"
#include "midi_file.hpp"
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
    NSColor* blackOutline = [NSColor colorWithCalibratedRed:0.05 green:0.05 blue:0.07 alpha:1.0];

    // Soft green tint for keys that belong to the selected scale.
    NSColor* whiteScale = [NSColor colorWithCalibratedRed:0.72 green:0.90 blue:0.78 alpha:1.0];
    NSColor* blackScale = [NSColor colorWithCalibratedRed:0.18 green:0.42 blue:0.30 alpha:1.0];
    // The scale root gets a stronger green plus a dot marker.
    NSColor* whiteRoot  = [NSColor colorWithCalibratedRed:0.36 green:0.78 blue:0.52 alpha:1.0];
    NSColor* blackRoot  = [NSColor colorWithCalibratedRed:0.42 green:0.93 blue:0.60 alpha:1.0];
    NSColor* rootMark   = [NSColor colorWithCalibratedRed:0.04 green:0.24 blue:0.13 alpha:1.0];

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
        BOOL isRoot = inScale && pc == self.scaleRoot;
        [(held ? whiteHeld : isRoot ? whiteRoot : inScale ? whiteScale : whiteFill) setFill];
        NSRectFill(r);
        if (isRoot) {
            // Sit the marker above the octave label so they don't collide.
            NSRect markerArea = r;
            if (pc == 0) markerArea.size.height -= 12;
            [self drawRootMarkerInRect:markerArea color:rootMark];
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

        // Pixel-align so the 1pt outline stays crisp.
        CGFloat x = std::round(wi * whiteW - blackW * 0.5);
        NSRect r = NSMakeRect(x, 0, std::round(blackW), std::round(blackH));
        BOOL held = [self.held containsIndex:(NSUInteger)n];
        BOOL inScale = [self inScale:pc];
        BOOL isRoot = inScale && pc == self.scaleRoot;
        [(held ? blackHeld : isRoot ? blackRoot : inScale ? blackScale : blackFill) setFill];
        NSRectFill(r);

        // Dark contour so a highlighted black key doesn't blend into
        // highlighted white keys next to it.
        [blackOutline setFill];
        NSFrameRect(r);

        if (isRoot) {
            [self drawRootMarkerInRect:r color:rootMark];
        }
    }
}

@end

// ------------------------------------------------------------
// Pitch-class histogram subview
// ------------------------------------------------------------

@interface HistogramView : NSView
@end

@implementation HistogramView {
    uint16_t _counts[12];
    uint16_t _scaleMask;
    int _scaleRoot;
}

- (BOOL)isFlipped { return YES; }

- (void)updateCounts:(const uint16_t*)counts scaleMask:(uint16_t)mask root:(int)root {
    if (std::memcmp(counts, _counts, sizeof(_counts)) == 0
        && mask == _scaleMask && root == _scaleRoot) {
        return;
    }
    std::memcpy(_counts, counts, sizeof(_counts));
    _scaleMask = mask;
    _scaleRoot = root;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect b = self.bounds;

    NSColor* panel     = [NSColor colorWithCalibratedRed:0.13 green:0.13 blue:0.16 alpha:1.0];
    NSColor* barPlain  = [NSColor colorWithCalibratedWhite:0.42 alpha:1.0];
    NSColor* barScale  = [NSColor colorWithCalibratedRed:0.72 green:0.90 blue:0.78 alpha:1.0];
    NSColor* barRoot   = [NSColor colorWithCalibratedRed:0.42 green:0.93 blue:0.60 alpha:1.0];
    NSColor* labelDim  = [NSColor colorWithCalibratedWhite:0.50 alpha:1.0];
    NSColor* labelLit  = [NSColor colorWithCalibratedWhite:0.85 alpha:1.0];

    [panel setFill];
    [[NSBezierPath bezierPathWithRoundedRect:b xRadius:6 yRadius:6] fill];

    uint16_t maxCount = 0;
    for (uint16_t c : _counts) maxCount = std::max(maxCount, c);

    CGFloat inset = 8;
    CGFloat labelH = 14;
    CGFloat colW = (b.size.width - inset * 2) / 12.0;
    CGFloat barW = std::round(colW * 0.62);
    CGFloat baseY = b.size.height - inset - labelH;
    CGFloat maxBarH = baseY - inset;

    NSFont* labelFont = [NSFont systemFontOfSize:10 weight:NSFontWeightMedium];
    for (int pc = 0; pc < 12; ++pc) {
        BOOL inScale = (_scaleMask >> pc) & 1;
        BOOL isRoot = inScale && pc == _scaleRoot;
        CGFloat colX = inset + pc * colW;

        if (maxCount > 0 && _counts[pc] > 0) {
            CGFloat h = std::max<CGFloat>(2, std::round(maxBarH * _counts[pc] / maxCount));
            NSRect bar = NSMakeRect(std::round(colX + (colW - barW) * 0.5), baseY - h, barW, h);
            [(isRoot ? barRoot : inScale ? barScale : barPlain) setFill];
            [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:2 yRadius:2] fill];
        }

        NSDictionary* attrs = @{
            NSFontAttributeName: labelFont,
            NSForegroundColorAttributeName: inScale ? labelLit : labelDim,
        };
        NSString* name = [NSString stringWithUTF8String:scale::kShortNames[pc]];
        NSSize sz = [name sizeWithAttributes:attrs];
        [name drawAtPoint:NSMakePoint(colX + (colW - sz.width) * 0.5, baseY + 2)
           withAttributes:attrs];
    }
}

@end

// ------------------------------------------------------------
// Chord pad: momentary button for one diatonic triad
// ------------------------------------------------------------

@class ChordPadView;

@protocol ChordPadDelegate
- (void)chordPadPressed:(ChordPadView*)pad;
- (void)chordPadReleased:(ChordPadView*)pad;
// A .mid file holding the pad's chord, or nil if there is nothing to drag.
- (NSURL*)midiFileURLForChordPad:(ChordPadView*)pad;
@end

@interface ChordPadView : NSView <NSDraggingSource>
@property(nonatomic, assign) int degree;
@property(nonatomic, copy)   NSString* numeral;
@property(nonatomic, copy)   NSString* chordName;
@property(nonatomic, assign) BOOL enabled;
@property(nonatomic, assign) BOOL pressed;
@property(nonatomic, weak)   id<ChordPadDelegate> delegate;
@end

@implementation ChordPadView {
    NSPoint _mouseDownPoint;
    BOOL _dragStarted;
}

- (BOOL)isFlipped { return YES; }

// Respond to the first click even when the plugin window isn't key.
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }

- (void)setNumeral:(NSString*)numeral chordName:(NSString*)chordName enabled:(BOOL)enabled {
    if ([numeral isEqualToString:_numeral] && [chordName isEqualToString:_chordName]
        && enabled == _enabled) {
        return;
    }
    _numeral = [numeral copy];
    _chordName = [chordName copy];
    _enabled = enabled;
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent*)event {
    if (!self.enabled) return;
    _mouseDownPoint = [self convertPoint:event.locationInWindow fromView:nil];
    _dragStarted = NO;
    self.pressed = YES;
    [self setNeedsDisplay:YES];
    [self.delegate chordPadPressed:self];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!self.pressed || _dragStarted) return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (std::hypot(p.x - _mouseDownPoint.x, p.y - _mouseDownPoint.y) < 4) return;

    // Moving turns the click into a drag: stop the preview, hand out a .mid file.
    _dragStarted = YES;
    [self releasePad];

    NSURL* url = [self.delegate midiFileURLForChordPad:self];
    if (!url) return;

    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:url];
    [item setDraggingFrame:self.bounds contents:[self snapshotImage]];
    [self beginDraggingSessionWithItems:@[item] event:event source:self];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    [self releasePad];
}

- (void)releasePad {
    if (!self.pressed) return;
    self.pressed = NO;
    [self setNeedsDisplay:YES];
    [self.delegate chordPadReleased:self];
}

- (NSImage*)snapshotImage {
    NSBitmapImageRep* rep = [self bitmapImageRepForCachingDisplayInRect:self.bounds];
    [self cacheDisplayInRect:self.bounds toBitmapImageRep:rep];
    NSImage* img = [[NSImage alloc] initWithSize:self.bounds.size];
    [img addRepresentation:rep];
    return img;
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    (void)context;
    return NSDragOperationCopy;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect b = NSInsetRect(self.bounds, 0.5, 0.5);

    NSColor* fill;
    NSColor* border;
    NSColor* numeralColor;
    NSColor* nameColor;
    if (!self.enabled) {
        fill         = [NSColor colorWithCalibratedRed:0.12 green:0.12 blue:0.14 alpha:1.0];
        border       = [NSColor colorWithCalibratedWhite:0.20 alpha:1.0];
        numeralColor = [NSColor colorWithCalibratedWhite:0.32 alpha:1.0];
        nameColor    = numeralColor;
    } else if (self.pressed) {
        fill         = [NSColor colorWithCalibratedRed:0.32 green:0.74 blue:1.00 alpha:1.0];
        border       = [NSColor colorWithCalibratedRed:0.60 green:0.85 blue:1.00 alpha:1.0];
        numeralColor = [NSColor colorWithCalibratedRed:0.05 green:0.10 blue:0.16 alpha:1.0];
        nameColor    = numeralColor;
    } else {
        fill         = [NSColor colorWithCalibratedRed:0.16 green:0.16 blue:0.20 alpha:1.0];
        border       = [NSColor colorWithCalibratedWhite:0.30 alpha:1.0];
        numeralColor = [NSColor whiteColor];
        nameColor    = [NSColor colorWithCalibratedWhite:0.62 alpha:1.0];
    }

    NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:b xRadius:5 yRadius:5];
    [fill setFill];
    [shape fill];
    [border setStroke];
    [shape setLineWidth:1.0];
    [shape stroke];

    NSDictionary* numeralAttrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName: numeralColor,
    };
    NSDictionary* nameAttrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: nameColor,
    };
    NSString* numeral = self.numeral ?: @"";
    NSString* name = self.chordName ?: @"";
    NSSize ns = [numeral sizeWithAttributes:numeralAttrs];
    NSSize cs = [name sizeWithAttributes:nameAttrs];
    CGFloat top = (self.bounds.size.height - ns.height - cs.height) * 0.5;
    [numeral drawAtPoint:NSMakePoint((self.bounds.size.width - ns.width) * 0.5, top)
          withAttributes:numeralAttrs];
    [name drawAtPoint:NSMakePoint((self.bounds.size.width - cs.width) * 0.5, top + ns.height)
       withAttributes:nameAttrs];
}

@end

// ------------------------------------------------------------
// Container view: label stack + keyboard + polling timer
// ------------------------------------------------------------

@interface ChordAnalyzerView : NSView <ChordPadDelegate>
@property(nonatomic, strong) NSTextField* subtitleLabel;
@property(nonatomic, strong) NSTextField* chordLabel;
@property(nonatomic, strong) KeyboardView* keyboardView;
@property(nonatomic, strong) NSTextField* scaleLabel;
@property(nonatomic, strong) NSPopUpButton* rootPopup;
@property(nonatomic, strong) NSPopUpButton* scaleTypePopup;
@property(nonatomic, strong) NSTextField* detectedTitleLabel;
@property(nonatomic, strong) NSTextField* detectedNameLabel;
@property(nonatomic, strong) NSButton* useDetectedButton;
@property(nonatomic, strong) NSButton* resetHistoryButton;
@property(nonatomic, strong) HistogramView* histogramView;
@property(nonatomic, strong) NSArray<ChordPadView*>* chordPads;
@property(nonatomic, assign) int detectedRoot;
@property(nonatomic, assign) int detectedType;
@property(nonatomic, strong) NSTimer* pollTimer;
@property(nonatomic, copy)   NSString* lastChordText;
@property(nonatomic, assign) ChordGuiCallbacks callbacks;
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

+ (NSString*)dragFileRoot {
    return [NSTemporaryDirectory() stringByAppendingPathComponent:@"ChordAnalyzer"];
}

// Drops drag files from earlier sessions. Anything under a day old is kept
// in case a host reads the file lazily after the drop.
+ (void)removeStaleDragFiles {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* root = ChordAnalyzerView.dragFileRoot;
    NSDate* cutoff = [NSDate dateWithTimeIntervalSinceNow:-24 * 60 * 60];
    for (NSString* entry in [fm contentsOfDirectoryAtPath:root error:nil]) {
        NSString* path = [root stringByAppendingPathComponent:entry];
        NSDate* modified = [fm attributesOfItemAtPath:path error:nil].fileModificationDate;
        if (modified && [modified compare:cutoff] == NSOrderedAscending) {
            [fm removeItemAtPath:path error:nil];
        }
    }
}

- (NSButton*)makeSmallButton:(NSString*)title action:(SEL)action {
    NSButton* btn = [NSButton buttonWithTitle:title target:self action:action];
    btn.bezelStyle = NSBezelStyleRounded;
    btn.controlSize = NSControlSizeSmall;
    btn.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    return btn;
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

        NSMutableArray<ChordPadView*>* pads = [NSMutableArray array];
        for (int d = 0; d < 7; ++d) {
            ChordPadView* pad = [[ChordPadView alloc] initWithFrame:NSZeroRect];
            pad.degree = d;
            pad.delegate = self;
            pad.toolTip = @"Click to play, drag into your DAW as a MIDI clip";
            [self addSubview:pad];
            [pads addObject:pad];
        }
        _chordPads = pads;

        [self applyScaleRoot:0 type:0];

        NSColor* dim = [NSColor colorWithCalibratedWhite:0.55 alpha:1.0];

        _detectedTitleLabel = [ChordAnalyzerView
            makeLabelWithFont:[NSFont systemFontOfSize:12 weight:NSFontWeightMedium] color:dim];
        _detectedTitleLabel.alignment = NSTextAlignmentLeft;
        _detectedTitleLabel.stringValue = @"Detected";
        [self addSubview:_detectedTitleLabel];

        _detectedNameLabel = [ChordAnalyzerView
            makeLabelWithFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]
                        color:[NSColor whiteColor]];
        _detectedNameLabel.alignment = NSTextAlignmentLeft;
        [self addSubview:_detectedNameLabel];

        _useDetectedButton = [self makeSmallButton:@"Use" action:@selector(useDetectedScale:)];
        _useDetectedButton.toolTip = @"Select the detected scale";
        [self addSubview:_useDetectedButton];

        _resetHistoryButton = [self makeSmallButton:@"Reset" action:@selector(resetHistory:)];
        _resetHistoryButton.toolTip = @"Clear the note histogram";
        [self addSubview:_resetHistoryButton];

        _histogramView = [[HistogramView alloc] initWithFrame:NSZeroRect];
        [self addSubview:_histogramView];

        [self updateDetectedRoot:0 type:0 early:NO historyCount:0];

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
    CGFloat histH = 80;
    CGFloat sectionGap = 14;
    CGFloat padH = 44;

    self.subtitleLabel.frame = NSMakeRect(0, pad, b.size.width, 16);

    // Bottom-up: histogram, detection row, keyboard, scale row.
    CGFloat histY = b.size.height - pad - histH;
    CGFloat detectY = histY - 6 - rowH;
    CGFloat kbY = detectY - sectionGap - kbH;
    CGFloat padY = kbY - rowGap - padH;
    CGFloat rowY = padY - rowGap - rowH;

    CGFloat chordY = pad + 22;
    CGFloat chordH = rowY - pad - chordY;
    if (chordH < 40) chordH = 40;
    self.chordLabel.frame = NSMakeRect(12, chordY, b.size.width - 24, chordH);
    [self resizeChordFontForText:self.chordLabel.stringValue];

    CGFloat right = b.size.width - pad;
    self.resetHistoryButton.frame = NSMakeRect(right - 60, detectY, 60, rowH);
    self.useDetectedButton.frame  = NSMakeRect(right - 60 - 4 - 50, detectY, 50, rowH);
    self.detectedTitleLabel.frame = NSMakeRect(pad, detectY + 3, 62, 16);
    self.detectedNameLabel.frame  = NSMakeRect(pad + 62, detectY + 2,
                                               NSMinX(self.useDetectedButton.frame) - 6 - pad - 62,
                                               18);
    self.histogramView.frame = NSMakeRect(pad, histY, b.size.width - pad * 2, histH);
    self.scaleLabel.frame     = NSMakeRect(pad, rowY + 3, 40, 16);
    self.rootPopup.frame      = NSMakeRect(pad + 40, rowY, 84, rowH);
    self.scaleTypePopup.frame = NSMakeRect(pad + 40 + 84 + 6, rowY, 140, rowH);

    CGFloat padGap = 6;
    CGFloat padW = (b.size.width - pad * 2 - padGap * 6) / 7.0;
    for (ChordPadView* p in self.chordPads) {
        CGFloat x = std::round(pad + p.degree * (padW + padGap));
        p.frame = NSMakeRect(x, padY, std::round(padW), padH);
    }

    self.keyboardView.frame = NSMakeRect(pad, kbY, b.size.width - pad * 2, kbH);
}

- (void)applyScaleRoot:(int)root type:(int)type {
    if (self.rootPopup.indexOfSelectedItem != root) [self.rootPopup selectItemAtIndex:root];
    if (self.scaleTypePopup.indexOfSelectedItem != type) [self.scaleTypePopup selectItemAtIndex:type];
    self.rootPopup.enabled = type != 0;
    [self.keyboardView updateScaleMask:scale::pitchClassMask(root, type) root:root];
    [self updateChordPadsForRoot:root type:type];
}

- (void)updateChordPadsForRoot:(int)root type:(int)type {
    static NSString* const kPlainNumerals[7] = {@"I", @"II", @"III", @"IV", @"V", @"VI", @"VII"};
    for (ChordPadView* pad in self.chordPads) {
        scale::Triad t;
        if (scale::diatonicTriad(root, type, pad.degree, &t)) {
            NSString* numeral = [NSString stringWithUTF8String:
                                 scale::romanNumeral(pad.degree, t.quality).c_str()];
            NSString* name = [NSString stringWithUTF8String:scale::triadName(t).c_str()];
            [pad setNumeral:numeral chordName:name enabled:YES];
        } else {
            // Pentatonic/blues/none: no seven diatonic triads.
            [pad setNumeral:kPlainNumerals[pad.degree] chordName:@"—" enabled:NO];
        }
    }
}

- (void)chordPadPressed:(ChordPadView*)pad {
    if (self.callbacks.playChord) self.callbacks.playChord(self.callbacks.userData, pad.degree);
}

- (void)chordPadReleased:(ChordPadView*)pad {
    (void)pad;
    if (self.callbacks.releaseChord) self.callbacks.releaseChord(self.callbacks.userData);
}

- (NSURL*)midiFileURLForChordPad:(ChordPadView*)pad {
    int root = (int)self.rootPopup.indexOfSelectedItem;
    int type = (int)self.scaleTypePopup.indexOfSelectedItem;
    scale::Triad t;
    if (!scale::diatonicTriad(root, type, pad.degree, &t)) return nil;

    int keys[3];
    scale::triadKeys(root, t, keys);
    std::string name = scale::triadAsciiName(t);
    std::vector<uint8_t> bytes =
        midifile::chordFile(keys, 3, (uint8_t)scale::kChordVelocity, name);

    // A fresh directory per drag keeps the file name clean ("Cm.mid", which
    // hosts use as the clip name) without clobbering a file a host may
    // still be reading from an earlier drag.
    NSString* dir = [ChordAnalyzerView.dragFileRoot
        stringByAppendingPathComponent:[NSUUID UUID].UUIDString];
    NSError* err = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:dir
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:&err]) {
        return nil;
    }
    NSString* path = [dir stringByAppendingPathComponent:
                      [NSString stringWithFormat:@"%s.mid", name.c_str()]];
    NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    if (![data writeToFile:path atomically:YES]) return nil;
    return [NSURL fileURLWithPath:path];
}

- (void)scaleSelectionChanged:(id)sender {
    (void)sender;
    int root = (int)self.rootPopup.indexOfSelectedItem;
    int type = (int)self.scaleTypePopup.indexOfSelectedItem;
    if (root < 0 || type < 0) return;
    [self applyScaleRoot:root type:type];
    if (self.callbacks.setScale) {
        self.callbacks.setScale(self.callbacks.userData, (uint8_t)root, (uint8_t)type);
    }
}

- (void)updateDetectedRoot:(int)root type:(int)type early:(BOOL)early historyCount:(int)count {
    self.detectedRoot = root;
    self.detectedType = type;

    NSString* name;
    if (type > 0) {
        name = [NSString stringWithFormat:@"%s %s%@",
                scale::kRootNames[root], scale::kTypes[type].name, early ? @"?" : @""];
    } else if (count == 0) {
        name = @"—";
    } else {
        name = @"Play more notes…";
    }
    if (![self.detectedNameLabel.stringValue isEqualToString:name]) {
        self.detectedNameLabel.stringValue = name;
        // Tentative guesses are dimmer than a full detection.
        self.detectedNameLabel.textColor = type == 0 ? [NSColor colorWithCalibratedWhite:0.55 alpha:1.0]
                                         : early     ? [NSColor colorWithCalibratedWhite:0.78 alpha:1.0]
                                                     : [NSColor whiteColor];
    }

    self.useDetectedButton.enabled = type > 0;
    self.resetHistoryButton.enabled = count > 0;
}

- (void)useDetectedScale:(id)sender {
    (void)sender;
    if (self.detectedType <= 0) return;
    [self applyScaleRoot:self.detectedRoot type:self.detectedType];
    if (self.callbacks.setScale) {
        self.callbacks.setScale(self.callbacks.userData,
                                (uint8_t)self.detectedRoot, (uint8_t)self.detectedType);
    }
}

- (void)resetHistory:(id)sender {
    (void)sender;
    if (self.callbacks.resetHistory) self.callbacks.resetHistory(self.callbacks.userData);
    [self pollTick];
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

- (void)startPollingWithCallbacks:(ChordGuiCallbacks)callbacks {
    self.callbacks = callbacks;
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
    if (!self.callbacks.getSnapshot) return;
    ChordSnapshot snap;
    std::memset(&snap, 0, sizeof(snap));
    self.callbacks.getSnapshot(self.callbacks.userData, &snap);

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

    int detRoot = snap.detectedRoot <= 11 ? snap.detectedRoot : 0;
    int detType = snap.detectedType < scale::kTypeCount ? snap.detectedType : 0;
    [self updateDetectedRoot:detRoot
                        type:detType
                       early:snap.detectedEarly != 0
                historyCount:snap.historyCount];
    // Bars are coloured by the detected scale, so the histogram shows how well it fits.
    [self.histogramView updateCounts:snap.histogram
                           scaleMask:scale::pitchClassMask(detRoot, detType)
                                root:detRoot];
}

- (void)dealloc {
    [_pollTimer invalidate];
}

@end

// ------------------------------------------------------------
// C entry points
// ------------------------------------------------------------

extern "C" {

void* chordGuiCreate(uint32_t w, uint32_t h, ChordGuiCallbacks callbacks) {
    @autoreleasepool {
        NSRect frame = NSMakeRect(0, 0, w, h);
        ChordAnalyzerView* view = [[ChordAnalyzerView alloc] initWithFrame:frame];
        [view startPollingWithCallbacks:callbacks];
        [ChordAnalyzerView removeStaleDragFiles];
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
    if (h) *h = 480;
}

} // extern "C"
