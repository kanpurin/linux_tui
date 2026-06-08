# linux_tui

Linux 向け TUI ツール集です。

## Tools

- `procview/` - C/ncurses 製プロセスビューアー
- `testforge/` - C/ncurses 製 TestForge、負荷注入・試験シナリオ支援ツール

## Build

```bash
make
```

個別にビルドする場合:

```bash
make -C procview
make -C testforge
```

## Run

```bash
./procview/procview
./testforge/testforge
```

## AutoTest Script Builder

```bash
make -C autotest-assist-tui
./autotest-assist-tui/autotest-builder
```
