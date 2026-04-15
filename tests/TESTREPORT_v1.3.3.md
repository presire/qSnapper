# qSnapper v1.3.3 Security Fix — Test Report

**Version under test**: v1.3.3 (fixes for SUSE security review 2026-04)
**Baseline comparison**: v1.3.2 tag
**Report date**: YYYY-MM-DD
**Reviewer**: Presire (Tomoki Fujikawa)
**Status**: _DRAFT / FINAL_

---

## 1. Executive summary

| Layer | Cases | Pass | Fail | Skip |
|---|---|---|---|---|
| A. Unit (Qt Test)                 |  __/__ |  __  |  __  |  __  |
| B. D-Bus integration              |  __/__ |  __  |  __  |  __  |
| C. PoC regression                 |  __/7  |  __  |  __  |  __  |
| D. Functional regression (manual) |  __/13 |  __  |  __  |  __  |
| **Total**                         |  __/__ |  __  |  __  |  __  |

**Verdict**: _ALL PASS / BLOCKERS PRESENT_ — brief one-line summary.

Known gaps / deviations from plan: _(e.g. "symlink test A-2-8 deferred to integration")_

---

## 2. Environment

| Item | Value |
|---|---|
| Test VM OS         | openSUSE Tumbleweed ____ (kernel ____) |
| Filesystem         | Btrfs on ____ GB |
| Qt version         | 6._._ |
| Polkit version     | ____ |
| snapper version    | ____ |
| qSnapper build     | v1.3.3-__g_______ (commit ________) |
| Baseline build     | v1.3.2 RPM ________ |
| Compiler / sanitize | gcc/clang ____, `-fsanitize=address,undefined` |
| Test accounts      | alice (admin), bob (non-admin), nobody_test |

Build commands used:
```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DQSNAPPER_BUILD_TESTS=ON \
      -DQSNAPPER_TEST_SANITIZE=ON ..
cmake --build . -j
```

---

## 3. Layer A — Unit tests (Qt Test)

Command:
```bash
ctest --output-on-failure -R "tst_configname|tst_filepaths"
```

### A-1. validateConfigName (13 cases + length boundary)
| Case | Input | Expected | Result |
|---|---|---|---|
| A-1-1  | `"root"` | accept | _PASS/FAIL_ |
| A-1-2  | `"home"` | accept | _PASS/FAIL_ |
| A-1-3  | `"my-config_01.test"` | accept | _PASS/FAIL_ |
| A-1-4  | `""` | reject | _PASS/FAIL_ |
| A-1-5  | `".."` | reject | _PASS/FAIL_ |
| A-1-6  | `"../etc"` | reject | _PASS/FAIL_ |
| A-1-7  | `"root/.."` | reject | _PASS/FAIL_ |
| A-1-8  | `"/etc/snapper"` | reject | _PASS/FAIL_ |
| A-1-9  | `"config with space"` | reject | _PASS/FAIL_ |
| A-1-10 | `"config\0inject"` | reject | _PASS/FAIL_ |
| A-1-11 | `"日本語"` | reject | _PASS/FAIL_ |
| A-1-12 | 255/256 chars boundary | accept/reject | _PASS/FAIL_ |
| A-1-13 | `"-leading-dash"` | reject | _PASS/FAIL_ |

### A-2. isPathWithinSnapshotRoot (10 cases + long path)
| Case | Summary | Expected | Result |
|---|---|---|---|
| A-2-1 | normal file under root | accept | _PASS/FAIL_ |
| A-2-2 | relative path | reject | _PASS/FAIL_ |
| A-2-3 | `../` traversal | reject | _PASS/FAIL_ |
| A-2-4 | prefix mismatch `/etc/hosts` | reject | _PASS/FAIL_ |
| A-2-5 | root itself | accept | _PASS/FAIL_ |
| A-2-6 | NUL / newline | reject | _PASS/FAIL_ |
| A-2-7 | symlink escapes root | reject (integration) | _PASS/FAIL_ |
| A-2-8 | symlink within root | _(policy)_ | _PASS/FAIL_ |
| A-2-9 | path > PATH_MAX | reject | _PASS/FAIL_ |
| A-2-10 | canonical escape | reject | _PASS/FAIL_ |

ASan/UBSan findings: _none / see attached asan.log_

---

## 4. Layer B — D-Bus integration

