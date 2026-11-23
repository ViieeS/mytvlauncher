# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- The clock rolls a changed digit: the old one rises away, shrinking and fading, as the new one
  springs up from below. With Reduce motion the two digits cross-fade in place.
- The focused tile carries a soft glint instead of the fixed sheen, as on Apple TV. It follows the
  Magic Remote pointer and slides in from the side focus came from. Holding OK presses the tile
  in, and the tile a menu opens over stays raised under it.

### Changed

- White text is slightly heavier, as macOS draws light text on a dark background, so small labels
  stay readable from the sofa.
- The typeface is Inter instead of Geist, in the UI and the app icon. Inter is the open design
  closest to Apple TV's SF Pro, and its optical sizes follow the text size.
- All digits are the same width, so the clocks no longer shift sideways when a digit changes.
- No text is smaller than 23 px, Apple TV's minimum: tile names, top bar labels, the weather,
  hints and Control Center were 19 to 22 px. A Control Center value too long for its module ends
  with an ellipsis.
- Reduce motion works as on Apple TV: fades stay, nothing slides, zooms or sways, and the dock
  and clock dissolve to their new place instead of jumping. Scrolling a row stays smooth. It used
  to cut every animation, fades included.

## [1.1.0] - 2026-11-22

### Added

- Remote app buttons (such as Netflix or Prime Video) can open any installed app: hold OK on a
  tile, choose Assign Remote Button and press the button. Needs the Home button hook on a rooted TV.

### Fixed

- Power-on after leaving the TV in an app opens myTV straight away, with no HDMI shown first, after
  standby and after a full power-off alike. Power-on after leaving it on an input still resumes that
  input. This also holds across the TV's nightly restart in standby.
- myTV no longer stays in its background state when it comes back after power-on.
- Closing an app, for example with Exit app in the browser, returns to myTV instead of LG Home.
- Home opens myTV again after the TV was turned on in the middle of an OLED pixel refresh. The TV
  could list myTV as running with no myTV process, and every launch waited on it.

### Changed

- While another app is in front, myTV frees the wallpapers that are not on screen, about 34 MB.

### Removed

- The Recent row under the home rows. It repeated apps already on screen. Recent apps stay in the
  switcher: hold OK on a tile, then Recent Apps.

## [1.0.0] - 2026-10-30

First public release.

### Added

- Home screen: glass dock, second row and recent apps, focus zoom and lift, running-app dots.
- App management: move, remove from Home and uninstall from the hold-OK menu. Magic Remote hover
  and drag.
- Control Center: sound and picture mode, sleep timer, screen off, backlight, eye comfort.
- Wallpapers: five bundled public-domain paintings, rotation, night dimming, plain colour.
- Screensaver: the TV's own by default, or myTV's photo slideshow with clock or StandBy clock faces.
- Date, time and weather for a chosen city.
- Home button opens myTV on rooted TVs, and the TV returns to myTV after standby.
- Settings for the home screen, wallpaper, screensaver, weather, clock and appearance.

[1.1.0]: https://github.com/ViieeS/mytvlauncher/releases/tag/v1.1.0
[1.0.0]: https://github.com/ViieeS/mytvlauncher/releases/tag/v1.0.0
