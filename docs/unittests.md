# Unit Tests (Current Setup)

This document explains the current Unity test setup for `files.c`.

The goal was:
- keep the normal firmware build unchanged
- run tests in a small dedicated app
- keep test files under `tests/unity/`

## Directory Convention

- Unity tests: `tests/unity/`
- Integration tests (HTTP/QEMU flow tests): `tests/integration/`

## What Was Added

### 1) Unity test cases for `files_props_*`

File:
- `tests/unity/files_props/test_files_props.c`

This file has 3 tests:
- `files_props_init sets basename and defaults`
- `files_props_set clamps probability and volume`
- `files_props_set accepts disabled zeroed entry`

These are logic tests. They do not need real hardware.

### 2) Dedicated Unity test app

Folder:
- `tests/unity/files_props/`

This is a separate ESP-IDF app for tests only.
Your production app in `main/` is unchanged.

Main files:
- `tests/unity/files_props/CMakeLists.txt`
- `tests/unity/files_props/main/CMakeLists.txt`
- `tests/unity/files_props/main/test_runner.c`
- `tests/unity/files_props/components/files_under_test/CMakeLists.txt`

## How It Works

### `tests/unity/files_props/main/test_runner.c`

`app_main()` does:
- `UNITY_BEGIN()`
- `unity_run_all_tests()`
- `UNITY_END()`

So all registered `TEST_CASE(...)` tests are run.

### `tests/unity/files_props/components/files_under_test/CMakeLists.txt`

This component compiles:
- production code: `main/files.c`
- test code: `tests/unity/files_props/test_files_props.c`

Important details:
- `WHOLE_ARCHIVE` is enabled.
  - This prevents test symbols from being optimized away.
  - Without it, Unity may print `0 Tests`.
- `CONFIG_BASE_PATH` is forced to `"/data"` because `files.c` expects it.
- `joltwallet__littlefs` is required because `files.c` includes LittleFS headers.

### `tests/unity/files_props/CMakeLists.txt`

Important details:
- `EXTRA_COMPONENT_DIRS "../../../managed_components"`
  - allows using `joltwallet__littlefs`
- `idf_build_set_property(MINIMAL_BUILD ON)`
  - keeps this test build smaller/faster

## How To Run

From repo root:

```bash
source /home/patrick/esp/v5.5.2/esp-idf/export.sh
idf.py -C tests/unity/files_props -B build_unity_files_props_test set-target esp32
timeout 45s idf.py -C tests/unity/files_props -B build_unity_files_props_test qemu
```

Notes:
- `timeout 45s` is intentional, so QEMU exits automatically after results are printed.
- If your ESP-IDF path is different, change the `source` path.

## Expected Output

You should see lines like:

```text
Running files_props_init sets basename and defaults...PASS
Running files_props_set clamps probability and volume...PASS
Running files_props_set accepts disabled zeroed entry...PASS

-----------------------
3 Tests 0 Failures 0 Ignored
OK
```

## Files/Artifacts You Will See

- build output:
  - `build_unity_files_props_test/`
- test project config:
  - `tests/unity/files_props/sdkconfig`
  - `tests/unity/files_props/dependencies.lock`

These belong to the test app only.

## Common Issues

### "0 Tests 0 Failures"

Usually means test objects were linked out.
In this setup, `WHOLE_ARCHIVE` is already enabled to fix that.

### "component not found" for LittleFS

Check `tests/unity/files_props/CMakeLists.txt` contains:

```cmake
set(EXTRA_COMPONENT_DIRS "../../../managed_components")
```

### `idf.py` not found or IDF not exported

Run:

```bash
source /home/patrick/esp/v5.5.2/esp-idf/export.sh
```

## How To Add More Unity Tests

Simple flow:
1. Add new `TEST_CASE(...)` into a file under `tests/unity/`
2. Add that test source file to the test component CMake file
   - `tests/unity/files_props/components/files_under_test/CMakeLists.txt`
3. Re-run the same QEMU command

For HTTP/UI flows (like `/skip` in bootstrap), add those under `tests/integration/` as integration tests instead of Unity tests.
