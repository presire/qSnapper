# qSnapper Security PoC Scripts

SUSE Security Review 2026-04で指摘された脆弱性の PoC 再現/回帰スクリプト群  

## ⚠ 安全に関する最重要事項

- **`common.sh` をsourceするPoC群はホスト環境では絶対に実行しないこと**。  
  特に、`poc_restore_traversal.sh` は `/etc/shadow` 上書きを試行する。  
  (修正版では拒否される想定だが、未修正バイナリに対しては成立しうる)  
- 専用 VM (openSUSE Tumbleweed, Btrfs) 上で、事前にスナップショットを取得してから実行すること。  
- 実行前に `common.sh` の `assert_running_in_vm()` チェックが通る必要がある。  
- **例外: `poc_change_record_injection.sh`** は `isolated_root_dbus_env.sh` のuser namespace内  
  scratch rootに全操作を閉じ込めるため、VMを必要とせずホストでも安全に実行できる。  
  実行体側も引数のscratch rootが `/` の場合は起動を拒否する。  

## ディレクトリ構成

```
security_poc/
├── README.md                             (この文書)
├── common.sh                             (共通ヘルパ: VM検証、ベースライン比較、snapshot ID取得)
├── poc_polkit_race.py                    (C-1: issue 1, UnixProcessSubject race)
├── poc_restore_traversal.sh              (C-5: issue 5a, 復元経路の任意ファイル上書き。StageRestoreEntriesで検証)
├── poc_quit_dos.sh                       (C-6: issue 5b, Quit()無認証DoS)
├── poc_change_record_injection.sh        (C-8: 改行注入によるパス偽装 --> 任意ファイル削除)
├── poc_change_record_injection.cpp       (同PoCの実行体。実ソースをリンクする)
├── poc_restore_plan_tamper.sh            (C-9: staged restoreのcommit時changeType改ざん)
├── poc_restore_plan_tamper.cpp           (同PoCの実行体。実ソースをリンクする)
└── results/                              (各実行の結果ログ保存先)
```

## C-8: 改行注入によるパス偽装 (poc_change_record_injection)

他のPoCと異なり、**VMもシステムD-Busも実snapper設定も不要**である。  
`tests/integration/isolated_root_dbus_env.sh` が張るuser namespace内のscratch rootに全操作を閉じ込め、  
本番の `/` には一切触れない。  

検証対象は本番の実ソースそのものである:  

| 層 | 実装 |
|---|---|
| サーバ側シリアライズの fail-closed | `qsnapper::security::isRecordSafeText()` |
| クライアント側の行分割とツリー構築 | `FileChangeModel::setupModelData()` |
| created削除前の復元元存在確認 | `qsnapper::security::isConfirmedAbsentAt()` |
| 実際の削除 | `qsnapper::security::safeRemoveAllBeneathRoot()` |

### ビルド

```bash
cmake -S . -B build -DQSNAPPER_BUILD_TESTS=ON
cmake --build build --target qsnapper_change_record_injection_poc
```

### 実行

```bash
# 修正前の再現 (ガードを外して連結する)
BASELINE=1 ./tests/security_poc/poc_change_record_injection.sh build

# 修正版の阻止確認
./tests/security_poc/poc_change_record_injection.sh build
```

いずれも、exit 0が期待値である。(README末尾のExitコード規約と同じ)  

### 出力の読み方

`STAGE 4a` はガードを外した対照実験であり、ここが `removed = YES` にならない場合は削除プリミティブ自体が機能しておらず、  
`STAGE 4b` の「拒否された」という結果は無意味 (vacuous) になる。  
そのため、本PoCは4aが成立しない場合にFAILを返す。  

- `STAGE 1` carrier 構築 — 改行入りディレクトリ + 配下の通常階層  
- `STAGE 2` サーバ側シリアライズ — baselineでは 2 パスが 3 レコードへ割れる  
- `STAGE 3` クライアントへの偽 `created` エントリの出現有無  
- `STAGE 4a` ガード無しでの削除 (対照) — victim が実際に消えること  
- `STAGE 4b` ガード有りでの削除 — 復元元に存在するため拒否され victim が残ること  
- `STAGE 4c` 正当な created (復元元に不在) は従来どおり削除されること  

## C-9: staged restoreのcommit時changeType改ざん (poc_restore_plan_tamper)

**VMもシステムD-Busも実snapper設定も不要** (`isolated_root_dbus_env.sh` の
user namespace内scratch rootに全操作を閉じる点はC-8と同じ)。  

