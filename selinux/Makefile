########################################
# qSnapper SELinux Policy Module Makefile
#
# This Makefile is for standalone builds without CMake.
# For normal builds, use CMake (see README.md).
#
# Usage:
#   make              # Build policy package (.pp)
#   make install      # Install policy and relabel files
#   make load         # Alias for install
#   make unload       # Remove policy from system
#   make clean        # Clean build artifacts
########################################

.PHONY: all install load unload clean check

########################################
# Configuration
########################################

POLICY_NAME = qsnapper
POLICY_VERSION = 1.0.0

# SELinux tools (standard paths)
CHECKMODULE = /usr/bin/checkmodule
SEMODULE_PACKAGE = /usr/bin/semodule_package
SEMODULE = /usr/sbin/semodule
RESTORECON = /sbin/restorecon

# Check if tools are available
ifeq ($(wildcard $(CHECKMODULE)),)
$(error checkmodule not found. Install selinux-policy-devel package)
endif

ifeq ($(wildcard $(SEMODULE_PACKAGE)),)
$(error semodule_package not found. Install selinux-policy-devel package)
endif

########################################
# Build targets
########################################

TARGETS = $(POLICY_NAME).pp

all: $(TARGETS)

# Step 1: Compile .te to .mod
$(POLICY_NAME).mod: $(POLICY_NAME).te
	@echo "Compiling Type Enforcement policy: $< -> $@"
	$(CHECKMODULE) -M -m -o $@ $<

# Step 2: Package .mod + .fc + .if to .pp
$(POLICY_NAME).pp: $(POLICY_NAME).mod $(POLICY_NAME).fc $(POLICY_NAME).if
	@echo "Packaging SELinux policy module: $@"
	$(SEMODULE_PACKAGE) -o $@ -m $(POLICY_NAME).mod -f $(POLICY_NAME).fc

########################################
# Installation and management
########################################

install: all
	@echo "Installing SELinux policy module: $(POLICY_NAME)"
	@if [ ! -x "$(SEMODULE)" ]; then \
		echo "ERROR: semodule not found at $(SEMODULE)"; \
		echo "Cannot install policy module without semodule"; \
		exit 1; \
	fi
	$(SEMODULE) -i $(POLICY_NAME).pp
	@echo "Policy module installed successfully"
	@echo ""
	@echo "Relabeling qSnapper binaries..."
	@if [ -x "$(RESTORECON)" ]; then \
		$(RESTORECON) -R -v /usr/bin/qsnapper 2>/dev/null || true; \
		$(RESTORECON) -R -v /usr/libexec/qsnapper-dbus-service 2>/dev/null || true; \
		$(RESTORECON) -R -v /etc/snapper 2>/dev/null || true; \
		$(RESTORECON) -R -v /.snapshots 2>/dev/null || true; \
		echo "File contexts updated"; \
	else \
		echo "WARNING: restorecon not found, skipping file relabeling"; \
		echo "Manually run: restorecon -R -v /usr/bin/qsnapper /usr/libexec/qsnapper-dbus-service"; \
	fi
	@echo ""
	@echo "Installation complete!"
	@echo "Verify: semodule -l | grep $(POLICY_NAME)"

load: install

unload:
	@echo "Removing SELinux policy module: $(POLICY_NAME)"
	@if [ ! -x "$(SEMODULE)" ]; then \
		echo "ERROR: semodule not found at $(SEMODULE)"; \
		exit 1; \
	fi
	$(SEMODULE) -r $(POLICY_NAME) || true
	@echo "Policy module removed (if it was installed)"

########################################
# Utility targets
########################################

check: $(POLICY_NAME).te
	@echo "Checking policy syntax..."
	$(CHECKMODULE) -M -m -o $(POLICY_NAME).mod.tmp $(POLICY_NAME).te
	@rm -f $(POLICY_NAME).mod.tmp
	@echo "Syntax check passed"

clean:
	@echo "Cleaning build artifacts..."
	rm -f $(POLICY_NAME).pp $(POLICY_NAME).mod
	rm -f *.tmp
	@echo "Clean complete"

########################################
# Help
########################################

help:
	@echo "qSnapper SELinux Policy Module Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build policy package ($(POLICY_NAME).pp)"
	@echo "  make install      - Install policy and relabel files (requires root)"
	@echo "  make load         - Alias for 'make install'"
	@echo "  make unload       - Remove policy from system (requires root)"
	@echo "  make check        - Check policy syntax without building"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Requirements:"
	@echo "  - selinux-policy-devel package (provides checkmodule, semodule_package)"
	@echo "  - root privileges for install/unload operations"
	@echo ""
	@echo "Example workflow:"
	@echo "  make              # Build as regular user"
	@echo "  sudo make install # Install as root"
