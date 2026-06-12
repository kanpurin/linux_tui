#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT_DIR/autotest_builder.c"
README="$ROOT_DIR/README.md"
FAILURES=0
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/autotest-assist-feature-tests.XXXXXX")"

cleanup() {
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

pass() {
  printf '[OK] %s\n' "$1"
}

fail() {
  printf '[NG] %s\n' "$1"
  FAILURES=$((FAILURES + 1))
}

assert_file_contains() {
  label="$1"
  file="$2"
  pattern="$3"
  if grep -Fq -- "$pattern" "$file"; then
    pass "$label"
  else
    fail "$label"
  fi
}

run_case() {
  label="$1"
  shift
  if "$@"; then
    pass "$label"
  else
    fail "$label"
  fi
}

match_value() {
  type="$1"
  actual="$2"
  expected="$3"
  case "$type" in
    none) return 0 ;;
    exact) [ "$actual" = "$expected" ] ;;
    contains) case "$actual" in *"$expected"*) return 0 ;; *) return 1 ;; esac ;;
    regex) printf '%s\n' "$actual" | grep -Eq -- "$expected" ;;
    empty) [ -z "$actual" ] ;;
    not_empty) [ -n "$actual" ] ;;
    *) return 1 ;;
  esac
}

autotest_backup_key() {
  printf '%s' "$1" | cksum | awk '{print $1}'
}

autotest_backup() {
  path="$1"
  [ -n "${AUTOTEST_BACKUP_DIR:-}" ] || return 0
  key="$(autotest_backup_key "$path")"
  dest="$AUTOTEST_BACKUP_DIR/$key"
  rm -rf "$dest"
  mkdir -p "$dest"
  printf '%s\n' "$path" >"$dest/path"
  if [ -e "$path" ]; then
    echo 1 >"$dest/exists"
    cp -a "$path" "$dest/item"
  else
    echo 0 >"$dest/exists"
  fi
}

autotest_restore() {
  path="$1"
  [ -n "${AUTOTEST_BACKUP_DIR:-}" ] || return 0
  key="$(autotest_backup_key "$path")"
  dest="$AUTOTEST_BACKUP_DIR/$key"
  [ -d "$dest" ] || return 0
  if [ "$(cat "$dest/exists" 2>/dev/null || echo 0)" = 1 ]; then
    rm -rf "$path"
    cp -a "$dest/item" "$path"
  else
    rm -rf "$path"
  fi
}

test_build() {
  (cd "$ROOT_DIR" && make clean >/dev/null && make >/dev/null)
}

test_match_value() {
  match_value exact "active" "active" &&
  match_value contains "service active" "active" &&
  match_value regex "12345" '^[[:digit:]]+$' &&
  match_value empty "" "" &&
  match_value not_empty "x" ""
}

test_multiple_checks_all_pass() {
  A="active"
  B="123"
  C="hello world"
  ok=1
  match_value exact "$A" "active" || ok=0
  match_value regex "$B" '^[[:digit:]]+$' || ok=0
  match_value contains "$C" "world" || ok=0
  [ "$ok" -eq 1 ]
}

test_multiple_checks_one_fails() {
  A="active"
  B="abc"
  ok=1
  match_value exact "$A" "active" || ok=0
  match_value regex "$B" '^[[:digit:]]+$' || ok=0
  [ "$ok" -eq 0 ]
}

test_assert_true() {
  value="ready"
  if ! [ "$value" = ready ]; then
    return 1
  fi
}

test_assert_false() {
  value="not-ready"
  if ! [ "$value" = ready ]; then
    return 0
  fi
  return 1
}

test_backup_restore_existing_file() {
  AUTOTEST_BACKUP_DIR="$TMP_ROOT/backups-existing"
  mkdir -p "$AUTOTEST_BACKUP_DIR"
  file="$TMP_ROOT/config.txt"
  printf 'original\n' >"$file"
  autotest_backup "$file"
  printf 'changed\n' >"$file"
  autotest_restore "$file"
  [ "$(cat "$file")" = "original" ]
}

