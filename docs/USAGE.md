# qSnapper - Usage

[Back to README](../README.md) | [日本語](USAGE_JP.md)

## Usage

### Launching qSnapper

You can launch qSnapper from:  
- Application menu (System Tools category)
- Command line: `qsnapper`

### Creating Snapshots

1. Click the "Create Snapshot" button
2. Enter a description (required)
3. Select the snapshot type
   - **Single** creates one standalone snapshot
   - **Pre** creates a pre snapshot that can be paired with a post snapshot later
   - **Post** is available only when a pre snapshot without a post snapshot is selected; the new snapshot is paired with it
4. Choose a cleanup algorithm (None, number, or timeline)
5. Optionally mark the snapshot as important;
   important snapshots are shown with a `!` in the list and use the separate important number limit
6. Optionally enter user data as `key=value` lines or comma-separated pairs
7. Click "Create"

### Viewing Snapshots

The Snapshots tab displays a list of all snapshots with:  
- Snapshot number
- Type (Single, Pre, Post)
- Important marker (`!`)
- Start and end date and time
- Description
- User data

Pre/Post pairs are grouped into one row, and selecting a pair selects both snapshots.  

### Deleting Snapshots

Select one or more snapshots with the checkboxes and click "Delete Selected".  
The whole selection is sent as a single request and requires a single authentication.  
When a single snapshot is deleted in an active local session, polkit keeps the authorization for a short time, so deleting another single snapshot shortly afterwards does not ask again.  
If some snapshots cannot be deleted, the remaining ones are deleted and the failures are reported with their count.  

### Comparing Snapshots

Select a snapshot to view:  
- Files that were added, modified, or deleted
- Detailed file differences

### Restoring from Snapshots

1. Select a snapshot
2. Click "Show Changes" to open the Snapshot Overview
3. Review the file changes in the tree view and diff panel
4. Check the files/directories you want to restore
5. Click "Restore Selected" to restore the checked items

For Pre/Post snapshot pairs, the Snapshot Overview offers four views:  
- **Revert to Pre #N**: undo the changes made between Pre #N and Post #M (restorable; this is the default view)  
- **Re-apply to Post #M**: redo the changes made between Pre #N and Post #M (restorable)  
- **Show differences between snapshot #N (Pre) and the current system**: view only  
- **Show differences between snapshot #M (Post) and the current system**: view only  

Checkboxes and the restore button are available only in the two restorable views. The two "vs current system" views are read-only and display the hint "View only. Choose a Pre / Post difference above to restore."  

The two restorable views list only the entries that still differ from the current system. Entries whose current state already matches the restore target — for example files you have already restored, or files the package manager removed again after the Post snapshot — are hidden, because restoring them would have no effect. When every entry has been filtered out, the tree shows "No differences with snapshot". This filtering never widens the restore scope: the list always remains a subset of the Pre/Post difference, and files changed outside that range are never added to it.  

After a successful restore, the view automatically switches to the differences between the restore target snapshot and the current system, so the result is visible immediately. Files changed outside the Pre/Post range (such as files you created manually after the Post snapshot) still appear there, which confirms they were left untouched.  

For Pre/Post pairs the restore button is labelled "Restore Selected to #<number>". Restoring only touches files that actually differ between the Pre and Post snapshots; files changed outside that range (for example, files you created manually after the Post snapshot) are left untouched. This matches the upstream `snapper undochange <pre>..<post>` behaviour.  

#### Restore Modes

qSnapper offers two restore methods, selectable from the restore confirmation dialog:  

- **Direct Copy (fast)** (default):  
  Copies files directly from the mounted snapshot using `cp -a --reflink=auto`.  
  On Btrfs, reflink enables near-instant copy-on-write regardless of file size.  

- **YaST compatible**:  
  Uses the same restore approach as YaST's "Filesystem Snapshots" module (`cp -a` without reflink).  
  Use this mode if you experience compatibility issues with the Direct Copy method.  

#### Restore Options

- **Batch size** (1–1000, default: 100):  
  Number of files submitted per staging chunk before the restore plan is frozen. Larger values may improve throughput; smaller values provide more granular progress feedback. A multi-file restore requires exactly one PolicyKit authorization at commit time, regardless of how many files or chunks are selected, with no re-prompt while the frozen plan executes.  

During restoration, a real-time progress log displays each file as it is restored, with automatic scrolling.  

**Warning**:  
Restoring snapshots may overwrite current data. Always review changes before confirming.  

**Note**:  
If the changes between the Pre and Post snapshots created a whole new directory (for example, a package installation that added `/etc/foo/`), reverting to Pre removes that directory recursively, including any files you added inside it afterwards (for example, `/etc/foo/mine.conf`). This is the same behaviour as `snapper undochange` and is intentional.  

### Settings Tab

The Settings tab edits the selected Snapper configuration:  
- Configuration information (subvolume, filesystem type, qgroup) is shown read-only
- Number cleanup, timeline cleanup, empty pre/post cleanup, space limits, and background comparison values can be changed
- The timeline creation setting only takes effect while `snapper-timeline.timer` is enabled
- The snapper systemd timers can be enabled or disabled; the timers are shared by all configurations
- The theme can be set to Light, Dark, or follow the system

"New..." creates a configuration for a mounted Btrfs subvolume. The `.snapshots` subvolume is created automatically, and the filesystem root cannot be used.  
"Delete..." removes a configuration after typing its name to confirm; all snapshots in that configuration are deleted. The root configuration cannot be created or deleted here.  

### Filesystem

The Filesystem tab shows read-only Btrfs information for the selected configuration:  
- Overall size, allocated space, unallocated space, node size, and sector size
- Space by chunk type, including profile, total allocated, and used space
- Per-device size and allocated space
- A subvolume list loaded on demand, with an option to hide Snapper snapshots

The values come from the kernel Btrfs ioctls and can differ from the output of `btrfs filesystem usage`.  
Loading the subvolume list requires authentication and is read-only. Long lists are capped and marked as truncated.  

## Configuration

### Snapper Configuration

qSnapper uses existing Snapper configurations and can also create and delete them from the Settings tab.  
A configuration for the filesystem root cannot be created from the Settings tab, so create it with the command below before using qSnapper on the root filesystem.  

```bash
sudo snapper -c root create-config /
```

For other filesystems:  

```bash
sudo snapper -c <config-name> create-config <mount-point>
```

### Application Settings

Application settings are stored in:  
- `~/.config/Presire/qSnapper.conf`

Settings include:  
- Theme mode (Light, Dark, or system)
- Window geometry and state
- Restore method (Direct Copy or YaST compatible)
- Restore batch size (1–1000)
- User preferences

### Theme Configuration

qSnapper supports theme switching through the built-in ThemeManager:  

1. **Light Mode**:  
   Optimized light color scheme with Material Design colors  
2. **Dark Mode**:  
   Comfortable dark color scheme for low-light environments  
3. **System**:  
   Follows the desktop's light or dark preference  

The theme setting is persisted per user in the application configuration and is applied when you press Save in the Settings tab.  
