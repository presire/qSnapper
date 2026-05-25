# qSnapper Security PoC Scripts

SUSE Security Review 2026-04 で指摘された脆弱性の PoC 再現/回帰スクリプト群。

## ⚠ 安全に関する最重要事項

- **本スクリプト群はホスト環境では絶対に実行しないこと**。特に `poc_restore_traversal.sh` は
  `/etc/shadow` 上書きを試行する (修正版では拒否される想定だが、未修正バイナリに対しては成立しうる)。
- 専用 VM (openSUSE Tumbleweed, Btrfs) 上で、事前にスナップショットを取得してから実行すること。
- 実行前に `common.sh` の `assert_running_in_vm()` チェックが通る必要がある。

## ディレクトリ構成

```
security_poc/
├── README.md                   (この文書)
├── common.sh                   (共通ヘルパ: VM検証、ベースライン比較、snapshot ID取得)
├── poc_polkit_race.py          (C-1: issue 1, UnixProcessSubject race)
├── poc_restore_traversal.sh    (C-5: issue 5a, RestoreFiles任意ファイル上書き)
├── poc_quit_dos.sh             (C-6: issue 5b, Quit()無認証DoS)
└── results/                    (各実行の結果ログ保存先)
```

## 実行手順

### 1. ベースライン取得 (修正前 v1.3.2 で実行)

```bash
sudo zypper in qSnapper-1.3.2-*.rpm       # 修正前版
cd tests/security_poc

sudo BASELINE=1 python3 poc_polkit_race.py --iterations 1000
sudo BASELINE=1 ./poc_restore_traversal.sh
sudo BASELINE=1 ./poc_quit_dos.sh
```

**全 PoC が「成功 (=脆弱性再現)」となることを確認**。成立しないPoCがあれば、環境差異か修正が既に入ったかを先に調査。

### 2. 修正版検証

```bash
sudo zypper in qSnapper-1.3.3-*.rpm       # 修正版

sudo python3 poc_polkit_race.py --iterations 1000
sudo ./poc_restore_traversal.sh
sudo ./poc_quit_dos.sh
```

**全 PoC が「失敗 (=修正により閉じられた)」となることを確認**。

### 3. bob からの実行時の注意

`poc_polkit_race.py` を `sudo -u bob` で実行する場合、bob は `/home/<owner>/...` 配下を
読めない (700 ホーム) ため、スクリプトを `/tmp/qsnapper_poc/` 等の共有可能な場所に
コピーしてから実行する:

```bash
sudo cp -r tests/security_poc /tmp/qsnapper_poc
sudo chmod -R o+rX /tmp/qsnapper_poc
sudo -u bob python3 /tmp/qsnapper_poc/poc_polkit_race.py --iterations 1000
```

## 対応表

| PoC | Issue | 修正項目 |
|---|---|---|
| C-1 | #1   | P0-1 (SystemBusNameSubject置換) |
| C-5 | #5a  | P0-4 (RestoreFiles統合 + openat) |
| C-6 | #5b  | P1-6 (Quit削除) + P2 (.conf per-member ACL) |

> **削除済 PoC** (テスト計画書 [テスト計画]SUSE_Security_Fix_テスト項目.md と同期):
> - **C-2** (poc_config_traversal.sh): B-2 + `tests/integration/test_configname_dbus.py` で完全カバー
> - **C-3** (poc_cross_user.sh): B-1-4 (suse@KDE + bob@SSH) と重複、自動化不能 (Polkit subject 制約)
> - **C-4** (poc_action_mixup.sh): 攻撃起点 `Authenticate()` が削除済のため実行不能
> - **C-7** (poc_log_leak.sh): B-5 (ログファイル権限テスト) と完全重複
> - **run_all.sh**: 削除済 PoC を含むため一括実行廃止、PoC 単位で個別実行する

## 各スクリプトのExitコード規約

- `0` = 「期待通りの結果」(ベースラインなら再現成功、修正版なら阻止成功)
- `1` = 「想定外」(ベースラインで再現失敗、修正版で再現成功 — いずれもregression)
- `2` = 環境エラー (VMでない、サービス未起動など)
