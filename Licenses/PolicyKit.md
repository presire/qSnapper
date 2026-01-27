# PolicyKit (polkit) License Information

**Package:** PolicyKit (polkit)
**Website:** https://www.freedesktop.org/software/polkit/
**Source:** https://gitlab.freedesktop.org/polkit/polkit

**Copyright:**
- (C) 2008-2024 David Zeuthen <davidz@redhat.com>
- (C) Red Hat, Inc.

All rights reserved.

**License:** GNU Lesser General Public License v2.0 or later (LGPL-2.0-or-later)
**SPDX-License-Identifier:** LGPL-2.0-or-later

---

## Description

This application uses PolicyKit (polkit), an application-level toolkit for defining and handling authorizations. PolicyKit is licensed under the GNU Lesser General Public License version 2.0 or later.

### PolicyKit is used in this application to

- Manage privilege escalation for system operations
- Provide fine-grained access control
- Handle user authentication for privileged actions

### Components used

- polkit daemon (system service)
- polkit D-Bus API
- polkit authorization policies (.policy files)

---

## LGPL v2.0 License Summary

The GNU Lesser General Public License (LGPL) allows this software to be freely used with both open source and proprietary applications.

### Key points

- You can use PolicyKit in any application
- You can link with PolicyKit libraries
- You don't need to open-source your application
- You must allow users to upgrade/replace the PolicyKit library

**For complete license text, see:**
https://www.gnu.org/licenses/old-licenses/lgpl-2.0.html

---

## Source Code Availability

PolicyKit source code is available at:
https://gitlab.freedesktop.org/polkit/polkit

---

## Additional Resources

- [PolicyKit documentation](https://www.freedesktop.org/software/polkit/docs/latest/)
- Manual: `man polkit(8)`
