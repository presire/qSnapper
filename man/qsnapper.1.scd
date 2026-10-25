qsnapper(1) "October 2026" "qSnapper 1.3.10"

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

The main window has three tabs. *Snapshots* lists, creates, modifies, compares,
and deletes snapshots. *Settings* edits the selected Snapper configuration,
manages the snapper systemd timers, creates and deletes configurations, and
sets up Btrfs quota. *Filesystem* shows read-only Btrfs allocation, usage, and
subvolume information for the selected configuration.

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
	Pre/Post pair captures what changed during a system operation. A Post
	snapshot can be created only while a Pre snapshot without a Post
	snapshot is selected; the new snapshot is paired with it.

Snapshots can be marked as important when they are created. Important snapshots
are shown with *!* in the snapshot list and are counted against the separate
_NUMBER_LIMIT_IMPORTANT_ setting during number cleanup.

# CLEANUP ALGORITHMS

An optional cleanup algorithm may be assigned to a snapshot at creation time.
Snapper will apply the chosen algorithm when performing automatic cleanup. The
creation dialog offers *None*, *Number*, and *Timeline*. The algorithm and the
description can be changed later with *Modify*.

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

The Snapshots tab lists all snapshots for the selected Snapper configuration,
showing the snapshot number, type (Single/Pre/Post), important marker (*!*),
start and end timestamps, description, and user data. Pre/Post pairs are grouped
into one row, and selecting a pair selects both snapshots. Checkboxes allow
selecting multiple snapshots for bulk deletion.

## Creating Snapshots

Click *Create Snapshot* to open the creation dialog. Enter a description, then
choose the snapshot type:

*Single*
	Creates a standalone snapshot.

*Pre*
	Creates a pre snapshot that can be paired with a post snapshot later.

*Post*
	Available only while a pre snapshot without a post snapshot is selected.
	The new snapshot is paired with that pre snapshot.

Optionally choose a cleanup algorithm (*None*, *Number*, or *Timeline*), mark
the snapshot as important, and enter user data as *key=value* lines or
comma-separated pairs. Then click *Create*.

## Deleting Snapshots

Select one or more snapshots with the checkboxes and click *Delete Selected*.
The whole selection is sent as a single request and requires one PolicyKit
authorization. If some snapshots cannot be deleted, the remaining snapshots are
deleted and the number of failures is reported.

## Viewing File Changes

Select a snapshot and click *Show Changes* to open the Snapshot Overview
dialog. A tree view lists files that were added, modified, or deleted relative
to the current system (for single snapshots) or between a Pre/Post pair. A
diff panel shows the unified diff for the selected file.

For Pre/Post pairs, radio buttons choose between reverting to the Pre snapshot,
re-applying to the Post snapshot, and viewing either snapshot against the
current system. The two current-system views are read-only.

## Restoring Files

In the Snapshot Overview dialog, check the files or directories to restore,
then click *Restore Selected*. The confirmation dialog contains restore method
and batch size options. For Pre/Post pairs the button reads *Restore Selected
to #N* and restores only the entries that differ between Pre and Post. A
real-time progress log displays each file as it is restored.

*Warning*: Restoring files overwrites the current versions on disk. Review all
selected changes carefully before confirming.

## Settings

The Settings tab edits the selected Snapper configuration. Configuration
information (subvolume, filesystem type, and qgroup) is read-only. Number
cleanup, timeline cleanup, empty pre/post pair cleanup, space limits, and
background comparison values can be changed. The *Timeline* creation setting
only takes effect while *snapper-timeline.timer* is enabled.

The tab also lists the snapper systemd timers and allows enabling or disabling
them. The timers are shared by all configurations.

*New...* creates a configuration for a mounted Btrfs subvolume. The .snapshots
subvolume is created automatically, and the filesystem root cannot be used.
*Delete...* deletes a configuration after its name is typed to confirm; all
snapshots in that configuration are deleted as well. The root configuration
cannot be created or deleted from the Settings tab.

When a configuration has no qgroup yet, *Set up quota* enables Btrfs quota
after a confirmation prompt. Quota setup cannot be undone from qSnapper, and
the space limit settings only take effect once quota is enabled.

## Filesystem

The Filesystem tab shows read-only Btrfs information for the selected
configuration: overall size, allocated and unallocated space, node size,
sector size, space by chunk type, and per-device size and allocation. A
subvolume list can be loaded on demand with an option to hide Snapper
snapshots.

The values come from the kernel Btrfs ioctls and can differ from the output of
*btrfs filesystem usage*. Loading the subvolume list requires authentication
and is read-only. Long lists are capped and marked as truncated.

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
files are submitted per staging chunk before the restore plan is frozen.
Larger values may improve throughput; smaller values provide more granular
progress feedback. Regardless of how many files or chunks are selected, a
multi-file restore requires exactly one PolicyKit authorization at commit
time, with no re-prompt while the frozen plan executes.

# CONFIGURATION

## Application Settings

qSnapper stores its settings in:

	_~/.config/Presire/qSnapper.conf_

Settings written to this file include the current theme (Light, Dark, or
System), saved window geometry, the selected restore method, and the restore
batch size.

## Snapper Configuration

qSnapper uses existing Snapper configurations and can create and delete them
from the Settings tab. Creating or deleting a configuration, and deleting
several snapshots at once, is performed through the Snapper D-Bus service
*org.opensuse.Snapper*, which is provided by the *snapper* package and must be
available.

The root configuration cannot be created or deleted in the Settings tab. To
create a configuration for the root filesystem, use:

```
sudo snapper -c root create-config /
```

For other filesystems:

