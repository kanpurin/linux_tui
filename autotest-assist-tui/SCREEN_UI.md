# Screen UI

AutoTest Builder TUI の画面設計です。主導線は `Create test`、保存済みパス、複数選択、`Start selected tests` です。

## 操作方針

| Key | Action |
| --- | --- |
| `Up` / `Down` | リストまたはフィールドを移動 |
| `Left` / `Right` | Action Menu の項目を移動 |
| `Tab` | 次のメニュー項目へ移動 |
| `Enter` | 選択中の項目を実行 |
| `Esc` | エディタ以外ではTUIを終了してターミナルへ戻る |
| `F1` | ヘルプ |

## Dashboard

```text
+ AutoTest Builder ----------------------------------------------- Test List --+
| Tests                       | Summary                                      |
| > * TC001 service active    | generated: current directory                |
|   * TC002 version format    | selected: 2                                  |
|     TC003 reboot            | cleanup/reboot: auto                         |
|                             | result: OK / NG, final exit 0 / 1            |
|                                                                              |
| Action Menu                                                                 |
| > Open test list   Create test   Preview selected   Start selected tests     |
+------------------------------------------------------------------------------+
```

## Test List

```text
+ Tests ---------------------------------------------------------- Saved: yes ---+
| Test Cases                    | Details                                      |
| > * TC001 service is active   | kind: shell  selected: yes                   |
|   * TC002 version format      | command: systemctl is-active myapp          |
|     TC003 reboot              | expected_exit: 0                             |
|                               | stdout: exact "active"                       |
|                               | stderr: empty                                |
|                               | cleanup: auto                                |
|                               | path: /work/autotest_tc001.sh                |
|                                                                              |
| Selection / Warnings                                                         |
| selected: 2  cleanup/reboot: auto                                            |
| selected reboot tests may interrupt sequential execution                     |
|                                                                              |
| Action Menu                                                                 |
| > Edit test  Select test  Delete test  Output match  Preview selected        |
|   Start selected tests  Back                                                 |
+------------------------------------------------------------------------------+
```

`Create test` は Dashboard にだけ置きます。Test List では作成済みテストの編集、選択、削除、出力判定、プレビュー、開始だけを扱います。

## Vim-Like Test Script Editor

```text
+ Create Test Script Editor ------------------------------------------- TC004 --+
| 001 echo "setup"                                                       |
| 002 {                                                                 |
| 003   sleep 0.3                                                       |
| 004   printf 'gg'                                                     |
| 005   printf 'dG'                                                     |
| 006   printf 'i'                                                      |
| 007   printf 'new text\n'                                             |
| 008   printf '\033'                                                   |
| 009   printf ':wq\r'                                                  |
| 010 } | script -q -c "vim -Nu NONE -n /tmp/testfile" /dev/null ...    |
|                                                                      |
| -- INSERT --  Arrow move  i insert  Esc normal  :wq save  :q cancel  |
+------------------------------------------------------------------------------+
```

現在行全体はハイライトしません。現在のカーソル位置だけを下線で表示します。

エディタ操作:

- `i`: 入力モード
- `Esc`: ノーマルモード。画面は閉じません。
- `h` / `j` / `k` / `l` または矢印: 移動
- `x`: 1文字削除
- `o`: 下に新しい行を作って入力
- `:wq`: 保存してテスト一覧へ戻る
- `:q`: 破棄してテスト一覧へ戻る

`:q` で破棄した新規テストは Test List に残しません。

## Auto Cleanup And Reboot

Cleanup と Reboot Plan は手動作成しません。`:wq` 保存時に、テストスクリプト本文から自動生成します。

```text
+ Auto Plan ------------------------------------------------------------------+
| Test: TC004                                                                  |
| cleanup: rm -rf '/tmp/testfile'                                              |
| reboot: no                                                                   |
| reason: /tmp/testfile was found in the script body                           |
+------------------------------------------------------------------------------+
```

自動生成ルール:

- `/tmp/...` または `/var/tmp/...` のパスを見つけたら、cleanup に `rm -rf '<path>'` を追加します。
- `reboot`、`systemctl reboot`、`shutdown -r`、`init 6` を見つけたら reboot test と判定します。
- `after_reboot` のようなカテゴリ分けは行いません。再起動する可能性がある単体テストとして扱います。

## Start Selected Tests

`Start selected tests` は、選択済みテストが保持している保存済みパスを順番に実行します。未保存のテストがあれば、先にカレントディレクトリへ単体テストスクリプトを生成してから実行します。

選択済みテストに `reboot` を含むテストがある場合は、TUIが以下を自動生成します。

- `autotest_selected_runner.sh`: reboot前までのテストを実行し、resume用systemd unitを登録する
- `autotest_selected_resume.sh`: reboot後の残りテストを順番に実行する
- `autotest-selected-resume.service`: 起動後にresume scriptを一度だけ実行するsystemd unit

この場合、`Start selected tests` は root 権限が必要です。
