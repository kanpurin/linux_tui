# AutoTest Assist Feature Test Plan

This test plan verifies the current tool without modifying the program source.

## Scope

The TUI is interactive, so the tests are split into two groups:

- Static implementation checks: verify that source-level feature hooks exist.
- Runtime contract checks: execute small shell harnesses that mirror generated
  script behavior.

## Feature Matrix

| Feature | Why it matters | Test method |
| --- | --- | --- |
| Build | The TUI must compile on Linux. | Run `make clean && make`. |
| Title-based script name | Generated scripts should be named from the test title, not only the test id. | Static check that filename generation uses `tc->title` with fallback to `tc->id`, without an `autotest_` prefix. |
| Script name collision | Saving should not overwrite another test with the same title-derived name. | Static check that title input rejects names whose generated script path already exists, and save still keeps a collision guard. |
| Script rename | Users should be able to rename a test later by changing its title. | Static check that saved script paths are recalculated from title and old script/result files are removed after rename. |
| Rename menu | Existing test names must be changeable from the test list. | Static check that the test list exposes `Rename test` and calls the rename flow. |
| Copy menu | Existing test content should be reusable as a new test. | Static check that the test list exposes `Copy test`, duplicates the selected case, prompts for a new title, checks name collisions, and writes a new script. |
| Test registry | Tests from different directories must remain visible. | Check that `~/.config/autotest-assist/tests.tsv` is used and `script_path` is saved. |
| Delete test cleanup | Deleting a test should delete generated script/result files. | Check `delete_case` removes `script_path` and `.result`. |
| Variable assertion | OK/NG should be based on script variables, not stdout/stderr. | Runtime test for `match_value` with exact/contains/regex/empty/not_empty. |
| Multiple variable checks | A test may need several independent assertions. | Runtime test that all checks must pass. |
| Match editor persistence | `Set match` and `Set expected` must persist even when the script body has `@check`. | Static check that UI changes synchronize the primary `@check` directive before script regeneration. |
| Detailed result option | Users need a full result artifact beyond terminal OK/NG output. | Static check that generated scripts parse `--detail-result`, write detailed stdout/stderr/check data, preserve the path across reboot resume, and reject unknown options with usage. |
| `@assert` | Fail early at a meaningful point in the script. | Runtime test for true and false conditions. |
| `@backup` / `@restore` | Safely mutate and restore files, including missing paths. | Runtime test for existing file and missing file restoration. |
| `@reboot-if` | Conditional reboot must not break shell `if` blocks. | Runtime test for false branch without reboot; static check for true branch support. |
| `@tui` automation | Text TUI operations can be scripted. | Static check for `send`, `text`, `send-shell`, `text-shell`, `enter`, `esc`, `tab`, `space`, `sleep`, `ctrl`. |
| `@tui` output capture | Assertions may need to inspect what the TUI session printed without terminal control bytes. | Static check that generated scripts save raw `AUTOTEST_TUI_STDOUT`, cleaned `AUTOTEST_TUI_TEXT`, `AUTOTEST_TUI_STDERR`, status, and backing files. |
| Editor `:help` | Users need an in-editor reference for custom script directives and commands. | Static check that `:help` opens the editor help overlay and supports scrolling. |
| Editor line commands | Editing must be efficient enough for scripts. | Static check for `dd/dNd`, `yy/yNy`, `p/P`, `u`, `G/gg/nG`, `/`, `n`, `N`, and Home/End handling in insert mode. |
| Reboot/rescue limitations docs | Some commands are unreliable during boot transitions. | Check README documents fragile commands such as `runlevel`. |

## Notes

- The default test runner does not reboot the machine.
- Reboot behavior is represented by the safe `@reboot-if` false branch and static
  checks for generated resume logic.
- Manual reboot tests can still be run separately on a disposable VM.
