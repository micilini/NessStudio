Third-party components accompanying NessStudio

manifest.json records the versions in the managed publish dependency graph,
the source of each included license/notice, and the SHA-256 of its exact bytes.
Package identifiers include transitive dependencies even when NuGet chooses
the runtime asset of another package with the same assembly name.

MaterialDesignColors and MaterialDesignThemes: MIT.
Embedded Roboto fonts: Apache-2.0, with their font copyright notice.
Material Design Icons: accompanying Pictogrammers notice and Apache-2.0 text;
the upstream notice also describes separately licensed contributions.
Microsoft.Xaml.Behaviors.Wpf: MIT.
NAudio and its managed components: MIT.
sqlite-net-pcl and sqlite-net-sqlcipher: MIT.
SQLitePCLRaw packages: Apache-2.0; preserve the accompanying upstream notices.
SQLite: public domain; its notice is included in SQLitePCLRaw-NOTICE.txt.
SQLCipher Community 4.20.0: SQLCipher-4.20.0.txt (Zetetic BSD-style license).
Its SQLite 3.54.1 engine: SQLite-3.54.1.txt (public domain).
Its statically linked OpenSSL 3.5.9: OpenSSL-3.5.9.txt (Apache-2.0).
Windows SDK for .NET: the license linked by the targeting package.
WinRT.Runtime: the MIT license from its embedded source revision.
MSVC runtime: Microsoft Visual C++ V14 Redistributable and Runtime terms.

SQLitePCLRaw-NOTICE.txt is the unmodified v2.1.2 upstream notice. Package
copyrights, including the newer e_sqlite3 package, are recorded separately in
manifest.json. An upstream notice may mention components not linked in this
particular Windows build. It is not a declaration that all those components
are present. The current e_sqlcipher.dll is built from the pinned upstream
sources in build/sqlcipher/sources.lock.json, using OpenSSL 3.5.9. Its actual
bytes and build provenance are recorded in sqlcipher-runtime.json and the
nativeComponents inventory. The older NuGet native payload is excluded;
the existing managed SQLitePCLRaw provider and sqlite-net remain in use.
SQLCipher.txt and LibTomCrypt.txt are retained notices from the original
NuGet package and do not describe the replacement native binary.

Native engine dependencies have separate notices beside libnessstudio.dll:
NessMuxer, nlohmann/json, oneVPL, x264, NVENC headers and AMF headers.
x264 source and its GPL-2.0-or-later notice accompany its runtime.
FFmpeg's license and provider notice accompany Native/FFmpeg/ffmpeg.exe.
The framework-dependent App requires the separately installed .NET 10
Windows Desktop Runtime; that runtime is not bundled in the App directory.

These files preserve third-party notices. They do not change the license of
NessStudio's own source or approve a public release. Distribution review,
including GPL source obligations and native dependency maintenance, remains
a separate release gate.
