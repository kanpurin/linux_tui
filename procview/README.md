# procview

Linux の `/proc` を読む C/ncurses 製プロセスビューアー TUI です。

上部に `cmd name:` フィルタ、左にプロセス一覧、右に選択プロセスの詳細、オープンファイル、環境変数、プロセスツリーを表示します。
フィルタはプロセス名だけを対象にし、親プロセスは補完表示しません。

## Build

```bash
make
```

ncurses の開発パッケージがない場合は、Debian/Ubuntu 系なら次を入れてください。

```bash
apt-get update
apt-get install -y build-essential libncurses-dev
```

## Run

```bash
./procview
```

## Keys

| Key | Action |
| --- | --- |
| `j` / `Down` | 下へ移動 |
| `Up` | 上へ移動 |
| `PageUp` / `PageDown` | ページ移動 |
| `g` / `G` | 先頭 / 末尾へ移動 |
| `/` | 上部の `cmd name:` を編集してフィルタ |
| `Esc` | フィルタ解除 |
| `t` | プロセスツリー / リスト表示の切り替え |
| `Tab` / `Shift-Tab` | パネル移動 |
| `r` | 即時更新 |
| `k` / `Del` / `Enter` | `processes` または `process tree` の選択プロセスへ `SIGTERM` を送信 |
| `K` | `processes` または `process tree` の選択プロセスへ `SIGKILL` を送信 |
| `q` | 終了 |

kill 確認ダイアログでは `Left` / `Right`、`h` / `l`、`Tab` で `kill` / `Cancel` を切り替え、`Enter` で決定します。