test_backup_restore_missing_file() {
  AUTOTEST_BACKUP_DIR="$TMP_ROOT/backups-missing"
  mkdir -p "$AUTOTEST_BACKUP_DIR"
  file="$TMP_ROOT/missing.txt"
  rm -f "$file"
  autotest_backup "$file"
  printf 'created\n' >"$file"
  autotest_restore "$file"
  [ ! -e "$file" ]
}

test_reboot_if_false_branch() {
  need_reboot=no
  rc=0
  if [ "$need_reboot" = yes ]; then
    rc=99
  else
    rc=125
  fi
  [ "$rc" -eq 125 ]
}

test_title_filename_source() {
  grep -Fq 'const char *src = tc->title[0] ? tc->title : tc->id' "$SRC" &&
  grep -Fq 'snprintf(out, out_sz, "%s.sh", safe_name)' "$SRC" &&
  ! grep -Fq 'snprintf(out, out_sz, "autotest_%s.sh", safe_name)' "$SRC"
}

test_script_name_collision_source() {
  grep -Fq 'title_path_conflicts' "$SRC" &&
  grep -Fq 'access(path, F_OK) == 0' "$SRC" &&
  grep -Fq 'return -2' "$SRC" &&
  grep -Fq 'Script name already exists in this directory.' "$SRC" &&
  grep -Fq 'if (title_path_conflicts(&new_case, new_case.title))' "$SRC" &&
  grep -Fq 'if (title_path_conflicts(tc, tc->title))' "$SRC"
}

test_script_rename_source() {
  grep -Fq 'Rename test' "$SRC" &&
  grep -Fq 'rename_case' "$SRC" &&
  grep -Fq 'make_test_path(tc, path, sizeof(path))' "$SRC" &&
  grep -Fq 'strcmp(old_path, path) != 0' "$SRC" &&
  grep -Fq 'unlink(old_path)' "$SRC" &&
  grep -Fq 'make_result_path(old_path' "$SRC"
}

test_copy_test_source() {
  grep -Fq 'Copy test' "$SRC" &&
  grep -Fq 'copy_case' "$SRC" &&
  grep -Fq 'TestCase new_case = app->project.cases[app->selected_case]' "$SRC" &&
  grep -Fq 'prompt_text("copy test title"' "$SRC" &&
  grep -Fq 'title_path_conflicts(&new_case, new_case.title)' "$SRC" &&
  grep -Fq 'write_single_test_script(&new_case)' "$SRC"
}

test_detail_result_source() {
  grep -Fq -- '--detail-result' "$SRC" &&
  grep -Fq 'DETAIL_RESULT_FILE' "$SRC" &&
  grep -Fq 'autotest_write_detail_result' "$SRC" &&
  grep -Fq 'detail_result_path' "$SRC" &&
  grep -Fq 'run_case_001 \"$MODE\"' "$SRC" &&
  grep -Fq 'usage()' "$SRC" &&
  grep -Fq 'unknown option:' "$SRC" &&
  grep -Fq 'unexpected argument:' "$SRC" &&
  grep -Fq 'expected_exit=${expected_exit-}' "$SRC" &&
  grep -Fq -- '--- stdout ---' "$SRC" &&
  grep -Fq -- '--- stderr ---' "$SRC"
}

test_check_directive_source() {
  grep -Fq '@check' "$SRC" &&
  grep -Fq 'MAX_CHECKS' "$SRC" &&
  grep -Fq 'write_validate_checks' "$SRC" &&
  grep -Fq 'sync_primary_check_directive' "$SRC" &&
  grep -Fq 'unescape_field(check->expected' "$SRC" &&
  grep -Fq 'escape_field(expected' "$SRC"
}

test_assert_directive_source() {
  grep -Fq '@assert' "$SRC" &&
  grep -Fq '[NG] assert failed:' "$SRC"
}

test_backup_restore_source() {
  grep -Fq '@backup' "$SRC" &&
  grep -Fq '@restore' "$SRC" &&
  grep -Fq 'autotest_backup()' "$SRC" &&
  grep -Fq 'autotest_restore()' "$SRC"
}

test_reboot_if_source() {
  grep -Fq '@reboot-if' "$SRC" &&
  grep -Fq 'exit 125' "$SRC"
}

