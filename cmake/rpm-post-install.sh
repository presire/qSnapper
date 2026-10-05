#!/bin/bash
#
# @file rpm-post-install.sh
# @brief RPMインストール後にSELinuxポリシーを読み込み、ファイルを再ラベルし、ログディレクトリを作成する
#
# semoduleでポリシーモジュールを読み込み、restoreconで実行ファイルに正しいSELinuxコンテキストを付与し、
# tmpfiles.dエントリからログディレクトリ/var/log/qsnapperを作成する
# 各ツールやポリシーファイルが無い環境では該当処理をスキップする
#
# 使い方 : RPMの[%post] scriptletから引数なしで呼び出される
# 終了コード : 常に0 (各処理の失敗でインストールを止めない)
#

POLICY_PP="/usr/share/selinux/packages/qsnapper.pp"    # インストールされたSELinuxポリシーモジュール

# ポリシーモジュールをカーネルに読み込む
if [ -x /usr/sbin/semodule ] && [ -f "$POLICY_PP" ]; then
    /usr/sbin/semodule -i "$POLICY_PP" >/dev/null 2>&1 || :
fi

# 実行ファイルに正しいSELinuxコンテキストを再付与する
if [ -x /usr/sbin/restorecon ]; then
    /usr/sbin/restorecon -R /usr/bin/qsnapper >/dev/null 2>&1 || :
    /usr/sbin/restorecon -R /usr/libexec/qsnapper-dbus-service >/dev/null 2>&1 || :
fi

# tmpfiles.dエントリから/var/log/qsnapperを0700 rootで作成する
if [ -x /usr/bin/systemd-tmpfiles ]; then
    /usr/bin/systemd-tmpfiles --create qsnapper.conf >/dev/null 2>&1 || :
fi
