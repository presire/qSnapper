/**
 * @file configpathvalidation.cpp
 * @brief config作成と削除で扱うパスの検査の実装
 *
 * 成分を1つずつO_PATHとO_NOFOLLOWで開き、開いたfdに対してfstatで検査する
 * 名前で検査してから名前で開き直すと、その間に差し替えられた対象を見落とすため、検査は常に開いたfdに対して行う
 */

#include "configpathvalidation.h"

#include <QByteArray>
#include <QList>
#include <QRegularExpression>
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/statfs.h>
#include <unistd.h>

namespace qsnapper::security {

    namespace {

        constexpr int MaxPathBytes = 4095;          // パス全体の上限 (PATH_MAXから終端のNULを除いた長さ)
        constexpr int MaxComponentBytes = 255;      // 1成分の上限 (NAME_MAX)
        constexpr ino_t BtrfsSubvolumeRootIno = 256; // Btrfsのsubvolumeの最上位ディレクトリのinode番号

        /**
         * @class ScopedFd
         * @brief 例外や早期のreturnでもfdを閉じ忘れないための所有者
         */
        class ScopedFd
        {
        public:
            explicit ScopedFd(int fd = -1) : m_fd(fd) {}
            ~ScopedFd() { reset(); }
            ScopedFd(const ScopedFd &) = delete;
            ScopedFd &operator=(const ScopedFd &) = delete;

            int get() const { return m_fd; }

            /**
             * @brief 所有を手放す
             * @return 手放したfd
             */
            int release()
            {
                const int fd = m_fd;
                m_fd = -1;
                return fd;
            }

            /**
             * @brief 所有するfdを閉じて、新しいfdを所有する
             * @param fd 新しく所有するfd
             */
            void reset(int fd = -1)
            {
                if (m_fd >= 0) {
                    ::close(m_fd);
                }
                m_fd = fd;
            }

        private:
            int m_fd;   // 所有するfd (所有しない場合は-1)
        };

        /**
         * @brief rootが所有し、groupとotherが書き込めないかを判定する
         *
         * stickyビットの付いたディレクトリ (/tmpなど) も、他の利用者が成分を作れるため信頼しない
         * @param st 検査するfstatの結果
         * @return rootだけが内容を変えられる場合: true
         */
        bool isRootOnlyWritable(const struct stat &st)
        {
            return st.st_uid == 0 && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
        }

        /**
         * @brief 検証済みの絶対パスを成分へ分ける
         * @param absolutePath 検証済みの絶対パス
         * @param components 成分を受け取る (「/」の場合は空)
         * @return 形式と長さが正しい場合: true
         */
        bool splitComponents(const QString &absolutePath, QList<QByteArray> *components)
        {
            const QByteArray encoded = absolutePath.toUtf8();
            if (encoded.isEmpty() || encoded.size() > MaxPathBytes || !encoded.startsWith('/')
                    || encoded.contains('\0')) {
                return false;
            }

            components->clear();
            for (const QByteArray &part : encoded.mid(1).split('/')) {
                if (part.isEmpty()) {
                    // 「/」そのものは成分を持たない
                    // それ以外の空の成分は「//」か末尾の「/」であり、正規化されていないため拒否する
                    if (encoded.size() == 1) {
                        continue;
                    }
                    return false;
                }
                if (part == "." || part == ".." || part.size() > MaxComponentBytes) {
                    return false;
                }
                components->append(part);
            }
            return true;
        }

