# qSnapper - トラブルシューティング

[READMEへ戻る](../README_JP.md) | [English](TROUBLESHOOTING.md)

## トラブルシューティング

### D-Bus接続エラー

D-Bus接続エラーが表示される場合：  

1. D-Busサービスファイルがインストールされているか確認：  
   
   ```bash
   ls /usr/share/dbus-1/system-services/com.presire.qsnapper.Operations.service
   ```

2. D-Bus設定を確認：  
   
   ```bash
   ls /usr/share/dbus-1/system.d/com.presire.qsnapper.Operations.conf
   ```

3. D-Busサービスのステータスを確認：  
   
   ```bash
   systemctl status dbus
   ```

### 権限拒否エラー

権限エラーで操作が失敗する場合：  

1. PolicyKitポリシーがインストールされているか確認：  
   
   ```bash
   ls /usr/share/polkit-1/actions/com.presire.qsnapper.policy
   ```

2. ユーザが必要なグループに所属しているか確認 (実装固有)  

### Snapperが設定されていない

Snapperが設定されていない場合、スナップショット一覧は空になります。  
既存の設定を確認：  

```bash
sudo snapper list-configs
```

使用するサブボリュームの設定が存在しない場合は、設定タブの「新規...」または[設定](USAGE_JP.md#設定)セクションに示されているコマンドで作成してください。  
