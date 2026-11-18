# disklab: how it works

disklab (design F15) gives a test a real filesystem on a real block device whose every write is
logged with its flush and FUA marks, so the test can see the states a power cut leaves, plus a
slow device and a failing one. It needs root and device-mapper, so it runs in a Linux VM.

```
filesystem       <dir>/mnt
<name>-disk      linear | delay <ms> | flakey 0 1 1 error_writes|drop_writes   (fault layer)
<name>-log       log-writes data-loop log-loop
loop  loop       <dir>/data.img, <dir>/log.img   (sparse)
```

**The log.** dm-log-writes passes each write to the data device and, once it completes, records
it on the log device: a header sector (offset, length, flags, a mark's name) and its data. Plain
writes wait in a list until the next flush, which logs them followed by itself; FUA writes and
marks (`dmsetup message ... mark <name>`) are logged at once. So everything the log holds before
a flush, FUA write or mark is what a device with a volatile cache keeps when power fails there.
Removing the target logs `dm-log-writes-end` and the final super block; `stop` checks for that
mark, which is missing when the log filled up.

**Replay.** `mkfs` runs through the stack, so the log holds every write since a zeroed device.
`rebuild` reads the log in Python and writes the entries up to the chosen one into a zeroed sparse
image (a discard writes zeros). `replay` mounts that image over a loop device, read-write, so the
filesystem runs its own journal recovery, as at boot after the power cut. Each replay starts from
zero, so replays are independent and need no snapshot.

**The fault layer.** One dm device above the log whose table swaps between `linear`, `delay` and
`flakey`: `dmsetup suspend --nolockfs`, `load`, `resume`. `--nolockfs` keeps the suspend from
freezing the filesystem, which would sync it, add writes the test did not make, and, inside a
media window, fail the sync and abort the journal. The layer sits above dm-log-writes, so writes
that `media` fails or drops never reach the log: a replay sees the device the filesystem had.
dm-flakey with up 0 s and down 1 s is down from the moment it loads until `heal` replaces it.

**Teardown.** Top down: the replay and lab mounts, the `-disk` then `-log` devices (each resumed
first, in case a kill left it suspended), the loops, then the directory, which is deleted only
when no mount remains under it. It runs at `close`, at exit (atexit), and on SIGTERM and SIGHUP
when their handlers are the default (SIGINT exits through atexit). SIGKILL runs nothing, so names
carry the owner's pid: `<prefix>-<pid>-<n>-disk` and `-log`, loops backed by
`<root>/<prefix>-<pid>-<n>/*.img`, mounts under that directory; opening a lab clears those of
dead pids, and `disklab.py sweep` does the same from the shell.

**Evidence.** `python/tests/disklab_test.py --live`, as root in the Linux VM (Ubuntu 24.04,
kernel 6.8, images on `/dev/shm`): a planted unsynced write is missing at every point from the
mark before its fsync to the flush before the jbd2 commit block and present from that FUA commit
write on; a replay takes 0.055 s median over a 32.5 MiB log, and 0.26 s over a 314 MiB log of
256 fsynced 1 MiB files (rebuild alone 1.5 GB/s); `delay(100)` adds 104 ms to fdatasync (one
flush) and about 4 trips to a buffered write plus fsync; `media` fails or drops all 20 writes in
its window and none of 20 before or after; teardown leaves nothing after close, exit, SIGTERM,
SIGHUP, SIGINT and SIGKILL (sweep), also with a replay mounted.
