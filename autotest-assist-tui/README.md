# AutoTest Script Builder TUI

## Reboot and Rescue Limitations

Reboot tests can run while systemd is still moving between targets. In that
window, some commands do not yet have reliable state even when the machine
itself is working correctly.

Known fragile commands:

- `runlevel` and `who -r`: they read utmp. During early resume or
  rescue-to-default transition, utmp may still be empty, stale, or `unknown`.
- `systemctl is-active rescue.target`: the resume service can run before
  `rescue.service`, so rescue may not be `active` yet.
- `systemctl is-system-running`: target transitions may report `starting`,
  `maintenance`, or `degraded` temporarily.
- Login/session commands such as `who`, `w`, and `users`: these also depend on
  utmp/session state.
- Network state commands such as `hostname -I`, `ip route`, or `nmcli`: early
  resume may run before the network is fully ready.

For test assertions, prefer assigning a purpose-built variable from stable
checks, then match that variable:

```sh
target=$(systemctl get-default)
case "$target" in
  multi-user.target|runlevel2.target|runlevel3.target|runlevel4.target)
    AUTOTEST_ACTUAL=3
    ;;
  rescue.target|runlevel1.target)
    AUTOTEST_ACTUAL=1
    ;;
  *)
    AUTOTEST_ACTUAL=unknown
    ;;
esac
```

Use `runlevel` only after the system has completed the transition and utmp has
been updated.

## `@tui` Output Capture

Each `@tui` block stores the most recent pseudo-terminal session output in
variables that can be used by later script lines and `@check` rules:

- `AUTOTEST_TUI_STDOUT`: transcript captured from the pseudo terminal.
- `AUTOTEST_TUI_STDERR`: errors emitted by the `script` wrapper itself.
- `AUTOTEST_TUI_STATUS`: exit status from the `script` command.
- `AUTOTEST_TUI_STDOUT_FILE` / `AUTOTEST_TUI_STDERR_FILE`: backing files.

Because TUI programs run through a pseudo terminal, the program's stdout and
stderr may be merged by the terminal layer. For assertions, prefer `contains`
or `regex` checks against `AUTOTEST_TUI_STDOUT`.

## Editor Help

In any built-in editor, run `:help` from normal mode to open a centered help
window. It lists AutoTest-specific script directives such as `@check`,
`@assert`, `@backup`, `@restore`, `@reboot-if`, `@tui`, TUI send commands,
captured `AUTOTEST_TUI_*` variables, and the editor's normal-mode commands.
Use Up/Down to scroll the help window when the terminal is small. PageUp,
PageDown, Home, and End are also supported. Close the help window with `Esc`,
`Enter`, or `q`.

## Generated Script Names

When a test is saved, the generated script name is based on the test title:
`<test-title>.sh`. Spaces and unsafe path characters are normalized for
filenames. If the title is empty, the test id is used as a fallback.

Saving fails if another file with the same generated name already exists in the
same directory. To rename a test script later, change the test title and run
`Save test`; the script path is recalculated in the same directory, and the old
script/result files are removed after the new script is written.

From the test list, use `Rename test` to change an existing test title directly.
The new name is checked immediately; if the generated script name already
exists, the rename is rejected before the script is rewritten.

## Generated Script Options

Generated test scripts support an optional detailed result file:

```sh
./service_check.sh --detail-result /tmp/service_check.detail
```

The normal terminal output remains OK/NG focused. The detailed result file
includes the status line, summary, expected/actual exit codes, variable check
names and values, captured stdout, and captured stderr. For reboot tests, the
detail result path is saved before reboot and reused by the resume run.

Linux 向けの「自動テスト作成支援」TUI です。`Create test` から独自エディタを開いてテストを作成し、作成済みテストを複数選択して自動テストを開始できます。

生成されるテストスクリプトは、各テスト項目を実行し、終了コードと出力判定から結果を `OK` または `NG` として表示します。

## 目的

- Linux のテスト手順を対話的に組み立て、シェルスクリプトとして出力する。
- 作成したテストを複数選択して、自動テストを開始する。
- コマンド実行後の `echo $?` 相当の終了コードを判定に使う。
- 期待した標準出力/標準エラーになっているかを、複数の判定方法で確認できるようにする。
- マシン再起動を含むテストでも、再起動前スクリプトと再起動後スクリプトを生成できるようにする。
- テスト終了後の後処理も、生成スクリプトに組み込めるようにする。

## 基本フロー

```text
1. Create test
2. vim 風の独自エディタでテストスクリプト本文を作成
3. `:wq` で保存する
4. TUIを起動したカレントディレクトリに単体テストスクリプトを生成
5. TUIは生成されたスクリプトの絶対パスだけを保存
6. テスト一覧で実行したいテストを複数選択
7. Start selected tests
8. 保存済みパスを順番に実行
```

## Current Behavior

