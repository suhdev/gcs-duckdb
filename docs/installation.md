# Installation

`gcs_adc` is built for **DuckDB v2.0.1**. A DuckDB extension only loads into the DuckDB version it was built for, so
check your version with `SELECT version();`.

`gcs_adc` needs the `httpfs` extension, which does the storage I/O. It is autoloaded when you load `gcs_adc`, or you
can load it yourself first:

```sql
INSTALL httpfs;
LOAD httpfs;
```

## From a GitHub release

Each [release](https://github.com/suhdev/gcs-duckdb/releases) has a binary for every supported platform, named
`gcs_adc.<platform>.duckdb_extension.gz` (for example `linux_amd64`, `linux_arm64`, `osx_arm64`, `osx_amd64`,
`windows_amd64`). Download and unpack the one for your platform:

```sh
curl -LO https://github.com/suhdev/gcs-duckdb/releases/latest/download/gcs_adc.linux_amd64.duckdb_extension.gz
gunzip gcs_adc.linux_amd64.duckdb_extension.gz
```

These binaries are not signed by DuckDB, so start DuckDB with unsigned extensions allowed:

=== "CLI"

    ```sh
    duckdb -unsigned
    ```

    ```sql
    LOAD '/path/to/gcs_adc.linux_amd64.duckdb_extension';
    ```

=== "Python"

    ```python
    import duckdb

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute("LOAD '/path/to/gcs_adc.linux_amd64.duckdb_extension'")
    ```

Every release also includes `SHA256SUMS`, so you can check what you downloaded:

```sh
sha256sum -c SHA256SUMS --ignore-missing
```

### httpfs, the UI and the other bundled extensions

Each release also ships the extensions built alongside `gcs_adc` from the same DuckDB v2.0.1 build: `httpfs`, `ui`
(the DuckDB web UI) and DuckDB's own `json`, `parquet`, `icu` and `core_functions`. Each one comes as
`<extension>.<platform>.duckdb_extension.gz`, and they also come together as `gcs_adc-extensions.<platform>.tar.gz`.

The easiest setup is to unpack the bundle into DuckDB's extension directory, so a plain `LOAD` finds them:

```sh
platform=osx_arm64   # or linux_amd64, linux_arm64, osx_amd64, windows_amd64
dir=~/.duckdb/extensions/v2.0.1/$platform
mkdir -p "$dir"
curl -L https://github.com/suhdev/gcs-duckdb/releases/latest/download/gcs_adc-extensions.$platform.tar.gz \
  | tar -xz -C "$dir"
```

```sql
-- duckdb -unsigned
LOAD gcs_adc;
CREATE SECRET (TYPE gcs, PROVIDER adc);
LOAD ui;
CALL start_ui();
```

Like `gcs_adc`, these builds are not signed by DuckDB, so they also need `-unsigned`. Running `INSTALL ui` or
`INSTALL httpfs` later replaces them with DuckDB's signed builds, which work just as well for v2.0.1.

## From the community extension repository

Publishing to [DuckDB Community Extensions](https://duckdb.org/community_extensions/) is planned. Once it is
published you will be able to run:

```sql
INSTALL gcs_adc FROM community;
LOAD gcs_adc;
```

## From source

You need CMake, Ninja, a C++17 compiler, and the OpenSSL and libcurl development headers (or
[vcpkg](https://vcpkg.io)).

```sh
git clone --recurse-submodules https://github.com/suhdev/gcs-duckdb
cd gcs-duckdb
GEN=ninja make release
```

The build produces:

| Path | What |
| --- | --- |
| `build/release/duckdb` | DuckDB shell |
| `build/release/repository/<commit>/<platform>/gcs_adc.duckdb_extension` | The loadable extension |
| `build/release/repository/<commit>/<platform>/httpfs.duckdb_extension` | A matching httpfs |
| `build/release/test/unittest` | Test runner |

On Debian or Ubuntu the dependencies are:

```sh
sudo apt-get install -y cmake ninja-build g++ libssl-dev libcurl4-openssl-dev
```

On macOS, install the Xcode command line tools and the rest from Homebrew. libcurl comes with the macOS SDK.
Homebrew's OpenSSL is not linked into the default search paths, so point CMake at it:

```sh
xcode-select --install
brew install cmake ninja openssl@3 ccache
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"

git clone --recurse-submodules https://github.com/suhdev/gcs-duckdb
cd gcs-duckdb
GEN=ninja make release
```

The mock tests (`make test_mock`) also need `python3` and `openssl` on the `PATH`, and both are included in macOS.
