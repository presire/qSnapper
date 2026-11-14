/**
 * @file tst_filestatus.cpp
 * @brief qsnapper::internal::formatFileStatus()の単体テスト
 *
 * ファイル変更の一覧と詳細で、状態文字列の表記が共通化前と同じであることを固定する
 * クライアントは先頭の文字と各文字の有無で変更内容を判定するため、順序と桁埋めが変わると表示が壊れる
 * 依存 : snapshotoperations_internal.h、snapper/File.h、Qt Test
 */

#include <QtTest/QtTest>
#include <QString>
#include "snapshotoperations_internal.h"

using qsnapper::internal::formatFileStatus;

namespace {

/**
 * @brief 共通化前の4箇所と同じ手順で状態文字列を組み立てる参照実装
 *
 * 製品コードと独立に期待値を作るため、フラグと文字の対応をテスト側で持つ
 *
 * @param status 変更フラグ
 * @return 共通化前の実装が返していた状態文字列
 */
QString legacyStatusText(unsigned int status)
{
    static const struct {
        unsigned int flag;                      // snapperの変更フラグ
        char letter;                            // 表記に使う文字
    } table[] = {
        {1u, '+'}, {2u, '-'}, {4u, 't'}, {8u, 'c'}, {16u, 'p'},
        {32u, 'u'}, {64u, 'g'}, {128u, 'x'}, {256u, 'a'},
    };

    QString text;
    for (const auto &entry : table) {
        if (status & entry.flag) {
            text += QLatin1Char(entry.letter);
        }
    }
    if (text.isEmpty()) {
        text = QStringLiteral(".....");
    }
    return text.leftJustified(5, QLatin1Char('.'));
}

} // namespace

/**
 * @class TestFileStatus
 * @brief 状態文字列の変換のテスト
 */
class TestFileStatus : public QObject
{
    Q_OBJECT

private slots:
    void flagValuesMatchSnapper();
    void representativeValues();
    void allCombinationsMatchLegacyFormat();
    void unknownBitsAreIgnored();
};

/**
 * @brief テストが前提にするフラグの値が、snapperの定義と一致することを確認する
 *
 * 参照実装は数値でフラグを持つため、snapperの定義が変わった場合はここで検出する
 */
void TestFileStatus::flagValuesMatchSnapper()
{
    QCOMPARE(static_cast<unsigned int>(snapper::CREATED), 1u);
    QCOMPARE(static_cast<unsigned int>(snapper::DELETED), 2u);
    QCOMPARE(static_cast<unsigned int>(snapper::TYPE), 4u);
    QCOMPARE(static_cast<unsigned int>(snapper::CONTENT), 8u);
    QCOMPARE(static_cast<unsigned int>(snapper::PERMISSIONS), 16u);
    QCOMPARE(static_cast<unsigned int>(snapper::OWNER), 32u);
    QCOMPARE(static_cast<unsigned int>(snapper::GROUP), 64u);
    QCOMPARE(static_cast<unsigned int>(snapper::XATTRS), 128u);
    QCOMPARE(static_cast<unsigned int>(snapper::ACL), 256u);
}

/**
 * @brief 代表的な値の表記を確認する
 *
 * 変更なしは「.....」、5文字未満は「.」で埋め、6文字以上は切り詰めないことを確認する
 */
void TestFileStatus::representativeValues()
{
    QCOMPARE(formatFileStatus(0), QStringLiteral("....."));
    QCOMPARE(formatFileStatus(snapper::CREATED), QStringLiteral("+...."));
    QCOMPARE(formatFileStatus(snapper::DELETED), QStringLiteral("-...."));
    QCOMPARE(formatFileStatus(snapper::CONTENT | snapper::PERMISSIONS), QStringLiteral("cp..."));
    QCOMPARE(formatFileStatus(snapper::ACL | snapper::TYPE), QStringLiteral("ta..."));
    QCOMPARE(formatFileStatus(snapper::TYPE | snapper::CONTENT | snapper::PERMISSIONS
                              | snapper::OWNER | snapper::GROUP),
             QStringLiteral("tcpug"));
    QCOMPARE(formatFileStatus(0x1ffu), QStringLiteral("+-tcpugxa"));
}

/**
 * @brief 既知の9フラグの全ての組み合わせで、共通化前の表記と一致することを確認する
 */
void TestFileStatus::allCombinationsMatchLegacyFormat()
{
    for (unsigned int status = 0; status <= 0x1ffu; ++status) {
        QCOMPARE(formatFileStatus(status), legacyStatusText(status));
    }
}

/**
 * @brief 未知のビットが表記に影響しないことを確認する
 *
 * 将来のsnapperが新しいフラグを追加しても、既知のフラグの表記を変えないためである
 */
void TestFileStatus::unknownBitsAreIgnored()
{
    const unsigned int unknownBits[] = {0x200u, 0x400u, 0x8000u, 0x80000000u, ~0x1ffu};
    for (const unsigned int unknown : unknownBits) {
        QCOMPARE(formatFileStatus(unknown), QStringLiteral("....."));
        QCOMPARE(formatFileStatus(unknown | snapper::CONTENT), QStringLiteral("c...."));
        QCOMPARE(formatFileStatus(unknown | 0x1ffu), QStringLiteral("+-tcpugxa"));
    }
}

QTEST_MAIN(TestFileStatus)
#include "tst_filestatus.moc"