- エディタでは行全体をハイライトせず、現在カーソル位置だけを下線表示します。
- エディタ以外の画面では、`Esc` でTUIを終了してターミナルへ戻ります。
- エディタ画面では、`Esc` で画面を閉じません。`:wq` で保存、`:q` で破棄します。
- `after_reboot` のようなカテゴリ分けは行いません。
- Cleanup と Reboot Plan は手動作成せず、`:wq` 保存時にテスト本文から自動生成します。
- `/tmp/...` または `/var/tmp/...` を見つけた場合、cleanup に `rm -rf` を自動追加します。
- `reboot`、`systemctl reboot`、`shutdown -r`、`init 6` を見つけた場合、reboot test と自動判定します。
- 選択済みテストにreboot testがある場合、`autotest_selected_runner.sh`、`autotest_selected_resume.sh`、`autotest-selected-resume.service` を自動生成し、再起動後に残りのテストを自動実行します。
- reboot testを含む自動実行は systemd unit を登録するため root 権限が必要です。
- テスト保存時は、TUIを起動したディレクトリに `autotest_<id>.sh` を作成します。
- TUIは生成したテストスクリプトのパスを各テストに保存します。
- 複数選択の自動化では、選択済みテストの保存済みパスを順番に実行します。
- 再起動を含むテストが実行された場合、順次実行はそこで中断される可能性があります。

## 生成物

TUI は、入力されたテスト定義から以下を生成します。

```text
output/
  test_example.sh              # メインテストスクリプト
  test_example_resume.sh       # 再起動後に続きから実行するスクリプト
  test_example_cleanup.sh      # 後処理だけを行うスクリプト
  test_example.env             # 変数定義
  README_test_example.md       # 実行方法メモ
```

MVP では最低限 `test_example.sh` と `test_example_cleanup.sh` を生成します。再起動を含む場合のみ `test_example_resume.sh` を生成します。

## 結果判定

テスト結果は `OK` または `NG` の 2 値です。

基本ルール:

- コマンド終了コードが期待値と一致したら終了コード判定は成功。
- 通常は `0` が成功、`0` 以外が失敗。
- 出力判定が設定されている場合、終了コード判定と出力判定の両方が成功したら `OK`。
- どちらかが失敗したら `NG`。

生成スクリプト内の表示例:

```text
[OK] TC001 service is active
[NG] TC002 config format check
```

## 判定要素

### 終了コード

`echo $?` で確認できる値を使います。生成スクリプトでは、コマンド実行直後の `$?` を変数に保存して判定します。

```sh
actual_exit=$?
if [ "$actual_exit" -eq "$expected_exit" ]; then
  exit_result="OK"
else
  exit_result="NG"
fi
```

設定項目:

- `expected_exit`: 期待する終了コード。通常は `0`。
- `allow_nonzero`: 異常系テストで `0` 以外を期待する場合に使う。

### 出力の完全一致

標準出力または標準エラーが、期待値と完全に一致するかを判定します。

用途:

- 固定文字列を返すコマンド
- バージョンや設定値など、完全一致が必要な確認

例:

```text
expected_stdout: active
match_type: exact
```

### 出力の部分一致

期待文字列を含むかどうかを判定します。

用途:

- ログや複数行出力の一部を確認する
- 余分な情報が混ざるコマンドを確認する

例:

```text
expected_stdout: running
match_type: contains
```

### フォーマット一致

正規表現で出力形式を判定します。

用途:

- 日付、時刻、PID、IP アドレスなど値が毎回変わる出力
- 数値や列構造の確認
- コマンド出力の形式だけを確認する

例:

```text
expected_stdout: '^[0-9]+ +/usr/bin/myapp$'
match_type: regex
```

### 行単位一致

複数行出力について、指定行だけを比較します。

用途:

- 1 行目だけを見る
- 特定列だけを見る
- ヘッダ行を無視する

### 空出力判定

出力が空であること、または空でないことを判定します。

用途:

- エラーが出ていないことを確認する
- 検索結果が存在することを確認する

## テストケース定義

1 つのテストケースは以下の情報を持ちます。

```yaml
id: TC001
title: service is active
command: systemctl is-active myapp
expected_exit: 0
stdout:
  target: stdout
  match_type: exact
  expected: active
stderr:
  target: stderr
  match_type: empty
cleanup: ""
```

最小構成:

```yaml
id: TC001
title: command succeeds
command: /usr/bin/my_command
expected_exit: 0
```

## 再起動を含むテスト

TUI は再起動を直接実行するツールではなく、再起動前後に分割されたスクリプトを生成します。

生成方針:

- 再起動前に実行するテストを `test_example.sh` に出力する。
- 再起動後に実行するテストを `test_example_resume.sh` に出力する。
- 必要に応じて systemd unit 登録コマンドをスクリプトに含める。
- 再起動後の自動実行が不要な場合は、手動実行手順だけを README に出力する。

