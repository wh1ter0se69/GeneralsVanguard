# GeneralsX - iOS / iPadOS build

The iOS/iPadOS port comes from [ammaarreshi/Generals-Mac-iOS-iPad](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad)
(see [../IOS_PORT.md](../IOS_PORT.md)). There is no public iPad download; these steps build and install it on your
own device.

## Prerequisites

Everything in [MACOS.md](MACOS.md) (Xcode command line tools, Homebrew `cmake ninja meson pkgconf`, the LunarG Vulkan
SDK, vcpkg with `VCPKG_ROOT` set), plus:

* full Xcode, signed into your Apple ID;
* `brew install xcodegen`;
* an Apple Developer team (a free one works for installing on your own device).

## Build and install

```sh
git submodule update --init --recursive references/fbraz3-dxvk   # iOS DXVK is built from this + Patches/dxvk-ios.patch
./scripts/build/ios/fetch-moltenvk.sh                            # pinned MoltenVK.framework (checksummed)
./scripts/build/ios/stage-fonts.sh                               # Liberation fonts, renamed as the game expects
cmake --preset ios-vulkan
cmake --build build/ios-vulkan --target z_generals
GX_TEAM_ID=<your-team-id> GX_BUNDLE_ID=com.you.generalszh \
    ./scripts/build/ios/package-ios-zh.sh --install              # assembles, signs, installs
```

`--recursive` is required: DXVK vendors Vulkan-Headers, SPIRV-Headers and the mingw-directx headers as its own
submodules, and without them the Meson cross-configure fails with `Missing Vulkan-Headers`.

Your team id is in Xcode > Settings > Accounts. The game data ships inside the app bundle (a self-contained install,
about 2.7 GB); `--dev` skips that copy for code-only iteration. `--device <id>` picks a device when more than one is
connected. The packaging script does not build; run the `cmake --build` step first.
