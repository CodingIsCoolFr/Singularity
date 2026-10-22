# Singularity

## Anything that moves runs on the frame clock

Profile effects, avatar decorations, nameplates, animated avatars and banners,
name styles, chat emoji, and anything animated added later go through
`src/ui/FrameClock.h`:

- Take animation time from `FrameClock::now()`, never from a `QElapsedTimer`
  or `QDateTime` of your own. It holds still through a paint.
- Choose the frame on a tick and keep it; paint the kept frame. Never work a
  frame out from the time inside `paintEvent`: other widgets repaint strips of
  yours on their own beat, and a frame chosen at paint time draws a band
  through the picture and skips frames (the profile effect bug, 0.8.98).
- Book the next frame with `FrameClock::wakeAt`, step an animation with a
  `FramePulse` where a `QTimer` would have done, and repaint something that
  moves all the time with `FrameClock::scheduleRepaint`. `AnimatedImage`
  already does this for GIF, WebP and APNG pictures.

`tools/test_frames.cpp` checks these rules and the release build runs it. A new
kind of animation gets a case there.

The window backgrounds (`Backdrop`, `AuroraWidget`) are the one exception: they
stay on the frame pacer's beat (`SingularityApplication::setFramePace`), which
exists to keep their redraws cheap.
