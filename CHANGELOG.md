# Changelog

All notable changes to this project are documented in this file.

## [1.3.2] - 2026-08-16

### Changed
- Bump Android NDK used by CI from `26.3.11579264` to `29.0.14206865`

## [1.3.1] - 2026-08-12

### Added
- Automated npm publish pipeline: pushing a git tag now triggers a Semaphore
  promotion that builds the native binaries and publishes to npmjs.

### Fixed
- Walk `/dev/input` in numeric order when autodetecting the touch device (#25)
- Align LOAD segments to 16 KB for Android 15+ page size support (#24)
- Fixed typographical errors in docs (#13, #15)

### Changed
- Updated Semaphore CI OS image from Ubuntu 20.04 to 22.04 (#23)
- Updated `libevdev` git submodule URL (#20)
- Migrated to GitHub-native Dependabot (#4)
- Updated project branding and namespace to `devicefarmer` (#11)

## [1.3.0] - 2020-02-01

Initial release under the `@devicefarmer/minitouch-prebuilt` package name.
