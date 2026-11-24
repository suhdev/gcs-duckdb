# Releasing

Releases are built and published by `.github/workflows/Release.yml`.

## Cutting a release

1. Make sure CI is green on `main`.
2. Tag the commit with a semantic version and push the tag:

    ```sh
    git tag -a v0.1.0 -m "gcs_adc v0.1.0"
    git push origin v0.1.0
    ```

3. The workflow builds the extension for every platform with `extension-ci-tools` (the same build as CI) and creates
   a GitHub release named after the tag, with generated release notes. Its assets are:

    | Asset | Contents |
    | --- | --- |
    | `gcs_adc.<platform>.duckdb_extension.gz` | The loadable extension for `<platform>` (`linux_amd64`, `linux_arm64`, `osx_amd64`, `osx_arm64`, `windows_amd64`, …) |
    | `httpfs`, `ui`, `json`, `parquet`, `icu`, `core_functions` `.<platform>.duckdb_extension.gz` | The other extensions built in the same DuckDB build (`ui` and `httpfs` are pinned in `extension_config.cmake`) |
    | `gcs_adc-extensions.<platform>.tar.gz` | All of the above for one platform, ready to unpack into `~/.duckdb/extensions/v2.0.1/<platform>/` |
    | `SHA256SUMS` | Checksums of the `.gz` files |

    The MinGW Windows build (`windows_amd64_mingw`) is not part of releases. Windows users get the MSVC `windows_amd64` build.

Tags with a pre-release suffix (`v0.2.0-rc.1`) are published as pre-releases.

You can also run the workflow by hand from the Actions tab (**Release → Run workflow**) with an existing tag.

## Version string

The extension reports its version through `SELECT extension_version FROM duckdb_extensions() WHERE extension_name =
'gcs_adc'`. The build takes it from `git describe --tags`, so a build of a tagged commit reports the tag
(`v0.1.0`), a later commit reports something like `v0.1.0-3-gabc1234`, and builds before the first tag report a
commit hash.

## Updating DuckDB

A release is built for one DuckDB version, set in the workflows (`duckdb_version`) and pinned by the `duckdb`
submodule. To move to a new DuckDB release, follow [Updating DuckDB](UPDATING.md), then cut a new release.