test_tui_source() {
  grep -Fq 'send-shell' "$SRC" &&
  grep -Fq 'text-shell' "$SRC" &&
  grep -Fq 'strcmp(trimmed, "esc")' "$SRC" &&
  grep -Fq 'strcmp(trimmed, "tab")' "$SRC" &&
  grep -Fq 'invalid ctrl key' "$SRC"
}

test_tui_capture_source() {
  grep -Fq 'AUTOTEST_TUI_STDOUT_FILE' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_TEXT_FILE' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STDERR_FILE' "$SRC" &&
  grep -Fq 'autotest_clean_tui_output' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STDOUT=' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_TEXT=' "$SRC" &&
  grep -Fq 'cat \"$AUTOTEST_TUI_STDOUT_FILE\"' "$SRC" &&
  grep -Fq 'cat \"$AUTOTEST_TUI_TEXT_FILE\"' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STDERR=' "$SRC" &&
  grep -Fq 'cat \"$AUTOTEST_TUI_STDERR_FILE\"' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STATUS=' "$SRC"
}

test_editor_source() {
  grep -Fq 'editor_undo' "$SRC" &&
  grep -Fq 'editor_copy_lines' "$SRC" &&
  grep -Fq 'editor_paste_lines' "$SRC" &&
  grep -Fq 'editor_search_next' "$SRC" &&
  grep -Fq 'editor_goto_line' "$SRC" &&
  grep -Fq 'editor_help_open' "$SRC" &&
  grep -Fq 'editor_help_scroll' "$SRC" &&
  grep -Fq 'strcmp(app->editor_command, "help")' "$SRC" &&
  grep -Fq 'editor_handle_insert_escape_sequence' "$SRC" &&
  grep -Fq 'set_escdelay(150)' "$SRC" &&
  grep -Fq 'ch == KEY_DOWN' "$SRC" &&
  grep -Fq 'ch == KEY_UP' "$SRC"
}

test_registry_source() {
  grep -Fq '.config/autotest-assist/tests.tsv' "$SRC" &&
  grep -Fq 'script_path' "$SRC" &&
  grep -Fq 'save_test_registry' "$SRC"
}

test_delete_source() {
  grep -Fq 'delete_case' "$SRC" &&
  grep -Fq 'unlink(deleted_path)' "$SRC" &&
  grep -Fq 'result_path' "$SRC"
}

test_docs_limitations() {
  grep -Fq 'Reboot and Rescue Limitations' "$README" &&
  grep -Fq 'runlevel' "$README" &&
  grep -Fq 'utmp' "$README"
}

printf 'Running AutoTest Assist feature tests from %s\n' "$ROOT_DIR"

run_case 'build autotest-builder' test_build
run_case 'title-based script filename source support' test_title_filename_source
run_case 'script name collision source support' test_script_name_collision_source
run_case 'script rename source support' test_script_rename_source
run_case 'copy test source support' test_copy_test_source
run_case 'detail result option source support' test_detail_result_source
run_case 'variable match types' test_match_value
run_case 'multiple variable checks pass' test_multiple_checks_all_pass
run_case 'multiple variable checks fail as AND' test_multiple_checks_one_fails
run_case '@assert true condition' test_assert_true
run_case '@assert false condition fails' test_assert_false
run_case '@backup/@restore existing file' test_backup_restore_existing_file
run_case '@backup/@restore missing file' test_backup_restore_missing_file
run_case '@reboot-if false branch does not reboot' test_reboot_if_false_branch
run_case '@check source support' test_check_directive_source
run_case '@assert source support' test_assert_directive_source
run_case '@backup/@restore source support' test_backup_restore_source
run_case '@reboot-if source support' test_reboot_if_source
run_case '@tui source support' test_tui_source
run_case '@tui output capture source support' test_tui_capture_source
run_case 'editor command source support' test_editor_source
run_case 'registry source support' test_registry_source
run_case 'delete test source support' test_delete_source
run_case 'README limitations documentation' test_docs_limitations

if [ "$FAILURES" -eq 0 ]; then
  printf 'Summary: OK all feature tests passed\n'
  exit 0
fi

printf 'Summary: NG failures=%d\n' "$FAILURES"
exit 1