### B-1. Polkit authorization (8 cases)
| Case | Scenario | Expected | Result |
|---|---|---|---|
| B-1-1 | alice CreateSnapshot (authed) | success | _PASS/FAIL_ |
| B-1-2 | bob CreateSnapshot | AccessDenied | _PASS/FAIL_ |
| B-1-3 | alice `auth_admin_keep` reuse | success w/o re-auth | _PASS/FAIL_ |
| B-1-4 | cross-user (alice hot → bob call) | bob denied | _PASS/FAIL_ |
| B-1-5 | Authenticate() call | UnknownMethod | _PASS/FAIL_ |
| B-1-6 | Quit() call | UnknownMethod | _PASS/FAIL_ |
| B-1-7 | `list-snapshots` only → GetFileDiffAndDetails | denied | _PASS/FAIL_ |
| B-1-8 | `view-diff` authed → GetFileDiffAndDetails | success | _PASS/FAIL_ |

### B-2. configName validation (14 methods × 6 payloads = 84 cases)
Script: `tests/integration/test_configname_dbus.py`

| Method | InvalidArgs / 6 | Result |
|---|---|---|
| GetFstype              | _/6 | _PASS/FAIL_ |
| ListSnapshots          | _/6 | _PASS/FAIL_ |
| CreateSnapshot         | _/6 | _PASS/FAIL_ |
| ModifySnapshot         | _/6 | _PASS/FAIL_ |
| DeleteSnapshot         | _/6 | _PASS/FAIL_ |
| RollbackSnapshot       | _/6 | _PASS/FAIL_ |
| GetFileChanges         | _/6 | _PASS/FAIL_ |
| GetFileChangesBetween  | _/6 | _PASS/FAIL_ |
| GetFileDiffAndDetails  | _/6 | _PASS/FAIL_ |
| GetFileDiffBetween     | _/6 | _PASS/FAIL_ |
| RestoreFiles           | _/6 | _PASS/FAIL_ |
| RestoreFilesDirect     | _/6 | _PASS/FAIL_ |
| WriteSnapperConfig     | _/6 | _PASS/FAIL_ |
| SetupQuota             | _/6 | _PASS/FAIL_ |
| **Total**              | _/84 | — |

### B-3. RestoreFiles path sanitization (7 cases, both methods)
| Case | Expected | RestoreFiles | RestoreFilesDirect |
|---|---|---|---|
| B-3-1 absolute within root   | success      | _/_/_  | _/_/_  |
| B-3-2 prefix mismatch        | InvalidArgs  | _/_/_  | _/_/_  |
| B-3-3 `../../../etc/shadow`  | InvalidArgs  | _/_/_  | _/_/_  |
| B-3-4 relative               | InvalidArgs  | _/_/_  | _/_/_  |
| B-3-5 symlink to shadow      | InvalidArgs  | _/_/_  | _/_/_  |
| B-3-6 TOCTOU swap            | no race win  | _/_/_  | _/_/_  |
| B-3-7 parity (both methods)  | identical    | _/_/_  | _/_/_  |

### B-4. D-Bus policy introspection
- `busctl introspect` shows no `Authenticate` / `Quit`: _PASS/FAIL_
- Random interface/member name → AccessDenied: _PASS/FAIL_

### B-5. Log file permissions
- `/var/log/qsnapper` mode: ____ (expected 700): _PASS/FAIL_
- Log file mode: ____ (expected 600): _PASS/FAIL_
- `sudo -u bob cat` denied: _PASS/FAIL_

---

## 5. Layer C — PoC regression

Executed via: `sudo tests/security_poc/run_all.sh`
Baseline (`BASELINE=1`): all 7 PoCs **should reproduce**. Fixed: all 7 **should be blocked**.

| PoC | Issue | Baseline (reproduced?) | Fixed (blocked?) | Verdict |
|---|---|---|---|---|
| C-1 polkit_race       | #1   | _Y/N_ | _Y/N_ | _PASS/FAIL_ |
| C-2 config_traversal  | #2   | _Y/N_ | _Y/N_ | _PASS/FAIL_ |
| C-3 cross_user        | #4a  | _Y/N_ | _Y/N_ | _PASS/FAIL_ |
| C-4 action_mixup      | #4b  | _Y/N_ | _Y/N_ | _PASS/FAIL_ |
| C-5 restore_traversal | #5a  | _Y/N_ | _Y/N_ | _PASS/FAIL_ |
| C-6 quit_dos          | #5b  | _Y/N_ | _Y/N_ | _PASS/FAIL_ |
| C-7 log_leak          | #5c  | _Y/N_ | _Y/N_ | _PASS/FAIL_ |

