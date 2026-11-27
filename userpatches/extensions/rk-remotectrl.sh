# @description Armbian extension. Installs Rockchip's pwm-remotectrl DKMS driver. Downloads/adds/builds/installs the module in the chroot against the target kernel. Forces `INSTALL_HEADERS=yes`; needed on the rockchip edge kernel.

# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 ifroncy01
#
# Rockchip pwm-remotectrl DKMS driver for rockchip64 edge kernel (7.3 and up)
#
# This extension replaces pwm-rockchip fro rk35xx (6.1) 
#
# Only enabled for the edge branch on armbian boards.

function extension_finish_config__install_rk-remotectrl_dkms() {
	if [[ "${KERNEL_HAS_WORKING_HEADERS}" != "yes" ]]; then
		display_alert "Kernel version has no working headers package" "skipping rk-remotectrl dkms for kernel v${KERNEL_MAJOR_MINOR}" "warn"
		return 0
	fi
	declare -g INSTALL_HEADERS="yes"
	display_alert "Forcing INSTALL_HEADERS=yes; for use with rk-remotectrl dkms" "${EXTENSION}" "debug"

	# Pre-download rk-remotectrl DKMS source on the host side (which has direct GitHub access).
	# The Docker container may not have direct GitHub access (e.g. when using ghproxy).
	declare down_dir="${SRC}/cache/rk-remotectrl-dkms"
	mkdir -p "${down_dir}"
	
	declare down_url="https://github.com/joilg/rk-remotectrl/archive/refs/heads/main.zip"
	if [[ ! -f "${down_dir}/source.zip" ]]; then
		display_alert "Downloading rk-remotectrl DKMS source" "${down_url}" "info"
		run_host_command_logged mkdir -p "${down_dir}"
		run_host_command_logged curl -fsSL --progress-bar "${down_url}" -o "${down_dir}/source.zip"
	fi
}

function post_install_kernel_debs__install_rk-remotectrl_dkms_package() {
	if linux-version compare "${KERNEL_MAJOR_MINOR}" le 6.17; then
		display_alert "Kernel version is too old" "skipping rk-remotectrl dkms for kernel v${KERNEL_MAJOR_MINOR}" "warn"
		return 0
	fi

	[[ "${INSTALL_HEADERS}" != "yes" ]] || [[ "${KERNEL_HAS_WORKING_HEADERS}" != "yes" ]] && return 0

	display_alert "Installing Rockchip rk-remotectrl DKMS driver for pwm-remotectrl" "${EXTENSION}" "info"

	# Install DKMS in the chroot
	use_clean_environment="yes" chroot_sdcard_apt_get_install "dkms"

	# Copy pre-downloaded rk-remotectrl source into the chroot
	declare down_dir="${SRC}/cache/rk-remotectrl-dkms"
	cp "${down_dir}/source.zip" "${SDCARD}/tmp/source.zip"

	# Extract source into /usr/src/rk-remotectrl-7.3
	use_clean_environment="yes" chroot_sdcard "mkdir -p /usr/src/rk-remotectrl-7.3"
	use_clean_environment="yes" chroot_sdcard "unzip /tmp/source.zip -d /tmp/"
	use_clean_environment="yes" chroot_sdcard "cp -a /tmp/rk-remotectrl-main/. /usr/src/rk-remotectrl-7.3/"
	use_clean_environment="yes" chroot_sdcard "find /tmp/rk-remotectrl-main/"
	use_clean_environment="yes" chroot_sdcard "find /usr/src/rk-remotectrl-7.3/"
	use_clean_environment="yes" chroot_sdcard "rm -rf /tmp/rk-remotectrl-main /tmp/source.zip"

	# Build and install the kernel module via DKMS
	declare -ag if_error_find_files_sdcard=("/var/lib/dkms/rk-remotectrl*/*/build/*.log")
	display_alert "Building rk-remotectrl kernel module via DKMS" "${EXTENSION}" "info"

	# Build the full target kernel version string
	# IMAGE_INSTALLED_KERNEL_VERSION provides the base version (e.g., "7.3.1")
	# We need to append BRANCH and LINUXFAMILY for the full version (e.g., "7.3.1-vendor-rk3528")
	declare target_kver="${IMAGE_INSTALLED_KERNEL_VERSION}-${BRANCH}-${LINUXFAMILY}"
	if [[ -z "${target_kver}" ]]; then
		display_alert "Cannot determine target kernel version" "rk-remotectrl DKMS build skipped" "warn"
		return 0
	fi
	display_alert "Target kernel version for DKMS" "${target_kver}" "debug"

	use_clean_environment="yes" chroot_sdcard "dkms add -m rk-remotectrl -v 7.3"
	use_clean_environment="yes" chroot_sdcard "dkms build -m rk-remotectrl -v 7.3 -k ${target_kver}"
	use_clean_environment="yes" chroot_sdcard "dkms install -m rk-remotectrl -v 7.3 -k ${target_kver}"

	display_alert "rk-remotectrl DKMS driver installed" "${EXTENSION}" "info"
}
