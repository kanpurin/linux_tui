# TestForge

TUI-based load injection and test support tool for Linux and custom OS test labs.

TestForge is a focused TUI for choosing a test category, selecting an action, configuring the required values, and running it.

## Build

```bash
make
```

Requires a C compiler and ncurses development headers.

```bash
apt-get update
apt-get install -y build-essential libncurses-dev
```

## Run

```bash
./testforge
```

Dry Run is enabled by default. Press `d` to allow real execution.

Memory actions read `/proc/meminfo` and use `MemAvailable` as the maximum allocatable memory shown in the UI. If you enter a larger value, TestForge clamps it to the current available-memory limit.

The details panel shows what the selected setting means. Numeric settings accept digits only.

## Keys

| Key | Action |
| --- | --- |
| `/` | Search actions |
| `j` / `Down` | Move down |
| `k` / `Up` | Move up |
| `Enter` on category | Open category |
| `Esc` / `Backspace` | Return to categories |
| `Enter` on action | Open configuration screen |
| `Enter` in configuration | Edit selected value |
| `Left` / `Right` | Toggle boolean and choice values |
| `r` in configuration | Run configured action |
| `d` | Toggle Dry Run |
| `q` | Quit |

## Current Scope

The implementation includes category/action navigation, focused details/configuration, dry-run execution, and built-in runners for the actions shown in the catalog.

Real execution is intentionally bounded by the configured values. The current built-in runners cover:

- Memory pressure, leak, fragmentation, and OOM pressure
- Disk fill, inode fill, random write, fsync storm, and large file creation
- CPU burn, process flood, zombie/orphan process, and FD exhaustion
- Regular files, directories, links, FIFOs, UNIX sockets, and device nodes
- TCP server/flood behaviors
- Open/read failure checks, EBUSY lock holder, ENOSPC fill, timeout, signal injection, and file tamper
