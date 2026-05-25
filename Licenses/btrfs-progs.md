# btrfs-progs / libbtrfsutil License Information

**Package:** btrfs-progs (libbtrfsutil1)
**Source:** https://github.com/kdave/btrfs-progs

**Copyright:** (C) Facebook, Oracle, SUSE, and btrfs-progs contributors
All rights reserved.

**License:**
- btrfs-progs (overall): GNU General Public License v2.0 (GPL-2.0)
- libbtrfsutil: GNU Lesser General Public License v2.1 (LGPL-2.1)

**SPDX-License-Identifier:**
- btrfs-progs: GPL-2.0-only
- libbtrfsutil: LGPL-2.1-or-later

---

## Description

This application uses **libbtrfsutil**, a library for interacting with Btrfs
filesystems (subvolumes, snapshots, qgroups, etc.), provided by the
`libbtrfsutil1` package from the btrfs-progs project.

### This application uses libbtrfsutil for

- Btrfs subvolume and snapshot inspection
- Filesystem metadata retrieval
- Support for Snapper-managed Btrfs snapshots

---

## License Summary

### libbtrfsutil — LGPL v2.1

The GNU Lesser General Public License (LGPL) permits use of the library by
applications under non-GPL licenses, provided that:

- The LGPL library itself remains under the LGPL
- Users can relink the application with a modified version of the library
  (typically satisfied by dynamic linking)
- Proper attribution and license notice are preserved

**For complete LGPL v2.1 license text, see:**
https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html

### btrfs-progs — GPL v2

The command-line tools of btrfs-progs are distributed under GPL v2.

**For complete GPL v2 license text, see:**
https://www.gnu.org/licenses/old-licenses/gpl-2.0.html

---

## Source Code Availability

btrfs-progs source code (including libbtrfsutil) is available at:
https://github.com/kdave/btrfs-progs

License files in the upstream repository:
- `COPYING` — GPL v2 (btrfs-progs)
- `libbtrfsutil/COPYING` — LGPL v2.1 (libbtrfsutil)
