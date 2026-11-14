/**
 * @file tst_temprootguard.h
 * @brief root実行時のテスト用一時ディレクトリの基点を、信頼済みの/runへ揃える共有fixture
 *
 * QTemporaryDirの既定の基点は/tmpであり、/tmpは全員が書き込み可能なため、
 * root実行時は製品のsafeOpenDirectoryが祖先を信頼できないと判定してEPERMを返す
 * この拒否はテストが意図的に検証する攻撃経路でもあるため、正規の経路を検証するテストが
 * 同じ基点を使うと、テストの前提と製品の安全判定が食い違って失敗する
 * そのためeuid 0のときだけ、/runを製品と同じ判定で検証してからTMPDIRを切り替える
 *
 * 利用する側 : tst_filesystemhelpers.cppとtst_snapshotoperations.cppのinitTestCase/cleanupTestCase
 * 依存 : filesystemhelpers.h(qsnapper::security::safeOpenDirectory)、Qt Core
 */
#ifndef TST_TEMPROOTGUARD_H
#define TST_TEMPROOTGUARD_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "filesystemhelpers.h"

namespace qsnapper::test {

/**
 * @class TempRootGuard
 * @brief root実行時だけ、テスト用一時ディレクトリの基点を信頼済みの/runへ切り替えるfixture
 *
 * 非rootではTMPDIRを変更しない
 * 切り替え前のTMPDIRが未設定だったか空文字列だったかを区別して保存し、スイート終了時に同じ状態へ戻す
 * 未設定と空文字列はQtの既定値の扱いが同じでも環境の状態としては別物であり、
 * 同じプロセスで後続の処理へ環境変数の変更を持ち越さないためである
 *
 * m_changed : TMPDIRを/runへ切り替えた場合true
 * m_wasSet : 切り替え前にTMPDIRが設定されていた場合true (空文字列も設定済みとして扱う)
 * m_originalValue : 切り替え前のTMPDIRの値
 */
class TempRootGuard
{
public:
    /**
     * @brief rootのときだけ/runの信頼性を確認し、TMPDIRを/runへ切り替える
     * @param errorMessage 失敗した理由の返却先
     * @return 切り替えが不要または成功した場合true
     * @note 検証には製品と同じsafeOpenDirectoryを使い、テスト側だけで信頼判定を再実装しない
     *       検証に失敗した場合はTMPDIRを変更せずfalseを返し、呼び出し側がスキップではなく失敗として報告する
     */
    bool prepare(QString *errorMessage)
    {
        Q_ASSERT(errorMessage != nullptr);
        errorMessage->clear();

        // 非rootではカーネルが利用者自身の権限で検索を検査するため、/tmpのままでも正規の経路を検証できる
        if (::geteuid() != 0) {
            return true;
        }

        errno = 0;
        const int runFd = qsnapper::security::safeOpenDirectory(QStringLiteral("/run"));
        const int openError = errno;
        if (runFd < 0) {
            *errorMessage = QStringLiteral("/runをテストの一時ディレクトリの基点にできない: %1")
                                    .arg(QString::fromLocal8Bit(std::strerror(openError)));
            return false;
        }
        ::close(runFd);

        m_wasSet = qEnvironmentVariableIsSet("TMPDIR");
        m_originalValue = qgetenv("TMPDIR");
        qputenv("TMPDIR", QByteArrayLiteral("/run"));
        m_changed = true;
        return true;
    }

    /**
     * @brief prepareで切り替えたTMPDIRを、スイート開始前の状態へ戻す
     * @note 未設定だった場合はqunsetenvで削除し、空文字列だった場合は空文字列として設定し直す
     */
    void restore()
    {
        if (!m_changed) {
            return;
        }
        if (m_wasSet) {
            qputenv("TMPDIR", m_originalValue);
        } else {
            qunsetenv("TMPDIR");
        }
        m_changed = false;
    }

private:
    bool m_changed = false;         // TMPDIRを/runへ切り替えた場合true
    bool m_wasSet = false;          // 切り替え前にTMPDIRが設定されていた場合true
    QByteArray m_originalValue;     // 切り替え前のTMPDIRの値
};

} // namespace qsnapper::test

#endif // TST_TEMPROOTGUARD_H
