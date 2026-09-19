#ifndef WINDOWSTATEMANAGER_H
#define WINDOWSTATEMANAGER_H

#include <QObject>

class QQuickWindow;

/**
 * @brief メインウィンドウのサイズと最大化状態を永続化するクラス
 *
 * アプリケーション終了時に通常状態 (非最大化) の幅・高さと最大化状態をQSettingsへ保存し、
 * 次回起動時にウィンドウへ復元する
 * 最大化中は通常状態のサイズを追跡して保持することで、最大化解除時に元のサイズへ戻せるようにする
 *
 * 設定キー (グループ window) は既存の theme/mode と同じ形式で保存する:
 * - window/width     (int,  既定1280)
 * - window/height    (int,  既定900)
 * - window/maximized (bool, 既定false)
 */
class WindowStateManager : public QObject
{
    Q_OBJECT

public:
    // コンストラクタ/デストラクタ
    explicit WindowStateManager(QObject *parent = nullptr);    // コンストラクタ
    ~WindowStateManager();                                      // デストラクタ

    /**
     * @brief 保存済みのウィンドウ状態を復元する
     *
     * 設定を読み込み、画面の利用可能領域に収まるようクランプしてからウィンドウへ適用する
     * 適用後は通常状態のサイズ追跡と、終了時の保存フックの接続を開始する
     *
     * @param window 対象のウィンドウ
     */
    void restore(QQuickWindow *window); // ウィンドウ状態を復元する

    /**
     * @brief 現在のウィンドウ状態を保存する
     *
     * 最大化中は追跡している通常サイズを、通常状態では現在のサイズを保存する
     * 冪等であり、複数回呼び出しても安全
     *
     * @param window 対象のウィンドウ
     */
    void save(QQuickWindow *window);    // ウィンドウ状態を保存する

private:
    // サイズのクランプ
    int clampWidth(QQuickWindow *window, int requestedWidth) const;     // 幅を画面内にクランプする
    int clampHeight(QQuickWindow *window, int requestedHeight) const;   // 高さを画面内にクランプする

    // 追跡状態
    bool m_restored;            // 復元済みフラグ (二重接続防止)
    int m_normalWidth;          // 通常状態の幅
    int m_normalHeight;         // 通常状態の高さ
};

#endif // WINDOWSTATEMANAGER_H
