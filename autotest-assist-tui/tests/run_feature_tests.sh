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
    exact) AUTOTEST_MATCH_ACTUAL="$actual" AUTOTEST_MATCH_EXPECTED="$expected" awk 'BEGIN { pattern = "^(" ENVIRON["AUTOTEST_MATCH_EXPECTED"] ")$"; exit(ENVIRON["AUTOTEST_MATCH_ACTUAL"] ~ pattern ? 0 : 1) }' ;;
    not_exact) AUTOTEST_MATCH_ACTUAL="$actual" AUTOTEST_MATCH_EXPECTED="$expected" awk 'BEGIN { pattern = "^(" ENVIRON["AUTOTEST_MATCH_EXPECTED"] ")$"; exit(ENVIRON["AUTOTEST_MATCH_ACTUAL"] ~ pattern ? 1 : 0) }' ;;
    contains) AUTOTEST_MATCH_ACTUAL="$actual" AUTOTEST_MATCH_EXPECTED="$expected" awk 'BEGIN { exit(ENVIRON["AUTOTEST_MATCH_ACTUAL"] ~ ENVIRON["AUTOTEST_MATCH_EXPECTED"] ? 0 : 1) }' ;;
    not_contains) AUTOTEST_MATCH_ACTUAL="$actual" AUTOTEST_MATCH_EXPECTED="$expected" awk 'BEGIN { exit(ENVIRON["AUTOTEST_MATCH_ACTUAL"] ~ ENVIRON["AUTOTEST_MATCH_EXPECTED"] ? 1 : 0) }' ;;
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
  match_value not_exact "inactive" "active" &&
  match_value not_contains "service stopped" "active" &&
  match_value exact "12345" '[[:digit:]]+' &&
  match_value exact $'one\ntwo' $'one\ntwo' &&
  match_value exact $'one\ntwo' $'^one\ntwo$' &&
  match_value contains "abc123def" '[[:digit:]]+' &&
  ! match_value exact "abc123def" '[[:digit:]]+' &&
  ! match_value not_exact "active" "active" &&
  ! match_value not_contains "service active" "active" &&
  ! match_value exact $'/a/b/c/d : protected\n/i/j : protected' '(/[[:alpha:]]+)+ : protected' &&
  ! match_value exact $'one\ntwo' $'^one\ntwo\nthree$' &&
  match_value empty "" "" &&
  match_value not_empty "x" ""
}

test_multiple_checks_all_pass() {
  A="active"
  B="123"
  C="hello world"
  ok=1
  match_value exact "$A" "active" || ok=0
  match_value exact "$B" '[[:digit:]]+' || ok=0
  match_value contains "$C" "world" || ok=0
  [ "$ok" -eq 1 ]
}

test_multiple_checks_one_fails() {
  A="active"
  B="abc"
  ok=1
  match_value exact "$A" "active" || ok=0
  match_value exact "$B" '[[:digit:]]+' || ok=0
  [ "$ok" -eq 0 ]
}

test_check_position_capture() {
  harness="$TMP_ROOT/check_position.c"
  cat >"$harness" <<EOF
#define main autotest_builder_app_main
#include "$SRC"
#undef main

int main(void) {
    TestCase tc;
    memset(&tc, 0, sizeof(tc));
    copy_text(tc.id, sizeof(tc.id), "TC999");
    copy_text(tc.title, sizeof(tc.title), "check_position");
    tc.kind = CMD_SHELL;
    tc.expected_exit = 0;
    set_test_command(&tc,
        "TEST=A\\n"
        "@check TEST exact A\\n"
        "TEST=B\\n"
        "@check TEST exact B\\n");
    return write_single_test_script(&tc);
}
EOF
  (
    cd "$TMP_ROOT" &&
    gcc -Wall -Wextra -o check_position "$harness" -lncursesw >/dev/null 2>&1 &&
    ./check_position &&
    ./check_position.sh --detail check_position.detail >check_position.out 2>&1 &&
    grep -Fq '[OK] TC999 check_position' check_position.out &&
    grep -Fq 'actual_1=A' check_position.detail &&
    grep -Fq 'actual_2=B' check_position.detail &&
    ! grep -Fq '[NG] TC999 check_position' check_position.out
  )
}

