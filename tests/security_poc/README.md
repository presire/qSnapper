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
├── README.md            (この文書)
├── common.sh            (共通ヘルパ: VM検証、ベースライン比較、snapshot ID取得)
├── run_all.sh           (全PoCを順次実行し results/ に記録)
├── poc_polkit_race.py   (C-1: issue 1, UnixProcessSubject race)
├── poc_config_traversal.sh (C-2: issue 2, configNameパストラバーサル)
├── poc_cross_user.sh    (C-3: issue 4a, m_authenticated クロスユーザー汚染)
├── poc_action_mixup.sh  (C-4: issue 4b, Authenticate() 任意action混用)
├── poc_restore_traversal.sh (C-5: issue 5a, RestoreFiles任意ファイル上書き)
├── poc_quit_dos.sh      (C-6: issue 5b, Quit()無認証DoS)
├── poc_log_leak.sh      (C-7: issue 5c, /var/log/qsnapper情報漏洩)
└── results/             (各実行の結果ログ保存先)
```

## 実行手順

### 1. ベースライン取得 (修正前 v1.3.2 で実行)

```bash
sudo zypper in qSnapper-1.3.2-*.rpm       # 修正前版
cd tests/security_poc
sudo BASELINE=1 ./run_all.sh              # results/baseline_*.log に記録
```
**全PoCが「成功 (=脆弱性再現)」となることを確認**。成立しないPoCがあれば、環境差異か修正が既に入ったかを先に調査。

### 2. 修正版検証

```bash
sudo zypper in qSnapper-1.3.3-*.rpm       # 修正版
sudo ./run_all.sh                         # results/fixed_*.log に記録
```
**全PoCが「失敗 (=修正により閉じられた)」となることを確認**。

### 3. 比較レポート生成

```bash
./compare_results.sh > results/SUMMARY.md
```

## 対応表

| PoC | Issue | CVE candidate | 修正項目 |
|---|---|---|---|
| C-1 | #1 | 1 | P0-1 (SystemBusNameSubject置換) |
| C-2 | #2 | 2 | P0-3 (validateConfigName) |
| C-3 | #4a | 4 | P0-2 (m_authenticated撤去) |
| C-4 | #4b | 5 | P0-2 (Authenticate削除) |
| C-5 | #5a | – | P0-4 (RestoreFiles統合 + openat) |
| C-6 | #5b | – | P1-6 (Quit削除) |
| C-7 | #5c | – | P1-7 (ログ権限) |

## 各スクリプトのExitコード規約

- `0` = 「期待通りの結果」(ベースラインなら再現成功、修正版なら阻止成功)
- `1` = 「想定外」(ベースラインで再現失敗、修正版で再現成功 — いずれもregression)
- `2` = 環境エラー (VMでない、サービス未起動など)
