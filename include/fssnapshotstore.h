#ifndef FSSNAPSHOTSTORE_H
#define FSSNAPSHOTSTORE_H

#include <QString>

/**
 * @brief スナップショット番号の永続化を担うストアクラス
 *
 * 用途別にPreスナップショット番号を、
 * ユーザ単位のデータディレクトリ (QStandardPaths::AppLocalDataLocation) 配下のファイルへ保存・読み込み・削除する静的ユーティリティを提供する
 * GUIは非特権で動作するため、/var/lib配下のようなシステム全体の場所には保存しない
 *
 * 各用途 (purpose) は英数字・ハイフン・アンダースコアのみで構成される識別子で管理される
 */
class FsSnapshotStore
{
public:
    // 公開API (保存・読み込み・削除)
    static bool save(const QString &purpose, int snapshotNumber);   // スナップショット番号をファイルに保存する
    static int load(const QString &purpose);                        // ファイルからスナップショット番号を読み込む
    static bool clean(const QString &purpose);                      // 保存されたスナップショット番号ファイルを削除する

private:
    // 内部ヘルパー
    static QString snapshotDirectory();                             // スナップショットファイル保存ディレクトリを取得する
    static QString snapshotFilePath(const QString &purpose);        // スナップショットファイルの完全パスを生成する

    // 定数 (ファイル配置)
    static const QString SNAPSHOT_FILE_PREFIX;                      // スナップショットファイル名プレフィックス (pre_snapshot_)
    static const QString SNAPSHOT_FILE_SUFFIX;                      // スナップショットファイル名サフィックス (.id)
};

#endif // FSSNAPSHOTSTORE_H