```
sudo snapper -c CONFIG_NAME create-config MOUNT_POINT
```

See *snapper*(8) for full configuration options.

## Theme

qSnapper provides Light, Dark, and System themes, selectable from the
application UI. System follows the desktop's light or dark preference. The
chosen theme is persisted per user in the application configuration file and
is applied when Save is pressed in the Settings tab.

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

*DeleteSnapshots*(_configName_, _numbers_)
	Deletes several snapshots in one request with a single authorization.
	Numbers must be greater than zero; duplicates are removed and the batch
	is limited to 100 snapshots.

*RollbackSnapshot*(_configName_, _number_)
	Rolls the system back to the specified snapshot. A reboot is required
	to complete the rollback.

*GetFileChanges*(_configName_, _snapshotNumber_)
	Returns files changed between the snapshot and the current system.

*GetFileChangesBetween*(_configName_, _number1_, _number2_)
	Returns files changed between two snapshots.

*GetFileDiffAndDetails*(_configName_, _snapshotNumber_, _filePath_)
	Returns the unified diff and metadata for a single file.

*GetFileDiffBetween*(_configName_, _number1_, _number2_, _filePath_)
	Returns the unified diff for a file between two snapshots.

*BeginRestorePlan*(_configName_, _snapshotNumber_, _counterpartSnapshotNumber_, _restoreMode_)
	Starts a restore plan bound to the caller. _restoreMode_ is "yast" or
	"direct" (Direct Copy with Btrfs reflink support).

*StageRestoreEntries*(_manifestId_, _filePaths_, _changeTypes_)
	Adds a bounded chunk of config-relative paths to a restore plan.

*CommitRestorePlan*(_manifestId_)
	Freezes the plan, authorizes it once and starts the restore.

*ContinueRestorePlan*(_manifestId_), *GetRestorePlanStatus*(_manifestId_), *CancelRestorePlan*(_manifestId_)
	Keep alive, query and cancel a restore plan.

*IsConfigured*()
	Returns whether at least one Snapper configuration exists.

*WriteSnapperConfig*(_configName_, _settings_)
	Writes key/value settings to a Snapper configuration.

*SetupQuota*(_configName_)
	Enables Snapper quota management for the configuration.

*GetConfig*(_configName_)
	Returns the editable settings plus the subvolume, filesystem type, and
	qgroup of the configuration.

*GetTimerStates*(), *SetTimerEnabled*(_unit_, _enabled_)
	Read and change the state of the fixed Snapper timer units
	(*snapper-timeline.timer*, *snapper-cleanup.timer*,
	*snapper-boot.timer*). The unit file state and the active state are
	reported separately.

*CreateConfig*(_configName_, _subvolume_)
	Creates a Snapper configuration for a mounted Btrfs subvolume through
	*org.opensuse.Snapper*. The root configuration is never created here.

*DeleteConfig*(_configName_)
	Deletes a Snapper configuration through *org.opensuse.Snapper*. The
	root configuration is never deleted here.

*GetFilesystemUsage*(_configName_)
	Returns read-only Btrfs size, allocation, per-chunk-type, and
	per-device usage as CSV text.

*ListSubvolumes*(_configName_)
	Returns the subvolume list as CSV text. The list is capped by count,
	total path size, and elapsed time, and the first line reports whether
	it was truncated.

The helper also emits the *restorePlanProgress*(_manifestId_, _current_, _total_, _filePath_)
and *restorePlanFinished*(_manifestId_, _terminalState_, _message_) signals
during file restoration, which the GUI displays in the real-time progress log.

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

*com.presire.qsnapper.view-config*
	Reading Snapper configuration values. Allowed for active desktop
	sessions.

*com.presire.qsnapper.view-timers*
	Reading systemd timer states. Allowed for active desktop sessions.

*com.presire.qsnapper.manage-timers*
	Enabling or disabling systemd timers. Requires authentication (kept for
	the session).

*com.presire.qsnapper.setup-quota*
	Enabling Btrfs quota. Requires authentication (kept for the session).

*com.presire.qsnapper.create-config*
	Creating a Snapper configuration. Requires administrator credentials
	for all sessions.

*com.presire.qsnapper.delete-config*
	Deleting a Snapper configuration. Requires administrator credentials
	and is not kept for the session.

*com.presire.qsnapper.delete-snapshots*
	Deleting several snapshots in one request. Requires administrator
	credentials and is not kept for the session.

*com.presire.qsnapper.view-filesystem*
	Reading filesystem allocation and usage. Allowed for active desktop
	sessions.

*com.presire.qsnapper.list-subvolumes*
	Listing subvolumes. Requires authentication (kept for the session).

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

If no Snapper configurations exist, the snapshot list is empty. Check the
available configurations with *snapper list-configs*. A configuration for a
non-root Btrfs subvolume can be created from the *Settings* tab with *New...*;
the root configuration must be created from the command line as shown in the
*CONFIGURATION* section.

## Log File

The D-Bus helper writes diagnostic information to
_/var/log/qsnapper/qsnapper-dbus.log_.

# SEE ALSO

*snapper*(8), *btrfs*(8), *polkit*(8), *dbus-daemon*(1),
*systemctl*(1), *systemd-tmpfiles*(8), *cp*(1)

# AUTHORS

Presire <https://github.com/presire>

This manual page was written for the qSnapper project.

# COPYRIGHT

Copyright © 2025-2026 Presire. Released under the GNU General Public License,
version 2 or later. See _/usr/share/doc/qsnapper/LICENSE.md_ or
<https://www.gnu.org/licenses/gpl-2.0.html> for the full license text.