再起動ありスクリプトの流れ:

```text
1. 前処理
2. 再起動前テスト
3. resume script の配置
4. 必要なら systemd unit を作成
5. reboot
6. 起動後に resume script を実行
7. 再起動後テスト
8. 後処理
9. OK/NG サマリを表示
```

## 後処理

後処理は生成スクリプトに明示的に出力します。

種類:

- `per_test_cleanup`: 各テストケースの後に行う処理
- `final_cleanup`: 全テスト終了後に行う処理
- `reboot_cleanup`: 再起動後の自動実行設定を戻す処理
- `failure_cleanup`: `NG` 発生時にも行う処理

生成スクリプトでは `trap` を使い、途中終了時にも後処理が走る形を基本にします。

```sh
cleanup() {
  # generated cleanup commands
  :
}

trap cleanup EXIT
```

## スクリプト出力スタイル

生成スクリプトは、Linux 標準環境で読みやすく直しやすい POSIX sh 寄りの Bash とします。

基本方針:

- `#!/usr/bin/env bash`
- テストごとに関数を生成
- 終了コードはコマンド直後に保存
- stdout/stderr は一時ファイルへ分離保存
- 結果は `[OK]` / `[NG]` で表示
- 最後に合計件数、OK 件数、NG 件数を表示
- NG が 1 件でもあればスクリプトの最終終了コードは `1`

生成例:

```sh
run_test() {
  id="$1"
  title="$2"
  command="$3"
  expected_exit="$4"

  stdout_file="$WORK_DIR/${id}.stdout"
  stderr_file="$WORK_DIR/${id}.stderr"

  bash -c "$command" >"$stdout_file" 2>"$stderr_file"
  actual_exit=$?

  if [ "$actual_exit" -eq "$expected_exit" ]; then
    echo "[OK] $id $title"
  else
    echo "[NG] $id $title expected_exit=$expected_exit actual_exit=$actual_exit"
    NG_COUNT=$((NG_COUNT + 1))
  fi
}
```

## TUI の主な機能

- テストスクリプトプロジェクト作成
- Create test
- vim 風の独自エディタによるテストスクリプト作成
- 作成済みテストの複数選択
- 選択済みテストの自動テスト開始
- テストケース追加/編集/削除
- 期待終了コードの設定
- stdout/stderr の期待値設定
- 完全一致、部分一致、正規表現、空出力、非空出力の選択
- 再起動前/再起動後のフェーズ分け
- 独自 reboot command
- 独自 vim command
- 後処理コマンドの設定
- 生成スクリプトのプレビュー
- 危険操作の警告
- スクリプト出力

## 操作方式

操作はメニュー選択式にします。英字キーのショートカットを覚えて操作するのではなく、画面内の `Action Menu` から項目を選び、`Enter` で実行します。

基本操作:

- `Up` / `Down`: メニュー項目やリストを移動
- `Left` / `Right`: ペイン、タブ、選択肢を移動
- `Tab`: 次の操作領域へ移動
- `Enter`: 選択中の項目を決定
- `Esc`: 戻る/キャンセル
- `F1`: ヘルプ

## データ保存案

```text
~/.local/share/autotest-script-builder/
  projects/
    service-reboot-check.yaml
  generated/
    service-reboot-check/
      test_service_reboot_check.sh
      test_service_reboot_check_resume.sh
      test_service_reboot_check_cleanup.sh
      README_service_reboot_check.md
```

## MVP 範囲

最初の実装では以下までを目標にします。

- プロジェクト一覧
- テストケース新規作成
- command, expected_exit, stdout/stderr 判定の編集
- 判定方式: exact, contains, regex, empty, not_empty
- cleanup の編集
- reboot フェーズの選択
- Bash スクリプト生成
- 生成スクリプトプレビュー

TUI 自身によるテスト実行や実行ログ管理は MVP の主目的から外します。必要な場合は「生成したスクリプトを試し実行する」補助機能として後から追加します。

## Build

```bash
make
```

ncurses の開発パッケージが必要です。

Debian/Ubuntu 系:

```bash
apt-get update
apt-get install -y build-essential libncurses-dev
```

## Run

```bash
./autotest-builder
```

## Current Implementation

現時点の MVP 実装は `autotest_builder.c` です。

- メニュー選択式 TUI
- `Create test` 後にvim風の独自エディタを開く
- エディタ操作: `i` で入力、`Esc` でノーマル、`:wq` で保存、`:q` で破棄
- テストケースの追加/編集/削除
- `expected_exit` の設定
- stdout/stderr の `none`, `exact`, `contains`, `regex`, `empty`, `not_empty`
- 再起動前/後フェーズの設定
- cleanup コマンドの設定
- Bash スクリプトプレビュー
- `output/` への main/resume/cleanup/README 生成
