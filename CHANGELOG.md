# Changelog

All notable changes to `faultlab`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

## 2026-11-24

### Added

- **Plain and full syncs in faultfs.** `filesystem::set_flush_mode(flush_mode, sync_platform)`
  names completion's flush mode (`full`, `plain`) and the platform (`macos`, `linux_kernel`,
  `other`), and `sync_call_at(sync_site)` the call each sync site makes: F_FULLFSYNC, fsync,
  F_BARRIERFSYNC or fdatasync, as completion's PosixIO makes them. On macOS a plain fsync
  survives `kill()` but `crash()` may drop it like unsynced data, ordered ahead of the next
  ordering point; a plain directory sync makes nothing durable. F_FULLFSYNC, and fsync and
  fdatasync off macOS, persist as every sync did. The default is `full` on the host, so a user
  that sets no mode sees no change. `File::sync_data` is now modeled as its own call (fdatasync
  on Linux), with the same effect as `sync`. The table is in
  [docs/mechanisms/crash-model.md](docs/mechanisms/crash-model.md#plain-and-full-syncs).

### Changed

- A file sync that persists settles the files staged with no ordering point (a plain sync's
  push, or a file staged after another thread's directory sync settled its point), as a
  directory sync already did: both are a device flush.
