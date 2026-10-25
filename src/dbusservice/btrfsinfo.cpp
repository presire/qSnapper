/**
 * @file btrfsinfo.cpp
 * @brief btrfsinfo.h宣言の実装
 *
 * 全ての読み取りはopenBtrfsDirectory()が返したfdに対して行い、パス文字列を再解決しない
 * これにより検査したディレクトリと読み取ったディレクトリの間で差し替えが起きない
 */

#include "btrfsinfo.h"

#include <QElapsedTimer>
#include <QStringList>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include <fcntl.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <btrfsutil.h>

#include "csvrecord.h"
#include "inputvalidator.h"

namespace qsnapper::btrfs {

namespace {

// BTRFS_FS_TREE_OBJECTIDと同じ値であり、btrfs/ctree.hへ依存せずFSツリー全体を列挙するために使う
constexpr quint64 kFsTreeObjectId = 5;

// BTRFS_ROOT_SUBVOL_RDONLYと同じ値であり、root itemのflagsに含まれる読み取り専用ビット
constexpr quint64 kRootSubvolReadOnly = 1ULL << 0;

/**
 * @brief 開いたfdがBtrfsのディレクトリかどうかを確認する
 * @param fd openat2()で開いたfd
 * @return Btrfsのディレクトリの場合true
 */
bool isBtrfsDirectory(int fd)
{
    struct stat status = {};
    if (::fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode)) {
        return false;
    }

    struct statfs filesystemStatus = {};
    if (::fstatfs(fd, &filesystemStatus) != 0) {
        return false;
    }

    return static_cast<unsigned long>(filesystemStatus.f_type)
           == static_cast<unsigned long>(BTRFS_SUPER_MAGIC);
}

/**
 * @brief サブボリュームの表示用パスをCSVフィールドへ変換する
 *
 * パスは表示専用であり識別子としては使わない
 * そのためレコード区切りを壊す制御文字は置換して一覧全体を失敗させず、CSVのquoteで列ずれも防ぐ
 * 識別子として使う値では置換が別の対象を指し得るため、この扱いを流用してはならない
 *
 * @param rawPath サブボリュームの生パス
 * @return quote済みの表示用フィールド
 */
QString formatDisplayPathField(const QByteArray &rawPath)
{
    const QString displayPath = qsnapper::security::sanitizeRecordText(QString::fromUtf8(rawPath));
    return qsnapper::csv::quoteField(displayPath);
}

} // namespace

int openBtrfsDirectory(const QString &absolutePath)
{
    if (!absolutePath.startsWith(QLatin1Char('/')) || absolutePath.contains(QChar(0))) {
        return -1;
    }

    // パス中の0バイトはopenat2()に渡すと以降が切り捨てられるため、文字列の時点で拒否する
    const QByteArray encodedPath = absolutePath.toUtf8();
    struct open_how how = {};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

    const long openResult = ::syscall(SYS_openat2, AT_FDCWD, encodedPath.constData(),
                                      &how, sizeof(how));
    if (openResult < 0) {
        // ENOSYSはopenat2()非対応のカーネルを意味する
        // 成分単位のopenat()へ切り替えるとsymlinkを解決しない保証を同じ強度で維持できないため、代替せず失敗させる
        return -1;
    }

    const int fd = static_cast<int>(openResult);
    if (!isBtrfsDirectory(fd)) {
        ::close(fd);
        return -1;
    }

    return fd;
}

