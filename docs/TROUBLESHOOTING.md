# qSnapper - Troubleshooting

[Back to README](../README.md) | [日本語](TROUBLESHOOTING_JP.md)

## Troubleshooting

### D-Bus Connection Errors

If you see D-Bus connection errors:  

1. Check if the D-Bus service file is installed:  
   
   ```bash
   ls /usr/share/dbus-1/system-services/com.presire.qsnapper.Operations.service
   ```

2. Verify D-Bus configuration:  
   
   ```bash
   ls /usr/share/dbus-1/system.d/com.presire.qsnapper.Operations.conf
   ```

3. Check D-Bus service status:  
   
   ```bash
   systemctl status dbus
   ```

### Permission Denied

If operations fail with permission errors:  

1. Verify PolicyKit policy is installed:  
   
   ```bash
   ls /usr/share/polkit-1/actions/com.presire.qsnapper.policy
   ```

2. Ensure your user is in the required groups (implementation-specific)  

### Snapper Not Configured

If Snapper is not configured, the snapshot list is empty.  
Check the existing configurations:  

```bash
sudo snapper list-configs
```

If no configuration exists for the subvolume you want to use, create one from the Settings tab with "New..." or from the command line as shown in the [Configuration](USAGE.md#configuration) section.  
