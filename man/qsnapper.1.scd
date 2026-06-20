qsnapper(1) "June 2026" "qSnapper 1.3.4"

# NAME

qsnapper - graphical interface for Btrfs/Snapper filesystem snapshot management

# SYNOPSIS

*qsnapper* [_Qt-options_]

# DESCRIPTION

*qsnapper* is a Qt6/QML graphical application for managing filesystem snapshots
through the Snapper snapshot management tool. It supports creating, browsing,
comparing, and restoring Btrfs snapshots, and communicates with a privileged
D-Bus helper service (*qsnapper-dbus-service*(8)) for all operations that
require elevated privileges.

Privileged operations are authorized through PolicyKit. Active desktop sessions
are prompted for authentication when required; non-interactive or inactive
sessions require administrator credentials for all operations.

# OPTIONS

*qsnapper* does not define application-specific command-line options. The
following standard Qt options are accepted:

*-style* _style_
	Set the application widget style. Available styles depend on the Qt
	installation; *Fusion* is used internally regardless of this setting.

*-platform* _plugin_
	Specify the Qt platform plugin (e.g. _xcb_, _wayland_).

*-qmljsdebugger=* _options_
	Enable the QML/JS debugger. See Qt documentation for details.

*--version*
	Print the application version and exit.

# SNAPSHOT TYPES

*Single*
	A standalone snapshot with no paired counterpart.

*Pre*
	The "before" snapshot of a Pre/Post pair. Typically taken before a
	system change such as a package update.

*Post*
	The "after" snapshot paired with a specific Pre snapshot. Together, a
	Pre/Post pair captures what changed during a system operation.

# CLEANUP ALGORITHMS

An optional cleanup algorithm may be assigned to a snapshot at creation time.
Snapper will apply the chosen algorithm when performing automatic cleanup.

*Number*
	Keeps a configurable maximum number of snapshots. Oldest snapshots are
	removed when the limit is exceeded.

*Timeline*
	Retains snapshots on a time-based schedule (hourly, daily, weekly,
	monthly, yearly), removing those that fall outside the configured
	retention window.

*(none)*
	No automatic cleanup is applied. The snapshot is retained until
	manually deleted.

# USAGE

## Viewing Snapshots

The main window lists all snapshots for the selected Snapper configuration,
showing the snapshot number, type (Single/Pre/Post), creation timestamp,
creating user, and description.

## Creating Snapshots

Click *Create Snapshot* to open the creation dialog. Select the snapshot type
(Single, Pre, or Post), enter a description, optionally choose a cleanup
algorithm, and click *Create*. Creating a Post snapshot requires an existing
Pre snapshot to pair with.

## Viewing File Changes

Select a snapshot and click *Show Changes* to open the Snapshot Overview
dialog. A tree view lists files that were added, modified, or deleted relative
to the current system (for single snapshots) or between a Pre/Post pair. A
diff panel shows the unified diff for the selected file.

For Pre/Post snapshot pairs, radio buttons toggle the diff view between the
Pre and Post snapshots.

## Restoring Files

In the Snapshot Overview dialog, check the files or directories to restore,
then click *Restore Selected*. A confirmation dialog presents restore method
and batch size options before the operation proceeds. A real-time progress log
displays each file as it is restored.

For Pre/Post pairs, dedicated *Restore from Pre* and *Restore from Post*
buttons allow independent restoration from either snapshot.

*Warning*: Restoring files overwrites the current versions on disk. Review all
selected changes carefully before confirming.

# RESTORE MODES

*Direct Copy* (default)
	Copies files from the mounted snapshot using *cp*(1) with
	_--reflink=auto_. On Btrfs, this triggers copy-on-write cloning,
	making the restore near-instantaneous regardless of file size.
	Files with a type change (e.g., regular file replaced by a symlink)
	have the existing entry removed before copying.

*YaST compatible*
	Uses the same restore method as the YaST Filesystem Snapshots module
	(*cp -a* without reflink). Use this mode if the Direct Copy method
	causes compatibility issues.

Both modes accept a *batch size* (1–1000, default 100) controlling how many
files are processed per internal batch. Larger values may improve throughput;
smaller values provide more granular progress feedback.

# CONFIGURATION

## Application Settings

qSnapper stores its settings in:

	_~/.config/Presire/qSnapper.conf_

Settings written to this file include the current theme (Light or Dark), saved
window geometry, the selected restore method, and the restore batch size.

## Snapper Configuration

qSnapper uses existing Snapper configurations. To create a configuration for
the root filesystem:

```
sudo snapper -c root create-config /
```

For other filesystems:

```
sudo snapper -c CONFIG_NAME create-config MOUNT_POINT
```

See *snapper*(8) for full configuration options.

## Theme

qSnapper provides Light and Dark themes, selectable from the application UI.
The chosen theme is persisted in the application configuration file.

# FILES

_~/.config/Presire/qSnapper.conf_
	Per-user application settings (theme, window state, restore options).

_/usr/share/dbus-1/system-services/com.presire.qsnapper.Operations.service_
	D-Bus system service activation file for the privileged helper.

_/usr/share/dbus-1/system.d/com.presire.qsnapper.Operations.conf_
	D-Bus security configuration granting per-member access control.

_/usr/share/polkit-1/actions/com.presire.qsnapper.policy_
	PolicyKit action definitions governing privilege escalation.

_/usr/share/applications/qsnapper.desktop_
	Desktop entry; places qSnapper in the System Tools application category.