InfoError readFilesystemUsage(int dirFd, FilesystemUsage *out)
{
    if (dirFd < 0 || out == nullptr) {
        return InfoError::Failed;
    }

    btrfs_ioctl_fs_info_args fsInfo = {};
    if (::ioctl(dirFd, BTRFS_IOC_FS_INFO, &fsInfo) < 0) {
        // ENOTTYとEINVALはBtrfs以外のファイルシステムでこのioctlを発行した場合に返る
        if (errno == ENOTTY || errno == EINVAL) {
            return InfoError::NotBtrfs;
        }
        return InfoError::Failed;
    }

    if (fsInfo.max_id > MaxDeviceId) {
        return InfoError::LimitExceeded;
    }

    FilesystemUsage usage;
    usage.numDevices = fsInfo.num_devices;
    usage.nodesize = fsInfo.nodesize;
    usage.sectorsize = fsInfo.sectorsize;

    try {
        for (quint64 devid = 1; devid <= fsInfo.max_id; ++devid) {
            btrfs_ioctl_dev_info_args devInfo = {};
            devInfo.devid = devid;
            if (::ioctl(dirFd, BTRFS_IOC_DEV_INFO, &devInfo) < 0) {
                // ENODEVは間欠的なdevidであり、デバイスが存在しないことを示す
                if (errno == ENODEV) {
                    continue;
                }
                return InfoError::Failed;
            }

            DeviceEntry device = {};
            device.devid = static_cast<quint64>(devInfo.devid);
            device.totalBytes = static_cast<quint64>(devInfo.total_bytes);
            device.bytesUsed = static_cast<quint64>(devInfo.bytes_used);
            usage.devices.append(device);
        }

        // 取得できたデバイス数がカーネルの申告と違う場合、合計値が欠けたまま成功にしない
        if (static_cast<quint64>(usage.devices.size()) != fsInfo.num_devices) {
            return InfoError::Failed;
        }

        btrfs_ioctl_space_args slotQuery = {};
        if (::ioctl(dirFd, BTRFS_IOC_SPACE_INFO, &slotQuery) < 0) {
            return InfoError::Failed;
        }
        if (slotQuery.total_spaces > static_cast<quint64>(MaxSpaceSlots)) {
            return InfoError::LimitExceeded;
        }

        const quint64 allocatedSlots = slotQuery.total_spaces;
        const std::size_t bufferSize = sizeof(btrfs_ioctl_space_args)
                + static_cast<std::size_t>(allocatedSlots) * sizeof(btrfs_ioctl_space_info);
        std::vector<unsigned char> spaceBuffer(bufferSize, 0);
        auto *spaceArgs = reinterpret_cast<btrfs_ioctl_space_args *>(spaceBuffer.data());
        spaceArgs->space_slots = allocatedSlots;

        if (::ioctl(dirFd, BTRFS_IOC_SPACE_INFO, spaceArgs) < 0) {
            return InfoError::Failed;
        }
        // 2回目の呼び出しまでに領域数が増えた場合、確保した領域を超えて読まないよう失敗させる
        if (spaceArgs->total_spaces > spaceArgs->space_slots) {
            return InfoError::Failed;
        }

        const quint64 returnedSlots = spaceArgs->total_spaces;
        usage.spaces.reserve(static_cast<int>(returnedSlots));
        for (quint64 index = 0; index < returnedSlots; ++index) {
            const btrfs_ioctl_space_info &slot = spaceArgs->spaces[index];
            SpaceEntry space = {};
            space.flags = static_cast<quint64>(slot.flags);
            space.totalBytes = static_cast<quint64>(slot.total_bytes);
            space.usedBytes = static_cast<quint64>(slot.used_bytes);
            usage.spaces.append(space);
        }
    }
    catch (const std::bad_alloc &) {
        // 確保できない件数を部分的な結果として返さない
        return InfoError::Failed;
    }

    *out = usage;
    return InfoError::None;
}

