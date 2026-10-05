#ifndef FSSNAPSHOTSTORE_H
#define FSSNAPSHOTSTORE_H

#include <QString>

/**
 * @file fssnapshotstore.h
 * @brief 用途別のPreスナップショット番号をユーザ単位で永続化するストア
 */

/**
 * @class FsSnapshotStore
 * @brief スナップショット番号の永続化を担うストアクラス
 *
 * 用途別にPreスナップショット番号を、ユーザ単位のデータディレクトリ (QStandardPaths::AppLocalDataLocation) 配下のファイルへ保存・読み込み・削除する静的ユーティリティを提供する
 * GUIは非特権で動作するため、/var/lib配下のようなシステム全体の場所には保存しない
 * 各用途 (purpose) は英数字・ハイフン・アンダースコアのみで構成される識別子で管理される
 */
class FsSnapshotStore
{
public:
    /**
     * @brief スナップショット番号をファイルに保存する
     * @param purpose 用途を表す識別子
     * @param snapshotNumber 保存するスナップショット番号
     * @return 保存に成功した場合はtrue
     */
    static bool save(const QString &purpose, int snapshotNumber);

    /**
     * @brief ファイルからスナップショット番号を読み込む
     * @param purpose 用途を表す識別子
     * @return 保存済みのスナップショット番号、未保存または不正な値の場合は-1
     */
    static int load(const QString &purpose);

    /**
     * @brief 保存されたスナップショット番号ファイルを削除する
     * @param purpose 用途を表す識別子
     * @return 削除に成功した場合はtrue
     */
    static bool clean(const QString &purpose);

private:
    // 内部ヘルパー
    /**
     * @brief スナップショットファイル保存ディレクトリを取得する
     * @return ユーザ単位のデータディレクトリ配下の保存先
     */
    static QString snapshotDirectory();

    /**
     * @brief スナップショットファイルの完全パスを生成する
     * @param purpose 用途を表す識別子
     * @return 用途に対応するファイルの完全パス
     */
    static QString snapshotFilePath(const QString &purpose);

    // 定数 (ファイル配置)
    static const QString SNAPSHOT_FILE_PREFIX;  // スナップショットファイル名プレフィックス (pre_snapshot_)
    static const QString SNAPSHOT_FILE_SUFFIX;  // スナップショットファイル名サフィックス (.id)
};

#endif // FSSNAPSHOTSTORE_H