        /**
         * @brief 「/」から親ディレクトリまでを1成分ずつ開いて検査する
         * @param components パスの成分
         * @param depth 開くディレクトリの成分数 (0は「/」)
         * @param fdOut 成功時に最後に開いたディレクトリのfdを受け取る
         * @return 検査結果
         */
        TrustedPathResult walkDirectories(const QList<QByteArray> &components, qsizetype depth, ScopedFd *fdOut)
        {
            ScopedFd current(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
            if (current.get() < 0) {
                return TrustedPathResult::Failed;
            }

            struct stat st {};
            if (::fstat(current.get(), &st) != 0) {
                return TrustedPathResult::Failed;
            }
            if (!isRootOnlyWritable(st)) {
                return TrustedPathResult::Untrusted;
            }

            for (qsizetype i = 0; i < depth; ++i) {
                // O_NOFOLLOWとO_PATHを併用すると、symlinkはsymlinkそのものとして開かれ、fstatで判別できる
                ScopedFd next(::openat(current.get(), components.at(i).constData(),
                                       O_PATH | O_NOFOLLOW | O_CLOEXEC));
                if (next.get() < 0) {
                    return errno == ENOENT ? TrustedPathResult::Missing : TrustedPathResult::Failed;
                }
                if (::fstat(next.get(), &st) != 0) {
                    return TrustedPathResult::Failed;
                }
                if (S_ISLNK(st.st_mode)) {
                    return TrustedPathResult::SymbolicLink;
                }
                if (!S_ISDIR(st.st_mode)) {
                    return TrustedPathResult::WrongType;
                }
                if (!isRootOnlyWritable(st)) {
                    return TrustedPathResult::Untrusted;
                }
                current.reset(next.release());
            }

            fdOut->reset(current.release());
            return TrustedPathResult::Ok;
        }

        /**
         * @brief 親ディレクトリを検査し、最後の成分をsymlinkを辿らずにfstatする
         * @param absolutePath 検証済みの絶対パス
         * @param st 最後の成分のfstatの結果を受け取る
         * @return 検査結果 (最後の成分が存在しない場合はMissing)
         */
        TrustedPathResult statFinalComponent(const QString &absolutePath, struct stat *st)
        {
            QList<QByteArray> components;
            if (!splitComponents(absolutePath, &components) || components.isEmpty()) {
                return TrustedPathResult::Failed;
            }

            ScopedFd parent;
            const TrustedPathResult parentResult = walkDirectories(components, components.size() - 1, &parent);
            if (parentResult != TrustedPathResult::Ok) {
                return parentResult;
            }

            ScopedFd target(::openat(parent.get(), components.last().constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
            if (target.get() < 0) {
                return errno == ENOENT ? TrustedPathResult::Missing : TrustedPathResult::Failed;
            }
            if (::fstat(target.get(), st) != 0) {
                return TrustedPathResult::Failed;
            }
            return TrustedPathResult::Ok;
        }

    } // namespace

    bool validateNewConfigName(const QString &name)
    {
        static const QRegularExpression pattern(
            QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")));
        return pattern.match(name).hasMatch();
    }

    bool validateConfigSubvolumePath(const QString &path)
    {
        static const QRegularExpression pattern(
            QRegularExpression::anchoredPattern(QStringLiteral("/[A-Za-z0-9._@+/-]*")));
        if (!pattern.match(path).hasMatch()) {
            return false;
        }
        QList<QByteArray> components;
        return splitComponents(path, &components);
    }

    TrustedPathResult openTrustedDirectory(const QString &absolutePath, int *fdOut, struct stat *statOut)
    {
        *fdOut = -1;
        QList<QByteArray> components;
        if (!splitComponents(absolutePath, &components)) {
            return TrustedPathResult::Failed;
        }

        ScopedFd fd;
        const TrustedPathResult result = walkDirectories(components, components.size(), &fd);
        if (result != TrustedPathResult::Ok) {
            return result;
        }
        if (statOut && ::fstat(fd.get(), statOut) != 0) {
            return TrustedPathResult::Failed;
        }
        *fdOut = fd.release();
        return TrustedPathResult::Ok;
    }

    TrustedPathResult checkTrustedRegularFile(const QString &absolutePath)
    {
        struct stat st {};
        const TrustedPathResult result = statFinalComponent(absolutePath, &st);
        if (result != TrustedPathResult::Ok) {
            return result;
        }
        if (S_ISLNK(st.st_mode)) {
            return TrustedPathResult::SymbolicLink;
        }
        if (!S_ISREG(st.st_mode)) {
            return TrustedPathResult::WrongType;
        }
        if (!isRootOnlyWritable(st) || st.st_nlink != 1) {
            return TrustedPathResult::Untrusted;
        }
        return TrustedPathResult::Ok;
    }

    TrustedPathResult checkTrustedAbsent(const QString &absolutePath)
    {
        struct stat st {};
        const TrustedPathResult result = statFinalComponent(absolutePath, &st);
        if (result == TrustedPathResult::Missing) {
            // 親ディレクトリの検査は通過しており、存在しないのは最後の成分だけである
            // (親が存在しない場合はwalkDirectoriesが先にMissingを返すが、その場合も作成先として使えないため拒否する)
            QList<QByteArray> components;
            splitComponents(absolutePath, &components);
            ScopedFd parent;
            return walkDirectories(components, components.size() - 1, &parent) == TrustedPathResult::Ok
                       ? TrustedPathResult::Ok
                       : TrustedPathResult::Missing;
        }
        if (result != TrustedPathResult::Ok) {
            return result;
        }
        return TrustedPathResult::WrongType;
    }

    bool isBtrfsSubvolumeRoot(int fd, struct stat *statOut)
    {
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_ino != BtrfsSubvolumeRootIno) {
            return false;
        }
        struct statfs fs {};
        if (::fstatfs(fd, &fs) != 0 || static_cast<unsigned long>(fs.f_type) != BTRFS_SUPER_MAGIC) {
            return false;
        }
        if (statOut) {
            *statOut = st;
        }
        return true;
    }

} // namespace qsnapper::security
