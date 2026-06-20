#include <QFile>
#include <QSaveFile>
#include <QTextStream>
#include <QDir>
#include <QDebug>
#include <QRegularExpression>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include "fssnapshotstore.h"

const QString FsSnapshotStore::SNAPSHOT_DIR = QStringLiteral("/var/lib/qsnapper");
const QString FsSnapshotStore::SNAPSHOT_FILE_PREFIX = QStringLiteral("pre_snapshot_");
const QString FsSnapshotStore::SNAPSHOT_FILE_SUFFIX = QStringLiteral(".id");

namespace {
    bool validateSnapshotPurpose(const QString &purpose)
    {
        static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]+$"));
        return !purpose.isEmpty() && re.match(purpose).hasMatch();
    }

    bool ensureSnapshotDirectory()
    {
        const QString snapshotDir = QStringLiteral("/var/lib/qsnapper");
        QDir dir;
        if (!dir.mkpath(snapshotDir)) {
            return false;
        }

        const QByteArray encodedPath = snapshotDir.toUtf8();
        struct stat st;
        // 注意:
        // このクライアントサイドのヘルパーは、D-Busサービスのファイルシステム強化レイヤーに対してリンクされていない
        // パスはユーザが入力するものではなく、アプリケーションが所有する固定のディレクトリであるため、ここでは単純な lstat() で十分である
        return ::lstat(encodedPath.constData(), &st) == 0 && S_ISDIR(st.st_mode);
    }
}

/**
 * @brief スナップショット番号をファイルに保存
 *
 * 指定された用途(purpose)に対応するスナップショット番号をファイルに保存する
 * ファイルパスは /var/lib/qsnapper/pre_snapshot_<purpose>.id となる
 * ディレクトリが存在しない場合は自動的に作成される
 *
 * @param purpose スナップショットの用途識別子
 * @param snapshotNumber 保存するスナップショット番号
 *
 * @return 保存に成功した場合true、失敗した場合false
 */
bool FsSnapshotStore::save(const QString &purpose, int snapshotNumber)
{
    if (!validateSnapshotPurpose(purpose)) {
        qWarning() << "Invalid snapshot purpose:" << purpose;
        return false;
    }

    QString filePath = snapshotFilePath(purpose);

    if (!ensureSnapshotDirectory()) {
        qWarning() << "Failed to create or validate directory:" << SNAPSHOT_DIR;
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Failed to open file for writing:" << filePath;
        return false;
    }

    QTextStream out(&file);
    out << snapshotNumber;
    out.flush();
    if (!file.commit()) {
        qWarning() << "Failed to atomically save snapshot number to" << filePath;
        return false;
    }

    qDebug() << "Saved snapshot number" << snapshotNumber << "to" << filePath;
    return true;
}

/**
 * @brief ファイルからスナップショット番号を読み込み
 *
 * 指定された用途(purpose)に対応するファイルからスナップショット番号を読み込む
 * ファイルが存在しない、または読み込みに失敗した場合は-1を返す
 *
 * @param purpose スナップショットの用途識別子
 *
 * @return 読み込んだスナップショット番号、
 *         ファイルが存在しないまたは読み込みに失敗した場合: -1
 */
int FsSnapshotStore::load(const QString &purpose)
{
    if (!validateSnapshotPurpose(purpose)) {
        qWarning() << "Invalid snapshot purpose:" << purpose;
        return -1;
    }

    QString filePath = snapshotFilePath(purpose);

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open snapshot file for reading:" << filePath
                   << file.errorString();
        return -1;
    }

    QTextStream in(&file);
    QString content = in.readAll().trimmed();
    file.close();

    bool ok;
    int snapshotNumber = content.toInt(&ok);

    if (!ok) {
        qWarning() << "Failed to parse snapshot number from file:" << filePath << "content:" << content;
        return -1;
    }

    qDebug() << "Loaded snapshot number" << snapshotNumber << "from" << filePath;
    return snapshotNumber;
}

/**
 * @brief 保存されたスナップショット番号ファイルを削除
 *
 * 指定された用途(purpose)に対応するスナップショット番号ファイルを削除する
 * ファイルが存在しない場合はtrueを返す (既に削除済みとみなす)
 *
 * @param purpose スナップショットの用途識別子
 *
 * @return 削除に成功した場合またはファイルが存在しない場合: true、削除に失敗した場合: false
 */
bool FsSnapshotStore::clean(const QString &purpose)
{
    if (!validateSnapshotPurpose(purpose)) {
        qWarning() << "Invalid snapshot purpose:" << purpose;
        return false;
    }

    QString filePath = snapshotFilePath(purpose);
    const QByteArray encodedPath = filePath.toUtf8();
    if (::unlink(encodedPath.constData()) == 0 || errno == ENOENT) {
        qDebug() << "Removed snapshot file:" << filePath;
        return true;
    }

    qWarning() << "Failed to remove snapshot file:" << filePath;
    return false;
}

/**
 * @brief スナップショットファイルの完全パスを生成
 *
 * 用途識別子からスナップショット番号を保存するファイルの完全パスを生成する
 * パスは /var/lib/qsnapper/pre_snapshot_<purpose>.id の形式となる
 *
 * @param purpose スナップショットの用途識別子
 *
 * @return スナップショットファイルの完全パス
 */
QString FsSnapshotStore::snapshotFilePath(const QString &purpose)
{
    return SNAPSHOT_DIR + "/" + SNAPSHOT_FILE_PREFIX + purpose + SNAPSHOT_FILE_SUFFIX;
}