Critical side-effects checked on fixed build:
- `stat /etc/shadow` before vs after C-5: _unchanged / MODIFIED_
- Service PID before vs after C-6: _same / killed_

Logs attached: `tests/security_poc/results/{baseline,fixed}_*.log`

---

## 6. Layer D — Functional regression (manual QML)

| ID | Scenario | Expected | Result |
|---|---|---|---|
| D-1-1  | Launch + list snapshots                | no prompt, all configs | _PASS/FAIL_ |
| D-1-2  | Create snapshot (single)               | 1 prompt, added | _PASS/FAIL_ |
| D-1-3  | Create pre/post pair                   | success, paired display | _PASS/FAIL_ |
| D-1-4  | Delete snapshot                        | prompt, removed | _PASS/FAIL_ |
| D-1-5  | Modify userdata/description            | prompt, reflected | _PASS/FAIL_ |
| D-1-6  | Two-snapshot diff                      | `view-diff` prompt, rendered | _PASS/FAIL_ |
| D-1-7  | Single-file diff (GetFileDiffAndDetails)| prompt, rendered | _PASS/FAIL_ |
| D-1-8  | Restore via RestoreFiles path          | prompt, only target restored | _PASS/FAIL_ |
| D-1-9  | Restore via RestoreFilesDirect path    | identical behavior to D-1-8 | _PASS/FAIL_ |
| D-1-10 | Rollback                               | prompt, active after reboot | _PASS/FAIL_ |
| D-1-11 | Configure snapper setting              | prompt, saved | _PASS/FAIL_ |
| D-1-12 | Idle 5 min → service stops             | auto-stop confirmed | _PASS/FAIL_ |
| D-1-13 | Auto-activation after idle stop        | D-Bus activation works | _PASS/FAIL_ |

### D-2. Qt Quick Test
```bash
ctest --output-on-failure -R qmltest
```
Result: _PASS / FAIL — see qmltest.log_

### D-3. Translation sanity (ja / de / en)
- `view-diff` action description rendered correctly in all 3 locales: _PASS/FAIL_

---

## 7. Known issues / residual risk

_Document anything intentionally deferred or not fully closed, with justification._

- _(example)_ A-2-8 symlink-within-root policy: currently accepts, pending discussion with SUSE reviewer.
- _(example)_ C-1 race window measurement: on test VM unable to reliably reproduce baseline; still verified by code audit that `UnixProcessSubject` → `SystemBusNameSubject` change is present.

---

## 8. Sign-off

- [ ] All Layer A unit tests pass with ASan/UBSan clean
- [ ] All Layer B integration tests pass
- [ ] All 7 PoCs are neutralised on fixed build
- [ ] All 13 manual regression scenarios pass
- [ ] No regressions observed in ja/de/en UI
- [ ] Logs and reproducers attached under `tests/security_poc/results/`

Prepared by: ____________________   Date: YYYY-MM-DD
Reviewed by (SUSE liaison, if applicable): ____________________   Date: YYYY-MM-DD

---

## Appendix A — Attached artefacts

- `tests/security_poc/results/baseline_SUMMARY.txt`
- `tests/security_poc/results/fixed_SUMMARY.txt`
- `tests/security_poc/results/{baseline,fixed}_*.log` (per PoC)
- `tests/integration/results/configname_dbus_{baseline,fixed}.log`
- `asan.log`, `ubsan.log` (if any findings)

## Appendix B — Cross-reference to SUSE report

| SUSE issue # | CVE candidate | Fix item | Covering tests |
|---|---|---|---|
| 1 | CVE candidate 1 | P0-1 | A-(none pure), B-1-1〜4, C-1 |
| 2 | CVE candidate 2 | P0-3 | A-1 (all), B-2 (84), C-2 |
| 3 | —              | P1-5 | B-1-7/8, D-1-6/7 |
| 4a | CVE candidate 4 | P0-2 | B-1-4, C-3 |
| 4b | CVE candidate 5 | P0-2 | B-1-5, C-4 |
| 5a | —              | P0-4 | A-2, B-3, C-5 |
| 5b | —              | P1-6 | B-1-6, C-6 |
| 5c | —              | P1-7 | B-5, C-7 |
| —  | —              | P2-8 (D-Bus per-member ACL) | B-4-1〜4 |
