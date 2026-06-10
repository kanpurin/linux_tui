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
| Test registry | Tests from different directories must remain visible. | Check that `~/.config/autotest-assist/tests.tsv` is used and `script_path` is saved. |
| Delete test cleanup | Deleting a test should delete generated script/result files. | Check `delete_case` removes `script_path` and `.result`. |
| Variable assertion | OK/NG should be based on script variables, not stdout/stderr. | Runtime test for `match_value` with exact/contains/regex/empty/not_empty. |
| Multiple variable checks | A test may need several independent assertions. | Runtime test that all checks must pass. |
| `@assert` | Fail early at a meaningful point in the script. | Runtime test for true and false conditions. |
| `@backup` / `@restore` | Safely mutate and restore files, including missing paths. | Runtime test for existing file and missing file restoration. |
| `@reboot-if` | Conditional reboot must not break shell `if` blocks. | Runtime test for false branch without reboot; static check for true branch support. |
| `@tui` automation | Text TUI operations can be scripted. | Static check for `send`, `text`, `send-shell`, `text-shell`, `enter`, `esc`, `tab`, `space`, `sleep`, `ctrl`. |
| Editor line commands | Editing must be efficient enough for scripts. | Static check for `dd/dNd`, `yy/yNy`, `p/P`, `u`, `G/gg/nG`, `/`, `n`, `N`. |
| Reboot/rescue limitations docs | Some commands are unreliable during boot transitions. | Check README documents fragile commands such as `runlevel`. |

## Notes

- The default test runner does not reboot the machine.
- Reboot behavior is represented by the safe `@reboot-if` false branch and static
  checks for generated resume logic.
- Manual reboot tests can still be run separately on a disposable VM.
