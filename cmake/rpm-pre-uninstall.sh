#!/bin/bash
#
# @file rpm-pre-uninstall.sh
# @brief RPMアンインストール前にSELinuxポリシーモジュールを削除する
#
# アップグレード時ではなく完全削除時のみポリシーモジュールを削除する
# semoduleが存在しない環境では何もしない
#
# 使い方 : RPMの[%preun] scriptletから第1引数付きで呼び出される
# 終了コード : 常に0 (ポリシー削除に失敗してもアンインストールを止めない)
#

# 第1引数は完全削除で0、アップグレードで1
if [ "$1" -eq 0 ]; then
    # semoduleの失敗や不在でパッケージ操作を止めない
    if [ -x /usr/sbin/semodule ]; then
        /usr/sbin/semodule -r qsnapper >/dev/null 2>&1 || :
    fi
fi