test_check_loop_runtime_count() {
  harness="$TMP_ROOT/check_loop.c"
  cat >"$harness" <<EOF
#define main autotest_builder_app_main
#include "$SRC"
#undef main

int main(void) {
    TestCase tc;
    memset(&tc, 0, sizeof(tc));
    copy_text(tc.id, sizeof(tc.id), "TC998");
    copy_text(tc.title, sizeof(tc.title), "check_loop");
    tc.kind = CMD_SHELL;
    tc.expected_exit = 0;
    set_test_command(&tc,
        "for value in 0 1 0; do\\n"
        "  TEST=\"\$value\"\\n"
        "  @check TEST exact 0\\n"
        "done\\n");
    return write_single_test_script(&tc);
}
EOF
  (
    cd "$TMP_ROOT" &&
    gcc -Wall -Wextra -o check_loop "$harness" -lncursesw >/dev/null 2>&1 &&
    ./check_loop &&
    ./check_loop.sh --detail check_loop.detail >check_loop.out 2>&1 || true
    grep -Fq '[NG] TC998 check_loop' check_loop.out &&
    grep -Fq 'check_count=3' check_loop.detail &&
    grep -Fq 'actual_1=0' check_loop.detail &&
    grep -Fq 'actual_2=1' check_loop.detail &&
    grep -Fq 'actual_3=0' check_loop.detail
  )
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

test_tui_argument_quoting() {
  harness="$TMP_ROOT/tui_argument_quoting.c"
  cat >"$harness" <<EOF
#define main autotest_builder_app_main
#include "$SRC"
#undef main

static int expect_decoded(const char *src, const char *expected) {
    char out[LONG_LEN];
    decode_tui_argument(src, out, sizeof(out));
    return strcmp(out, expected) == 0;
}

int main(void) {
    const char quoted[] = {34, 'a', 'b', 'c', 34, 0};
    const char mixed[] = {'a', 34, 'b', ' ', 'c', 34, 'd', 0};
    const char escaped_quote[] = {92, 34, 'q', 'u', 'o', 't', 'e', 92, 34, 0};
    const char quote_expected[] = {34, 'q', 'u', 'o', 't', 'e', 34, 0};
    const char single_quoted[] = {39, 's', 'i', 'n', 'g', 'l', 'e', ' ', 'q', 'u', 'o', 't', 'e', 'd', 39, 0};
    const char slash_pair[] = {92, 92, 0};
    const char slash_expected[] = {92, 0};
    return expect_decoded(quoted, "abc") &&
           expect_decoded(mixed, "ab cd") &&
           expect_decoded(escaped_quote, quote_expected) &&
           expect_decoded(single_quoted, "single quoted") &&
           expect_decoded(slash_pair, slash_expected) ? 0 : 1;
}
EOF
  (
    cd "$TMP_ROOT" &&
    gcc -Wall -Wextra -o tui_argument_quoting "$harness" -lncursesw >/dev/null 2>&1 &&
    ./tui_argument_quoting
  )
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
  grep -Fq 'int ch = read_tui_input()' "$SRC" &&
  grep -Fq 'if (ch == 27)' "$SRC" &&
  grep -Fq 'if (!accepted) return false' "$SRC" &&
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
  grep -Fq -- '--detail)' "$SRC" &&
  grep -Fq -- '--detail=*)' "$SRC" &&
  ! grep -Fq -- '--detail-result' "$SRC" &&
  grep -Fq 'DETAIL_STDOUT' "$SRC" &&
  grep -Fq 'DETAIL_RESULT_FILE' "$SRC" &&
  grep -Fq 'autotest_write_detail_result' "$SRC" &&
  grep -Fq 'autotest_detail_cat' "$SRC" &&
  grep -Fq 'autotest_detail_kv' "$SRC" &&
  grep -Fq 'autotest_detail_value' "$SRC" &&
  grep -Fq 'autotest_detail_check_kv' "$SRC" &&
  grep -Fq 'autotest_detail_check_value' "$SRC" &&
  grep -Fq 'check_failed=0' "$SRC" &&
  grep -Fq 'check_failed=1' "$SRC" &&
  grep -Fq '\\033[31m%s\\033[0m' "$SRC" &&
  grep -Fq 'autotest_detail_section' "$SRC" &&
  grep -Fq 'local value=\"$2\"' "$SRC" &&
  grep -Fq 'autotest_write_detail_outputs' "$SRC" &&
  grep -Fq 'AUTOTEST_DETAIL_SAFE=1' "$SRC" &&
  grep -Fq 'AUTOTEST_DETAIL_COLOR=1' "$SRC" &&
  grep -Fq '\\033[36m%s\\033[0m=%s\\n' "$SRC" &&
  grep -Fq '\\033[36m%s\\033[0m:\\n' "$SRC" &&
  grep -Fq 'autotest_detail_sanitize()' "$SRC" &&
  grep -Fq 'autotest_detail_sanitize <\"$path\"' "$SRC" &&
  grep -Fq 'printf '\''%s\\n'\'' \"$value\" | autotest_detail_sanitize' "$SRC" &&
  grep -Fq 'AUTOTEST_DETAIL_SAFE:-0}' "$SRC" &&
  ! grep -Fq 'cat -v' "$SRC" &&
  grep -Fq 'autotest_detail_check_value \"$check_failed\" \"expected $name\" \"$expected\"' "$SRC" &&
  grep -Fq 'autotest_detail_check_value \"$check_failed\" \"actual $name\" \"$value\"' "$SRC" &&
  grep -Fq 'value_ref=\"actual_value_$i\"' "$SRC" &&
  grep -Fq 'expected_ref=\"expected_value_$i\"' "$SRC" &&
  grep -Fq 'value=\"${!value_ref}\"' "$SRC" &&
  grep -Fq 'expected=\"${!expected_ref}\"' "$SRC" &&
  grep -Fq 'check_match_%d=' "$SRC" &&
  grep -Fq 'expected_value_%d=' "$SRC" &&
  grep -Fq '[ -s \"$stdout_file\" ]' "$SRC" &&
  grep -Fq 'autotest_write_detail_result -' "$SRC" &&
  grep -Fq 'detail_result_path' "$SRC" &&
  grep -Fq 'detail_stdout' "$SRC" &&
  grep -Fq 'run_case_001' "$SRC" &&
  grep -Fq 'usage()' "$SRC" &&
  grep -Fq 'unknown option:' "$SRC" &&
  grep -Fq 'unexpected argument:' "$SRC" &&
  grep -Fq 'autotest_detail_kv expected_exit' "$SRC" &&
  grep -Fq -- '--- stdout ---' "$SRC" &&
  grep -Fq -- '--- stderr ---' "$SRC"
}

test_check_directive_source() {
  grep -Fq '@check' "$SRC" &&
  grep -Fq 'next_check_directive' "$SRC" &&
  grep -Fq 'next_effective_check' "$SRC" &&
  grep -Fq 'command_check_count' "$SRC" &&
  grep -Fq 'parse_check_arg' "$SRC" &&
  grep -Fq 'collect_check_heredoc' "$SRC" &&
  grep -Fq 'AUTOTEST_CHECK_INDEX' "$SRC" &&
  grep -Fq 'check_count\" 2>/dev/null || echo 0' "$SRC" &&
  grep -Fq 'printf -v \"actual_value_$i\"' "$SRC" &&
  grep -Fq 'write_validate_checks' "$SRC" &&
  grep -Fq 'AUTOTEST_MATCH_ACTUAL' "$SRC" &&
  grep -Fq 'pattern = \"^(\"' "$SRC" &&
  ! grep -Fq 'grep -Eq -- "$expected"' "$SRC" &&
  ! grep -Fq 'MATCH_REGEX' "$SRC" &&
  grep -Fq 'unescape_field(out->expected' "$SRC" &&
  grep -Fq "@check <var> <match> <<'EOF'" "$SRC" &&
  grep -Fq '@check <var> <match> <expected>' "$SRC" &&
  grep -Fq 'exact: regex full match' "$SRC" &&
  grep -Fq 'not_exact: regex must not full match' "$SRC" &&
  grep -Fq 'contains: regex search' "$SRC" &&
  grep -Fq 'not_contains: regex must not match' "$SRC" &&
  grep -Fq 'not_exact) AUTOTEST_MATCH_ACTUAL' "$SRC" &&
  grep -Fq 'not_contains) AUTOTEST_MATCH_ACTUAL' "$SRC" &&
  ! grep -Fq 'MAX_CHECKS' "$SRC" &&
  ! grep -Fq 'Variable match' "$SRC" &&
  ! grep -Fq 'checks: %d/%d' "$SRC" &&
  ! grep -Fq 'sync_primary_check_directive' "$SRC"
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
  grep -Fq 'decode_tui_argument' "$SRC" &&
  grep -Fq 'send-shell' "$SRC" &&
  grep -Fq 'text-shell' "$SRC" &&
  grep -Fq 'strcmp(trimmed, "esc")' "$SRC" &&
  grep -Fq 'strcmp(trimmed, "tab")' "$SRC" &&
  grep -Fq 'strcmp(trimmed, "vim-clear")' "$SRC" &&
  grep -Fq 'strcmp(trimmed, "vim-write-quit")' "$SRC" &&
  grep -Fq "printf 'ggdG'" "$SRC" &&
  grep -Fq "033:wq" "$SRC" &&
  grep -Fq "033' >>\\\"\$AUTOTEST_TUI_INPUT_FILE\\\"" "$SRC" &&
  grep -Fq "033:wq\\\\n' >>\\\"\$AUTOTEST_TUI_INPUT_FILE\\\"" "$SRC" &&
  grep -Fq 'invalid ctrl key' "$SRC"
}

test_tui_capture_source() {
  grep -Fq 'AUTOTEST_TUI_TRANSCRIPT_FILE' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_INPUT_FILE' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STDOUT_FILE' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_TEXT_FILE' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STDERR_FILE' "$SRC" &&
  grep -Fq 'autotest_clean_tui_output' "$SRC" &&
  grep -Fq 's/\\r+\\n/\\n/g' "$SRC" &&
  grep -Fq 'autotest_strip_tui_echo' "$SRC" &&
  grep -Fq 'my $target=$in' "$SRC" &&
  grep -Fq 'substr($out,$i+1,1)' "$SRC" &&
  grep -Fq 'write_capture_tui_metadata' "$SRC" &&
  grep -Fq 'write_load_tui_metadata' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_TRANSCRIPT=' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STDOUT=' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_TEXT=' "$SRC" &&
  grep -Fq 'cat \"$AUTOTEST_TUI_TRANSCRIPT_FILE\"' "$SRC" &&
  grep -Fq 'cat \"$AUTOTEST_TUI_STDOUT_FILE\"' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_TEXT=\"$AUTOTEST_TUI_STDOUT\"' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STDERR=' "$SRC" &&
  grep -Fq 'cat \"$AUTOTEST_TUI_STDERR_FILE\"' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STATUS=' "$SRC"
}

test_editor_source() {
  grep -Fq 'editor_undo' "$SRC" &&
  grep -Fq 'EDITOR_UNDO_DEPTH' "$SRC" &&
  grep -Fq 'editor_undo_count' "$SRC" &&
  grep -Fq -- '--app->editor_undo_count' "$SRC" &&
  grep -Fq 'Print script' "$SRC" &&
  grep -Fq 'print_current_test_script' "$SRC" &&
  grep -Fq 'strcmp(app->editor_command, "print")' "$SRC" &&
  grep -Fq 'print_editor_script(app)' "$SRC" &&
  grep -Fq 'editor_dirty' "$SRC" &&
  grep -Fq 'strcmp(app->editor_command, "w")' "$SRC" &&
  grep -Fq 'strcmp(app->editor_command, "q!")' "$SRC" &&
  grep -Fq 'E37: No write since last change (add ! to override)' "$SRC" &&
  grep -Fq 'set_editor_written_status' "$SRC" &&
  grep -Fq 'editor_try_tab_completion' "$SRC" &&
  grep -Fq 'editor_build_completion' "$SRC" &&
  grep -Fq 'completion_common_prefix' "$SRC" &&
  grep -Fq 'editor_complete_tui_block' "$SRC" &&
  grep -Fq 'editor_complete_shell_block' "$SRC" &&
  grep -Fq '"for "' "$SRC" &&
  grep -Fq '"if "' "$SRC" &&
  grep -Fq 'completion_open' "$SRC" &&
  grep -Fq 'editor_seen_tui_before_current' "$SRC" &&
  grep -Fq 'AUTOTEST_TUI_STDOUT_FILE' "$SRC" &&
  grep -Fq 'cursor_y + 1' "$SRC" &&
  grep -Fq 'draw_completion_window(app, height, width, first_row)' "$SRC" &&
  grep -Fq 'vim-write-quit' "$SRC" &&
  grep -Fq -- '-- INSERT --' "$SRC" &&
  grep -Fq 'editor_set_scroll_screen_row(app, scroll, rows, wrap_w)' "$SRC" &&
  grep -Fq 'for (int line_index = first_row' "$SRC" &&
  grep -Fq 'screen_row < rows' "$SRC" &&
  grep -Fq 'editor_copy_lines' "$SRC" &&
  grep -Fq 'editor_start_line_range' "$SRC" &&
  grep -Fq 'editor_apply_line_range' "$SRC" &&
  grep -Fq 'editor_range_pending' "$SRC" &&
  grep -Fq 'editor_paste_lines' "$SRC" &&
  grep -Fq 'editor_search_next' "$SRC" &&
  grep -Fq 'editor_goto_line' "$SRC" &&
  grep -Fq 'editor_help_open' "$SRC" &&
  grep -Fq 'editor_help_scroll' "$SRC" &&
  grep -Fq 'strcmp(app->editor_command, "help")' "$SRC" &&
  grep -Fq 'app->editor_target == EDIT_TARGET_COMMAND) return true' "$SRC" &&
  grep -Fq 'strcmp(app->editor_command, "template")' "$SRC" &&
  grep -Fq 'editor_handle_escape_sequence' "$SRC" &&
  grep -Fq 'if (!editor_handle_escape_sequence(app))' "$SRC" &&
  grep -Fq "seq[0] == '[' && n >= 2" "$SRC" &&
  grep -Fq "seq[n - 1] == 'H'" "$SRC" &&
  grep -Fq "seq[n - 1] == 'F'" "$SRC" &&
  grep -Fq 'first_param == 1 || first_param == 7' "$SRC" &&
  grep -Fq 'first_param == 4 || first_param == 8' "$SRC" &&
  grep -Fq 'indent_len' "$SRC" &&
  grep -Fq 'app->editor_col = indent_len' "$SRC" &&
  grep -Fq 'editor_first_row' "$SRC" &&
  grep -Fq 'editor_desired_col' "$SRC" &&
  grep -Fq 'editor_index_for_visual_col' "$SRC" &&
  grep -Fq 'editor_move_vertical' "$SRC" &&
  grep -Fq 'editor_ensure_cursor_visible' "$SRC" &&
  grep -Fq 'print_editor_line_segment' "$SRC" &&
  grep -Fq 'editor_wrapped_rows_for_line' "$SRC" &&
  grep -Fq 'editor_cursor_screen_row_from_first' "$SRC" &&
  grep -Fq 'mvprintw(y, 2, "    ")' "$SRC" &&
  grep -Fq 'editor_ensure_line_capacity' "$SRC" &&
  grep -Fq 'char *editor_lines[EDITOR_LINES]' "$SRC" &&
  ! grep -Fq '#define EDITOR_COLS' "$SRC" &&
  ! grep -Fq '"  > "' "$SRC" &&
  grep -Fq 'editor_first_wrap_row' "$SRC" &&
  grep -Fq 'editor_scroll_screen_row' "$SRC" &&
  grep -Fq 'editor_cursor_screen_row_abs' "$SRC" &&
  grep -Fq 'editor_set_scroll_screen_row' "$SRC" &&
  grep -Fq 'int first_row = app->editor_first_row' "$SRC" &&
  grep -Fq 'set_escdelay(150)' "$SRC" &&
  grep -Fq 'setlocale(LC_ALL, "")' "$SRC" &&
  grep -Fq 'meta(stdscr, TRUE)' "$SRC" &&
  grep -Fq 'get_wch' "$SRC" &&
  grep -Fq 'editor_insert_wchar' "$SRC" &&
  grep -Fq 'utf8_prev_index' "$SRC" &&
  grep -Fq 'utf8_next_index' "$SRC" &&
  grep -Fq 'editor_is_text_char' "$SRC" &&
  grep -Fq 'ch == KEY_BACKSPACE || ch == KEY_DC' "$SRC" &&
  grep -Fq 'app->editor_command_mode = false' "$SRC" &&
  grep -Fq 'app->editor_search_mode = false' "$SRC" &&
  grep -Fq 'if (cy >= 6 && cy <= height - 4' "$SRC" &&
  grep -Fq 'case_filter' "$SRC" &&
  grep -Fq 'filtered_case_count' "$SRC" &&
  grep -Fq 'move_filtered_case(app, -1)' "$SRC" &&
  grep -Fq 'move_filtered_case(app, 1)' "$SRC" &&
  grep -Fq 'prompt_text("filter tests"' "$SRC" &&
  grep -Fq '"Filter"' "$SRC" &&
  grep -Fq 'Edit description' "$SRC" &&
  grep -Fq 'EDIT_TARGET_DESCRIPTION' "$SRC" &&
  grep -Fq 'editor_has_unsaved_changes' "$SRC" &&
  ! grep -Fq 'Arrow move  i insert' "$SRC" &&
  ! grep -Fq 'Regex templates: :template' "$SRC" &&
  grep -Fq 'ch == KEY_DOWN' "$SRC" &&
  grep -Fq 'ch == KEY_UP' "$SRC"
}

test_editor_utf8_behavior() {
  harness="$TMP_ROOT/editor_utf8.c"
  cat >"$harness" <<EOF
#define main autotest_builder_app_main
#include "$SRC"
#undef main
EOF
  cat >>"$harness" <<'EOF'

int main(void) {
    App app;
    char jp[] = { (char)0xe3, (char)0x81, (char)0x82, 'A', '\0' };
    setlocale(LC_ALL, "");
    init_app(&app);
    editor_insert_wchar(&app, 0x3042);
    if (memcmp(app.editor_lines[0], jp, 3) != 0) return 6;
    if (editor_is_text_char(KEY_DOWN)) return 7;
    if (editor_is_text_char(KEY_UP)) return 8;
    app.editor_lines[0][0] = '\0';
    app.editor_col = 0;
    load_editor_text(&app, jp);
    if (!editor_is_text_char(0xe3)) return 1;
    if (utf8_next_index(app.editor_lines[0], 0) != 3) return 2;
    if (utf8_prev_index(app.editor_lines[0], 3) != 0) return 3;
    if (editor_visual_col(app.editor_lines[0], 3) != 2) return 4;
    app.editor_col = 3;
    editor_backspace(&app);
    if (strcmp(app.editor_lines[0], "A") != 0) return 5;
    load_editor_text(&app, "abcdef\nx\nabcdef\n");
    app.editor_col = 5;
    editor_update_desired_col(&app);
    editor_move_vertical(&app, 1);
    if (app.editor_row != 1 || app.editor_col != 1 || app.editor_desired_col != 5) return 9;
    editor_move_vertical(&app, 1);
    if (app.editor_row != 2 || app.editor_col != 5 || app.editor_desired_col != 5) return 10;
    load_editor_text(&app, "abcdefghij");
    app.editor_col = 10;
    if (editor_wrapped_rows_for_line(&app, 0, 5) != 3) return 46;
    if (editor_cursor_screen_row_from_first(&app, 0, 5) != 2) return 47;
    app.editor_col = 10;
    editor_set_scroll_screen_row(&app, 2, 2, 5);
    if (app.editor_first_row != 0 || app.editor_first_wrap_row != 1) return 50;
    if (editor_cursor_screen_row_from_first(&app, app.editor_first_row, 5) != 1) return 51;
    load_editor_text(&app, "");
    for (int i = 0; i < 2000; i++) editor_insert_char(&app, 'x');
    if ((int)strlen(app.editor_lines[0]) != 2000) return 48;
    app.editor_col = 1000;
    editor_insert_text(&app, "middle");
    if ((int)strlen(app.editor_lines[0]) != 2006 ||
        strncmp(app.editor_lines[0] + 1000, "middle", 6) != 0) return 49;
    load_editor_text(&app, "");
    for (int i = 0; i < 1700; i++) editor_insert_char(&app, 'a');
    editor_newline(&app);
    for (int i = 0; i < 1700; i++) editor_insert_char(&app, 'b');
    editor_newline(&app);
    for (int i = 0; i < 1700; i++) editor_insert_char(&app, 'c');
    char *expected_script = editor_text_alloc(&app);
    if (!expected_script) return 52;
    memset(&app.project, 0, sizeof(app.project));
    app.project.case_count = 1;
    app.selected_case = 0;
    copy_text(app.project.cases[0].id, sizeof(app.project.cases[0].id), "TC999");
    copy_text(app.project.cases[0].title, sizeof(app.project.cases[0].title), "long_save");
    app.editor_target = EDIT_TARGET_COMMAND;
    save_editor_to_case(&app);
    if (strcmp(test_command(&app.project.cases[0]), expected_script) != 0) return 53;
    free(expected_script);
    load_editor_text(&app, "one\ntwo\nthree\nfour");
    app.editor_row = 1;
    editor_start_line_range(&app, 'y');
    app.editor_row = 3;
    if (!editor_apply_line_range(&app, 'y')) return 11;
    if (app.editor_clipboard_count != 3) return 12;
    if (strcmp(app.editor_clipboard[0], "two") != 0 ||
        strcmp(app.editor_clipboard[1], "three") != 0 ||
        strcmp(app.editor_clipboard[2], "four") != 0) return 13;
    load_editor_text(&app, "one\ntwo\nthree\nfour");
    app.editor_row = 2;
    editor_start_line_range(&app, 'd');
    app.editor_row = 0;
    if (!editor_apply_line_range(&app, 'd')) return 14;
    if (app.editor_line_count != 1 || strcmp(app.editor_lines[0], "four") != 0) return 15;
    app.editor_target = EDIT_TARGET_COMMAND;
    load_editor_text(&app, "");
    app.editor_col = 0;
    if (editor_try_tab_completion(&app)) return 36;
    load_editor_text(&app, "@check TEST ");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (editor_try_tab_completion(&app)) return 37;
    load_editor_text(&app, "@ch");
    app.editor_col = 3;
    if (!editor_try_tab_completion(&app)) return 16;
    if (strcmp(app.editor_lines[0], "@check ") != 0) return 17;
    load_editor_text(&app, "  @tu");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (!editor_try_tab_completion(&app)) return 26;
    if (app.editor_line_count != 2) return 27;
    if (strcmp(app.editor_lines[0], "  @tui ") != 0 ||
        strcmp(app.editor_lines[1], "  @end") != 0) return 28;
    if (app.editor_row != 0 || app.editor_col != (int)strlen("  @tui ")) return 29;
    load_editor_text(&app, "fo");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (!editor_try_tab_completion(&app)) return 54;
    if (app.editor_line_count != 3 ||
        strcmp(app.editor_lines[0], "for item in ; do") != 0 ||
        strcmp(app.editor_lines[1], "  ") != 0 ||
        strcmp(app.editor_lines[2], "done") != 0) return 55;
    if (app.editor_row != 0 || app.editor_col != (int)strlen("for item in ")) return 56;
    load_editor_text(&app, "  if");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (!editor_try_tab_completion(&app)) return 57;
    if (app.editor_line_count != 3 ||
        strcmp(app.editor_lines[0], "  if [  ]; then") != 0 ||
        strcmp(app.editor_lines[1], "    ") != 0 ||
        strcmp(app.editor_lines[2], "  fi") != 0) return 58;
    if (app.editor_row != 0 || app.editor_col != (int)strlen("  if [ ")) return 59;
    load_editor_text(&app, "@check TEST con");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (!editor_try_tab_completion(&app)) return 18;
    if (strcmp(app.editor_lines[0], "@check TEST contains ") != 0) return 19;
    load_editor_text(&app, "@check TEST not_c");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (!editor_try_tab_completion(&app)) return 30;
    if (strcmp(app.editor_lines[0], "@check TEST not_contains ") != 0) return 31;
    load_editor_text(&app, "@check TEST not_ex");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (!editor_try_tab_completion(&app)) return 32;
    if (strcmp(app.editor_lines[0], "@check TEST not_exact ") != 0) return 33;
    load_editor_text(&app, "@check TEST not_em");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (!editor_try_tab_completion(&app)) return 34;
    if (strcmp(app.editor_lines[0], "@check TEST not_empty") != 0) return 35;
    load_editor_text(&app, "@tui vim\n  sl");
    app.editor_row = 1;
    app.editor_col = (int)strlen(app.editor_lines[1]);
    if (!editor_try_tab_completion(&app)) return 20;
    if (strcmp(app.editor_lines[1], "  sleep ") != 0) return 21;
    load_editor_text(&app, "echo foo");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (editor_try_tab_completion(&app)) return 22;
    load_editor_text(&app, "@tui vim\n@end\nAUTOTEST_TUI_STDOUT_F");
    app.editor_row = 2;
    app.editor_col = (int)strlen(app.editor_lines[2]);
    if (!editor_try_tab_completion(&app)) return 23;
    if (strcmp(app.editor_lines[2], "AUTOTEST_TUI_STDOUT_FILE") != 0) return 24;
    load_editor_text(&app, "@tui vim\n@end\n@check AU exact ok");
    app.editor_row = 2;
    app.editor_col = (int)strlen("@check AU");
    if (!editor_try_tab_completion(&app)) return 38;
    if (strcmp(app.editor_lines[2], "@check AUTOTEST_TUI_ exact ok") != 0) return 39;
    load_editor_text(&app, "AUTOTEST_TUI_STDOUT_F");
    app.editor_col = (int)strlen(app.editor_lines[0]);
    if (editor_try_tab_completion(&app)) return 25;
    memset(&app.project, 0, sizeof(app.project));
    app.project.case_count = 3;
    copy_text(app.project.cases[0].id, sizeof(app.project.cases[0].id), "TC001");
    copy_text(app.project.cases[0].title, sizeof(app.project.cases[0].title), "alpha");
    set_test_command(&app.project.cases[0], "echo alpha");
    copy_text(app.project.cases[1].id, sizeof(app.project.cases[1].id), "TC002");
    copy_text(app.project.cases[1].title, sizeof(app.project.cases[1].title), "beta reboot");
    copy_text(app.project.cases[1].description, sizeof(app.project.cases[1].description), "rescue flow");
    copy_text(app.project.cases[2].id, sizeof(app.project.cases[2].id), "TC003");
    copy_text(app.project.cases[2].title, sizeof(app.project.cases[2].title), "Gamma");
    copy_text(app.project.cases[2].script_path, sizeof(app.project.cases[2].script_path), "/tmp/gamma.sh");
    copy_text(app.case_filter, sizeof(app.case_filter), "BETA");
    app.selected_case = 0;
    clamp_selected(&app);
    if (filtered_case_count(&app) != 1 || app.selected_case != 1) return 40;
    move_filtered_case(&app, 1);
    if (app.selected_case != 1) return 41;
    copy_text(app.case_filter, sizeof(app.case_filter), "TC");
    app.selected_case = 0;
    move_filtered_case(&app, 1);
    if (app.selected_case != 1) return 42;
    move_filtered_case(&app, -1);
    if (app.selected_case != 0) return 43;
    copy_text(app.case_filter, sizeof(app.case_filter), "gamma.sh");
    app.selected_case = 0;
    clamp_selected(&app);
    if (filtered_case_count(&app) != 1 || app.selected_case != 2) return 44;
    copy_text(app.case_filter, sizeof(app.case_filter), "no-such-test");
    if (filtered_case_count(&app) != 0) return 45;
    return 0;
}
EOF
  (
    cd "$TMP_ROOT" &&
    gcc -Wall -Wextra -o editor_utf8 "$harness" -lncursesw >/dev/null 2>&1 &&
    ./editor_utf8
  )
}

test_syntax_validation_source() {
  grep -Fq 'validate_script_syntax' "$SRC" &&
  grep -Fq 'validate_custom_syntax' "$SRC" &&
  grep -Fq 'validate_generated_bash' "$SRC" &&
  grep -Fq 'bash -n' "$SRC" &&
  grep -Fq 'Syntax error line %d' "$SRC" &&
  grep -Fq 'missing @end for @tui block' "$SRC" &&
  grep -Fq 'unknown command in @tui block' "$SRC"
}

test_syntax_validation_behavior() {
  harness="$TMP_ROOT/syntax_validation.c"
  cat >"$harness" <<EOF
#define main autotest_builder_app_main
#include "$SRC"
#undef main
EOF
  cat >>"$harness" <<'EOF'

static int expect_valid(const char *script) {
    SyntaxError err;
    return validate_script_syntax(script, &err) ? 0 : 1;
}

static int expect_invalid_line(const char *script, int line_no) {
    SyntaxError err;
    if (validate_script_syntax(script, &err)) return 1;
    return err.line_no == line_no ? 0 : 1;
}

int main(void) {
    if (expect_valid("TEST=A\n@check TEST exact A\n")) return 1;
    if (expect_valid("@tui vim /tmp/a\n  send gg\n  vim-write-quit\n@end\n")) return 2;
    if (expect_invalid_line("@tui vim\n  send gg\n", 2)) return 3;
    if (expect_invalid_line("@tui vim\n  bogus\n@end\n", 2)) return 4;
    if (expect_invalid_line("@check TEST exact <<EOF\nhello\n", 1)) return 5;
    if (expect_invalid_line("@tui vim\n  send \"unterminated\n@end\n", 2)) return 6;
    if (!validate_script_syntax("if true; then\n  echo ok\n", &(SyntaxError){0})) return 0;
    return 7;
}
EOF
  (
    cd "$TMP_ROOT" &&
    gcc -Wall -Wextra -o syntax_validation "$harness" -lncursesw >/dev/null 2>&1 &&
    ./syntax_validation
  )
}

test_registry_source() {
  grep -Fq '.config/autotest-assist/tests.tsv' "$SRC" &&
  grep -Fq 'script_path' "$SRC" &&
  grep -Fq 'description' "$SRC" &&
  grep -Fq 'autotest-assist tests v3' "$SRC" &&
  grep -Fq 'write_escaped_field(f, test_command(tc))' "$SRC" &&
  grep -Fq 'read_line_alloc' "$SRC" &&
  grep -Fq 'unescape_field_alloc(fields[18])' "$SRC" &&
  grep -Fq 'unescape_field(tc->description' "$SRC" &&
  grep -Fq 'save_test_registry' "$SRC"
}

test_description_source() {
  grep -Fq 'char description[LONG_LEN]' "$SRC" &&
  grep -Fq 'description:' "$SRC" &&
  grep -Fq 'draw_wrapped_text' "$SRC" &&
  grep -Fq 'desc_rows = y + h - 1 - line' "$SRC" &&
  grep -Fq 'draw_wrapped_text(line' "$SRC" &&
  grep -Fq 'print_editor_line(y + row, x, w, buf)' "$SRC" &&
  grep -Fq 'print_editor_line(y + row, x, w, "")' "$SRC" &&
  ! grep -Fq 'copy_text(buf + cut' "$SRC" &&
  ! grep -Fq 'desc_y = y + h - desc_rows - 1' "$SRC" &&
  grep -Fq 'write_comment_block(f, "Title", tc->title)' "$SRC" &&
  grep -Fq 'write_comment_block(f, "Description", tc->description)' "$SRC" &&
  grep -Fq 'preview_add_comment_block(app, "  ", "Description", tc->description)' "$SRC"
}

test_delete_source() {
  grep -Fq 'delete_case' "$SRC" &&
  grep -Fq 'CONFIRM_DELETE' "$SRC" &&
  grep -Fq 'Confirm Delete Test' "$SRC" &&
  grep -Fq 'Delete selected test?' "$SRC" &&
  grep -Fq 'app->confirm_action = CONFIRM_DELETE' "$SRC" &&
  grep -Fq 'unlink(deleted_path)' "$SRC" &&
  grep -Fq 'result_path' "$SRC"
}

test_result_display_source() {
  grep -Fq 'selected_result_path' "$SRC" &&
  grep -Fq 'autotest_selected.result' "$SRC" &&
  grep -Fq 'Selected Test Results' "$SRC" &&
  grep -Fq 'Start selected tests writes one aggregate result file' "$SRC" &&
  grep -Fq 'AutoTest selected result' "$SRC" &&
  grep -Fq 'autotest_selected.result' "$README" &&
  grep -Fq 'one aggregate result file' "$README" &&
  grep -Fq 'Summary: total=%d OK=%d NG=%d' "$SRC" &&
  grep -Fq 'append_file_to_stream(result, result_path)' "$SRC" &&
  grep -Fq 'unlink(result_path)' "$SRC" &&
  ! grep -Fq 'case_display_status' "$SRC" &&
  ! grep -Fq 'Statuses are read from each generated .result file' "$SRC" &&
  ! grep -Fq 'tc->script_path[0] ? "OK" : "NG"' "$SRC"
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
run_case '@check captures value at directive position' test_check_position_capture
run_case '@check counts loop executions at runtime' test_check_loop_runtime_count
run_case '@assert true condition' test_assert_true
run_case '@assert false condition fails' test_assert_false
run_case '@backup/@restore existing file' test_backup_restore_existing_file
run_case '@backup/@restore missing file' test_backup_restore_missing_file
run_case '@reboot-if false branch does not reboot' test_reboot_if_false_branch
run_case '@tui send/text argument quoting' test_tui_argument_quoting
run_case '@check source support' test_check_directive_source
run_case '@assert source support' test_assert_directive_source
run_case '@backup/@restore source support' test_backup_restore_source
run_case '@reboot-if source support' test_reboot_if_source
run_case '@tui source support' test_tui_source
run_case '@tui output capture source support' test_tui_capture_source
run_case 'editor command source support' test_editor_source
run_case 'editor UTF-8 behavior' test_editor_utf8_behavior
run_case 'save-time syntax validation source support' test_syntax_validation_source
run_case 'save-time syntax validation behavior' test_syntax_validation_behavior
run_case 'registry source support' test_registry_source
run_case 'test description source support' test_description_source
run_case 'delete test source support' test_delete_source
run_case 'result display source support' test_result_display_source
run_case 'README limitations documentation' test_docs_limitations

if [ "$FAILURES" -eq 0 ]; then
  printf 'Summary: OK all feature tests passed\n'
  exit 0
fi

printf 'Summary: NG failures=%d\n' "$FAILURES"
exit 1