staged restore計画のentry (`path, changeType`) をクライアントが改ざんした場合、
サーバがcommit時に (source, counterpart) から権威ある比較を再構築して
全entryを照合する層の有効性を検証する。pre/postの2つの「snapshot」木から
現実の差分を計算し、本番と同一の純粋関数群 (`qsnapper::restore::restorevalidation`) と
実security core (`RestoreManifestRegistry` / `RestorePlanExecutor` /
`qsnapper::security::*`) を用いる。

| 層 | 実装 |
|---|---|
| 状態ビット -> changeType写像 / path正規化 / 権威あるmap構築 | `qsnapper::restore::restorevalidation` |
| 凍結済み計画の保持とbounded slice読み出し | `RestoreManifestRegistry` |
| created削除前の復元元存在確認 | `qsnapper::security::isConfirmedAbsentAt()` |
| 実際の削除 | `qsnapper::security::safeRemoveAllBeneathRoot()` |

### ビルド

```bash
cmake -S . -B build -DQSNAPPER_BUILD_TESTS=ON
cmake --build build --target qsnapper_restore_plan_tamper_poc
```

### 実行

```bash
./tests/security_poc/poc_restore_plan_tamper.sh build
```

exit 0が期待値である。

### 出力の読み方

- `STAGE 2a` modified を created へ改ざんした計画がcommit時に拒否され、
  live木が1バイトも変化しないこと  
- `STAGE 2b` 差分に現れないパスをmodifiedと偽った計画 (snapshot内容の
  live上書き攻撃) も拒否されること  
- `STAGE 3` 正当なRevert to Pre計画 (pre --> post向き) は検証を通過し、
  実行後も差分外のパスが保持されること (過剰拒否がないことの証明)  
- `STAGE 4` 正当なRe-apply to Post計画 (post --> pre向き) も検証を通過すること
  (CREATED / DELETEDが向きに応じて正しく反転すること)  

## 実行手順

### 1. ベースライン取得 (修正前 v1.3.2 で実行)

```bash
sudo zypper in qSnapper-1.3.2-*.rpm       # 修正前版
cd tests/security_poc

sudo BASELINE=1 python3 poc_polkit_race.py --iterations 1000
sudo BASELINE=1 ./poc_restore_traversal.sh
sudo BASELINE=1 ./poc_quit_dos.sh
```

**全PoCが「成功 (=脆弱性再現)」となることを確認**  
成立しないPoCがあれば、環境差異か修正が既に入ったかを先に調査。  

### 2. 修正版検証

```bash
sudo zypper in qSnapper-1.3.3-*.rpm       # 修正版

sudo python3 poc_polkit_race.py --iterations 1000
sudo ./poc_restore_traversal.sh
sudo ./poc_quit_dos.sh
```

**全PoCが「失敗 (=修正により閉じられた)」となることを確認**  

### 3. bob からの実行時の注意

`poc_polkit_race.py` を `sudo -u bob` で実行する場合、bob は `/home/<owner>/...` 配下を読めない (700ホーム) ため、  
スクリプトを `/tmp/qsnapper_poc/` 等の共有可能な場所にコピーしてから実行する:  

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
| C-8 | —    | シリアライズのfail-closed (isRecordSafeText) + created削除前の復元元存在確認 (isConfirmedAbsentAt) |
| C-9 | —    | commit時の権威ある比較再構築と全entry照合 (restorevalidation) + counterpartSnapshotNumber導入 |

> **削除済PoC** (テスト計画書 [テスト計画]SUSE_Security_Fix_テスト項目.md と同期):  
> - **C-2** (poc_config_traversal.sh): B-2 + `tests/integration/test_configname_dbus.py` で完全カバー  
> - **C-3** (poc_cross_user.sh): B-1-4 (suse@KDE + bob@SSH) と重複、自動化不能 (Polkit subject 制約)  
> - **C-4** (poc_action_mixup.sh): 攻撃起点 `Authenticate()` が削除済のため実行不能  
> - **C-7** (poc_log_leak.sh): B-5 (ログファイル権限テスト) と完全重複  
> - **run_all.sh**: 削除済 PoC を含むため一括実行廃止、PoC 単位で個別実行する  

## 各スクリプトのExitコード規約

- `0` = 「期待通りの結果」(ベースラインなら再現成功、修正版なら阻止成功)  
- `1` = 「想定外」(ベースラインで再現失敗、修正版で再現成功 — いずれもregression)  
- `2` = 環境エラー (VMでない、サービス未起動など)  
