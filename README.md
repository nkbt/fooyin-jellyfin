# fooyin-jellyfin

Adds a Jellyfin music library to fooyin, with local metadata, native playlists and on-demand audio streaming. Sign-in uses Quick Connect, and session tokens are stored in the desktop secret service.

Catalog sync runs on connection and when Refresh library is selected. Failed syncs retain the last complete catalog. Jellyfin playlists and favourites are read-only, and playback is reported to the server. The final report sent when fooyin closes is best effort. Signing out hides the catalog while retaining its metadata for reconnection.

The plugin requires [external library support (#4)](https://github.com/nkbt/fooyin/pull/4) from the [`external-libraries` branch of nkbt/fooyin](https://github.com/nkbt/fooyin/tree/external-libraries). It does not work with an unmodified fooyin release.

![Jellyfin settings](docs/jellyfin-settings.png)

## Build

Requires Linux, CMake 3.25 or later, Ninja, a C++23 compiler, Qt 6, libsecret and the [fooyin build dependencies](https://github.com/fooyin/fooyin). Tests also require GoogleTest, Python 3 and OpenSSL.

From this repository, build the `external-libraries` branch of `nkbt/fooyin`:

```sh
mkdir --parents tmp/runtime
export TMPDIR="$PWD/tmp/runtime"
git clone --branch external-libraries --single-branch https://github.com/nkbt/fooyin.git tmp/fooyin
cmake -S tmp/fooyin -B tmp/fooyin-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/tmp/install" -DINSTALL_HEADERS=ON
cmake --build tmp/fooyin-build --parallel 2
cmake --install tmp/fooyin-build
```

Build, test and install the plugin into the same prefix:

```sh
cmake -S . -B tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/tmp/install" -DCMAKE_INSTALL_PREFIX="$PWD/tmp/install" -DFOOYIN_SOURCE_DIR="$PWD/tmp/fooyin" -DBUILD_TESTING=ON
cmake --build tmp/build --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir tmp/build --output-on-failure --parallel 2
cmake --install tmp/build
```

Tests use synthetic accounts, media and local servers. No running Jellyfin server or secret service is needed for the test suite. Set `FOOYIN_SOURCE_DIR` to the matching fooyin source to include native library and playback integration tests.

## Connect

Start the local build with an isolated profile:

```sh
scripts/launch-candidate.sh --host-prefix "$PWD/tmp/install" --profile-dir "$PWD/tmp/profile"
```

Open Settings → Integrations → Jellyfin, enter the server address and select Sign in with Quick Connect. Authorise the displayed code in a signed-in Jellyfin client. Quick Connect must be enabled on the server, and the desktop secret service must be available to save the session.

The host PR adds external library snapshots, source-owned playlists and playback lifecycle events. Plugin code remains separate under `src/`.

Released under GPL-3.0-or-later. See [COPYING](COPYING).
