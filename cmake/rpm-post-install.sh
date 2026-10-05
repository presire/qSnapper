#!/bin/bash
# RPM %postスクリプト: SELinuxポリシーの読み込み、ファイルの再ラベル、ログディレクトリの作成を行う

POLICY_PP="/usr/share/selinux/packages/qsnapper.pp"

if [ -x /usr/sbin/semodule ] && [ -f "$POLICY_PP" ]; then
    /usr/sbin/semodule -i "$POLICY_PP" >/dev/null 2>&1 || :
fi

if [ -x /usr/sbin/restorecon ]; then
    /usr/sbin/restorecon -R /usr/bin/qsnapper >/dev/null 2>&1 || :
    /usr/sbin/restorecon -R /usr/libexec/qsnapper-dbus-service >/dev/null 2>&1 || :
fi

# インストール済みtmpfiles.dエントリから/var/log/qsnapper (0700 root) を作成する
if [ -x /usr/bin/systemd-tmpfiles ]; then
    /usr/bin/systemd-tmpfiles --create qsnapper.conf >/dev/null 2>&1 || :
fi