_/var/log/qsnapper/qsnapper-dbus.log_
	Log file written by the D-Bus helper service. The directory is
	configurable at build time with *-DQSNAPPER_LOG_DIR=*_path_.

_/usr/lib/tmpfiles.d/qsnapper.conf_
	systemd-tmpfiles configuration ensuring the log directory is created
	with mode 0700 on boot.

# D-BUS INTERFACE

*qsnapper* communicates exclusively with its privileged helper via the D-Bus
system bus interface *com.presire.qsnapper.Operations*. The helper is activated
on demand and exits automatically after five minutes of inactivity.

The interface provides the following methods:

*ListConfigs*()
	Returns the list of available Snapper configuration names.

*ListSnapshots*(_configName_)
	Returns snapshot metadata for the named configuration as CSV.

*CreateSnapshot*(_configName_, _type_, _description_, _preNumber_, _cleanup_, _userdata_, _important_)
	Creates a new snapshot. _type_ is one of _single_, _pre_, or _post_.

*ModifySnapshot*(_configName_, _number_, _description_, _cleanup_, _userdata_)
	Updates the description, cleanup algorithm, or userdata of an existing
	snapshot.

*DeleteSnapshot*(_configName_, _number_)
	Deletes the specified snapshot.

*GetFileChanges*(_configName_, _snapshotNumber_)
	Returns files changed between the snapshot and the current system.

*GetFileChangesBetween*(_configName_, _number1_, _number2_)
	Returns files changed between two snapshots.

*GetFileDiffAndDetails*(_configName_, _snapshotNumber_, _filePath_)
	Returns the unified diff and metadata for a single file.

*GetFileDiffBetween*(_configName_, _number1_, _number2_, _filePath_)
	Returns the unified diff for a file between two snapshots.

*RestoreFiles*(_configName_, _snapshotNumber_, _filePaths_, _changeTypes_)
	Restores files using the YaST-compatible copy method.

*RestoreFilesDirect*(_configName_, _snapshotNumber_, _filePaths_, _changeTypes_)
	Restores files using the Direct Copy method with Btrfs reflink support.

*IsConfigured*()
	Returns whether at least one Snapper configuration exists.

*WriteSnapperConfig*(_configName_, _settings_)
	Writes key/value settings to a Snapper configuration.

*SetupQuota*(_configName_)
	Enables Snapper quota management for the configuration.

The helper also emits the *restoreProgress*(_current_, _total_, _filePath_)
signal during file restoration, which the GUI displays in the real-time
progress log.

# SECURITY

Privileged operations are mediated by PolicyKit. The following actions are
defined:

*com.presire.qsnapper.list-snapshots*
	Listing snapshots. Allowed unconditionally for active desktop sessions.

*com.presire.qsnapper.view-diff*
	Viewing file diffs. Requires authentication (kept for the session) on
	active desktop sessions.

*com.presire.qsnapper.create-snapshot*
	Creating snapshots. Requires authentication (kept for the session).

*com.presire.qsnapper.delete-snapshot*
	Deleting snapshots. Requires authentication (kept for the session).

*com.presire.qsnapper.modify-snapshot*
	Modifying snapshot metadata. Requires authentication (kept for the
	session).

*com.presire.qsnapper.rollback-snapshot*
	Rolling back to a snapshot. Requires authentication (kept for the
	session).

*com.presire.qsnapper.configure*
	Writing Snapper configuration. Requires authentication (kept for the
	session).

Non-interactive sessions and remote sessions always require administrator
credentials for all operations.

The D-Bus service enforces per-method access control; only the methods listed
above are accessible to the GUI client. The service validates all input paths
to ensure they remain within the Snapper snapshot root before performing any
file operations.

# ENVIRONMENT

_LANG_, _LC\_ALL_, _LC\_MESSAGES_
	Determine the locale used for the user interface. qSnapper ships
	translations for English (default), Japanese (_ja_), and German (_de_).

_QT\_QPA\_PLATFORM_
	Overrides the Qt platform plugin (e.g. _wayland_, _xcb_).

_HOME_
	Used to locate the application configuration file under _~/.config_.

# DIAGNOSTICS

## D-Bus Connection Errors

Verify that the service file and security configuration are installed:

```
ls /usr/share/dbus-1/system-services/com.presire.qsnapper.Operations.service
ls /usr/share/dbus-1/system.d/com.presire.qsnapper.Operations.conf
```

Reload D-Bus after installation if needed:

```
sudo systemctl reload dbus
```

## Permission Denied

Verify that the PolicyKit policy file is installed:

```
ls /usr/share/polkit-1/actions/com.presire.qsnapper.policy
```

## Snapper Not Configured

If no Snapper configurations exist, qSnapper will indicate that Snapper is
not configured. Create a configuration as shown in the *CONFIGURATION* section.

## Log File

The D-Bus helper writes diagnostic information to
_/var/log/qsnapper/qsnapper-dbus.log_.

# SEE ALSO

*snapper*(8), *btrfs*(8), *polkit*(8), *dbus-daemon*(1),
*systemd-tmpfiles*(8), *cp*(1)

# AUTHORS

Presire <https://github.com/presire>

This manual page was written for the qSnapper project.

# COPYRIGHT

Copyright © 2025-2026 Presire. Released under the GNU General Public License,
version 2 or later. See _/usr/share/doc/qsnapper/LICENSE.md_ or
<https://www.gnu.org/licenses/gpl-2.0.html> for the full license text.
