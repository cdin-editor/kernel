# Installing cdin-kernel from a release asset

Choose the archive matching your operating system and CPU architecture. These
archives contain the public header, static library, shared library/import
library where supported, pkg-config metadata, documentation, and a quickstart
example. The package release version and C ABI version are recorded separately
in `BUILD-INFO.txt`.

## Linux / macOS

Extract the `.tar.gz`, enter the extracted directory, then install for your
current user (no administrator privileges required):

```sh
./install.sh
```

To choose a prefix:

```sh
./install.sh --prefix "$HOME/.local"
```

For a system-wide install, use a writable prefix such as `/usr/local` and run
with the appropriate privileges:

```sh
sudo ./install.sh --prefix /usr/local
```

To build a C application, set `PKG_CONFIG_PATH` if the prefix is not a system
pkg-config directory, then link with `pkg-config`:

```sh
export PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cc -std=c11 -I"$HOME/.local/include" app.c \
  "$HOME/.local/lib/libcdin_kernel.a" -pthread -o app
```

## Windows (x86-64, MinGW-w64 UCRT64)

Extract the `.zip`, open PowerShell in the extracted directory, and run:

```powershell
.\install.ps1
```

The default prefix is `%LOCALAPPDATA%\cdin-kernel`; choose another with
`-Prefix`. Add the installed DLL directory to your user PATH with
`-AddBinToUserPath`, or keep `bin\cdin_kernel.dll` beside the application that
uses the shared library. The archive is built with MinGW-w64 UCRT64; MSVC ABI
compatibility is not promised by this release.

## Consumer projects

Use `include/cdin/kernel.h` and link either `libcdin_kernel.a` (static) or the
platform's shared-library import/runtime files. This release provides the stable C ABI `1.0.0`. Compile consumers against the
matching `include/cdin/kernel.h`, query the loaded ABI version at startup, and
review the compatibility rules before updating bindings. Verify SHA-256 against
the adjacent `.sha256` file before installing.