InfoError listSubvolumes(int dirFd, const SubvolumeListLimits &limits,
                         QVector<SubvolumeEntry> *out, bool *truncated)
{
    if (dirFd < 0 || out == nullptr || truncated == nullptr) {
        return InfoError::Failed;
    }
    // 負の上限は列挙の打ち切り条件として成立しないため、空の一覧を成功として返さない
    if (limits.maxEntries < 0 || limits.maxPathBytes < 0 || limits.maxMilliseconds < 0) {
        return InfoError::Failed;
    }

    *truncated = false;
    QVector<SubvolumeEntry> collected;
    qint64 collectedPathBytes = 0;
    QElapsedTimer elapsed;
    elapsed.start();

    btrfs_util_subvolume_iterator *iterator = nullptr;
    if (btrfs_util_create_subvolume_iterator_fd(dirFd, kFsTreeObjectId, 0, &iterator)
            != BTRFS_UTIL_OK) {
        return InfoError::Failed;
    }

    bool failed = false;
    try {
        while (true) {
            char *rawPath = nullptr;
            struct btrfs_util_subvolume_info info = {};
            const btrfs_util_error error =
                    btrfs_util_subvolume_iterator_next_info(iterator, &rawPath, &info);
            const std::unique_ptr<char, decltype(&std::free)> pathHolder(rawPath, &std::free);

            // libbtrfsutilは終端をSTOP_ITERATIONで通知する
            if (error == BTRFS_UTIL_ERROR_STOP_ITERATION) {
                break;
            }
            if (error != BTRFS_UTIL_OK || !pathHolder) {
                failed = true;
                break;
            }

            const QByteArray path(pathHolder.get());
            if (collected.size() >= limits.maxEntries
                    || collectedPathBytes + static_cast<qint64>(path.size())
                               > limits.maxPathBytes) {
                *truncated = true;
                break;
            }

            SubvolumeEntry entry;
            entry.id = static_cast<quint64>(info.id);
            entry.parentId = static_cast<quint64>(info.parent_id);
            entry.otimeSec = static_cast<qint64>(info.otime.tv_sec);
            entry.readOnly = (static_cast<quint64>(info.flags) & kRootSubvolReadOnly) != 0;
            entry.path = path;
            collected.append(entry);
            collectedPathBytes += static_cast<qint64>(path.size());

            if (elapsed.elapsed() > limits.maxMilliseconds) {
                *truncated = true;
                break;
            }
        }
    }
    catch (const std::bad_alloc &) {
        failed = true;
    }

    btrfs_util_destroy_subvolume_iterator(iterator);

    if (failed) {
        out->clear();
        *truncated = false;
        return InfoError::Failed;
    }

    *out = collected;
    return InfoError::None;
}

bool isSnapperSnapshotPath(const QByteArray &path)
{
    const QList<QByteArray> components = path.split('/');
    if (components.size() < 3) {
        return false;
    }

    const int lastIndex = components.size() - 1;
    if (components.at(lastIndex) != QByteArrayLiteral("snapshot")) {
        return false;
    }

    const QByteArray number = components.at(lastIndex - 1);
    if (number.isEmpty()) {
        return false;
    }
    for (const char ch : number) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }

    return components.at(lastIndex - 2) == QByteArrayLiteral(".snapshots");
}

QString formatFilesystemUsageCsv(const FilesystemUsage &usage)
{
    QStringList lines;
    lines.append(QStringLiteral("kind,flags,total,used"));
    lines.append(QStringLiteral("info,%1,%2,%3")
                         .arg(QString::number(usage.nodesize),
                              QString::number(usage.sectorsize),
                              QString::number(usage.numDevices)));

    for (const SpaceEntry &space : usage.spaces) {
        lines.append(QStringLiteral("space,%1,%2,%3")
                             .arg(QString::number(space.flags),
                                  QString::number(space.totalBytes),
                                  QString::number(space.usedBytes)));
    }

    for (const DeviceEntry &device : usage.devices) {
        lines.append(QStringLiteral("device,%1,%2,%3")
                             .arg(QString::number(device.devid),
                                  QString::number(device.totalBytes),
                                  QString::number(device.bytesUsed)));
    }

    return lines.join(QLatin1Char('\n'));
}

QString formatSubvolumeListCsv(const QVector<SubvolumeEntry> &entries, bool truncated)
{
    QStringList lines;
    lines.append(truncated ? QStringLiteral("truncated,1") : QStringLiteral("truncated,0"));
    lines.append(QStringLiteral("id,parent_id,created,readonly,snapper,path"));

    for (const SubvolumeEntry &entry : entries) {
        QStringList fields;
        fields.append(QString::number(entry.id));
        fields.append(QString::number(entry.parentId));
        fields.append(QString::number(entry.otimeSec));
        fields.append(entry.readOnly ? QStringLiteral("1") : QStringLiteral("0"));
        fields.append(isSnapperSnapshotPath(entry.path) ? QStringLiteral("1")
                                                        : QStringLiteral("0"));
        fields.append(formatDisplayPathField(entry.path));
        lines.append(fields.join(QLatin1Char(',')));
    }

    return lines.join(QLatin1Char('\n'));
}

} // namespace qsnapper::btrfs
