#ifndef SNAPPERTIMERSTATE_H
#define SNAPPERTIMERSTATE_H

#include <QDBusArgument>
#include <QMap>
#include <QMetaType>
#include <QString>

/**
 * @file snappertimerstate.h
 * @brief snapperのsystemdタイマの状態をD-Bus経由で受け渡す共有型
 *
 * GUI側の SnapperService と特権D-Busサービスの両方から参照されるため、QtCoreとQtDBusだけに依存させる
 * GUI関連のヘッダ (QtGuiやQtQml) は取り込まない
 */

/**
 * @struct SnapperTimerState
 * @brief 1つのsystemdユニットの状態を表すD-Bus構造体 (ss)
 *
 * GetTimerStates が返す a{s(ss)} の値部分に対応する
 * 存在しないユニットは unitFileState が not-found、activeState が unknown になる
 */
struct SnapperTimerState
{
    QString unitFileState;  // 起動時の有効状態 (enabled / disabled / masked / static など、systemdのUnitFileState)
    QString activeState;    // 現在の稼働状態 (active / inactive / failed など、systemdのActiveState)
};

Q_DECLARE_METATYPE(SnapperTimerState)

/// ユニット名から状態への対応表 (D-Bus型 a{s(ss)})
using SnapperTimerStateMap = QMap<QString, SnapperTimerState>;

Q_DECLARE_METATYPE(SnapperTimerStateMap)

/**
 * @brief SnapperTimerStateをD-Bus構造体 (ss) として書き込む
 * @param argument 書き込み先のD-Bus引数
 * @param state 書き込む状態
 * @return 書き込み後のD-Bus引数
 */
inline QDBusArgument &operator<<(QDBusArgument &argument, const SnapperTimerState &state)
{
    argument.beginStructure();
    argument << state.unitFileState << state.activeState;
    argument.endStructure();
    return argument;
}

/**
 * @brief D-Bus構造体 (ss) をSnapperTimerStateとして読み出す
 * @param argument 読み出し元のD-Bus引数
 * @param state 読み出し先の状態
 * @return 読み出し後のD-Bus引数
 * @note 構造体の形が (ss) と異なる場合はQDBusArgument側がエラー状態になり、state は不完全なままになる
 */
inline const QDBusArgument &operator>>(const QDBusArgument &argument, SnapperTimerState &state)
{
    argument.beginStructure();
    argument >> state.unitFileState >> state.activeState;
    argument.endStructure();
    return argument;
}

#endif // SNAPPERTIMERSTATE_H
