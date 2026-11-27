# @description Armbian extension. Installs Seekwave's ea6x21 WIFI DKMS driver. Downloads/adds/builds/installs the module in the chroot against the target kernel. Forces `INSTALL_HEADERS=yes`; needed on the rockchip edge kernel.

# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026joilg

function extension_finish_config__install_ea6x21_dkms() {
	declare -g INSTALL_HEADERS="yes"
	display_alert "Forcing INSTALL_HEADERS=yes; for use with ea6x21 dkms" "${EXTENSION}" "debug"

	if linux-version compare "${KERNEL_MAJOR_MINOR}" gt 6.2; then
		display_alert "Kernel version not supported" "skipping ea6x21 dkms for kernel v${KERNEL_MAJOR_MINOR}" "warn"
		return 0
	fi
	# Pre-download ea6x21 DKMS source on the host side (which has direct GitHub access).
	# The Docker container may not have direct GitHub access (e.g. when using ghproxy).
	declare down_dir="${SRC}/cache/ea6x21-dkms-${KERNEL_MAJOR_MINOR}"
	mkdir -p "${down_dir}"
	# declare down_url="https://github.com/joilg/ea6x21-dkms/archive/refs/heads/${KERNEL_MAJOR_MINOR}.zip"
	declare down_url="https://github.com/joilg/ea6x21-dkms/archive/refs/tags/${KERNEL_MAJOR_MINOR}.zip"
	if [[ ! -f "${down_dir}/source.zip" ]]; then
		display_alert "Downloading ea6x21 DKMS source" "${down_url}" "info"
		run_host_command_logged mkdir -p "${down_dir}"
		run_host_command_logged curl -fsSL --progress-bar "${down_url}" -o "${down_dir}/source.zip"
	fi
}

function post_install_kernel_debs__install_ea6x21_dkms_package() {
	if linux-version compare "${KERNEL_MAJOR_MINOR}" gt 6.2; then
		display_alert "Kernel version not supported" "skipping ea6x21 dkms for kernel v${KERNEL_MAJOR_MINOR}" "warn"
		return 0
	fi

	[[ "${INSTALL_HEADERS}" != "yes" ]] || [[ "${KERNEL_HAS_WORKING_HEADERS}" != "yes" ]] && return 0

	display_alert "Installing Rockchip ea6x21 DKMS driver for pwm-remotectrl" "${EXTENSION}" "info"

	# Install DKMS in the chroot
	use_clean_environment="yes" chroot_sdcard_apt_get_install "dkms"

	# Copy pre-downloaded ea6x21 source into the chroot
	declare down_dir="${SRC}/cache/ea6x21-dkms-${KERNEL_MAJOR_MINOR}"
	cp "${down_dir}/source.zip" "${SDCARD}/tmp/source.zip"

	# Extract source into /usr/src/ea6x21-1.0
	use_clean_environment="yes" chroot_sdcard "unzip /tmp/source.zip -d /usr/src/"
	use_clean_environment="yes" chroot_sdcard "rm -rf /tmp/ea6x21-main /tmp/source.zip"

	# Build and install the kernel module via DKMS
	declare -ag if_error_find_files_sdcard=("/var/lib/dkms/ea6x21*/*/build/*.log")
	display_alert "Building ea6x21 kernel module via DKMS" "${EXTENSION}" "info"

	# Build the full target kernel version string
	# IMAGE_INSTALLED_KERNEL_VERSION provides the base version (e.g., "6.1.161)
	# We need to append BRANCH and LINUXFAMILY for the full version (e.g., "6.1.161-vendor-rk3528")
	declare target_kver="${IMAGE_INSTALLED_KERNEL_VERSION}-${BRANCH}-${LINUXFAMILY}"
	if [[ -z "${target_kver}" ]]; then
		display_alert "Cannot determine target kernel version" "ea6x21 DKMS build skipped" "warn"
		return 0
	fi
	display_alert "Target kernel version for DKMS" "${target_kver}" "debug"

	use_clean_environment="yes" chroot_sdcard "dkms add -m ea6x21-dkms -v ${KERNEL_MAJOR_MINOR}"
	use_clean_environment="yes" chroot_sdcard "dkms build -m ea6x21-dkms -v ${KERNEL_MAJOR_MINOR} -k ${target_kver}"
	use_clean_environment="yes" chroot_sdcard "dkms install -m ea6x21-dkms -v ${KERNEL_MAJOR_MINOR} -k ${target_kver}"

	display_alert "ea6x21 DKMS driver installed" "${EXTENSION}" "info"
}
