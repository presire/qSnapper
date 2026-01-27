#ifndef FSSNAPSHOTSTORE_H
#define FSSNAPSHOTSTORE_H

#include <QString>

class FsSnapshotStore
{
public:
    static bool save(const QString &purpose, int snapshotNumber);
    static int load(const QString &purpose);
    static bool clean(const QString &purpose);

private:
    static QString snapshotFilePath(const QString &purpose);
    static const QString SNAPSHOT_DIR;           // スナップショットファイル保存ディレクトリ (/var/lib/qsnapper)
    static const QString SNAPSHOT_FILE_PREFIX;   // スナップショットファイル名プレフィックス (pre_snapshot_)
    static const QString SNAPSHOT_FILE_SUFFIX;   // スナップショットファイル名サフィックス (.id)
};

#endif // FSSNAPSHOTSTORE_H
