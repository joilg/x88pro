function extension_finish_config__build_openvfd_dkms_kernel_module() {
	# Deny on minimal CLI images
	if [[ "${KERNEL_HAS_WORKING_HEADERS}" != "yes" ]]; then
		display_alert "Kernel version has no working headers package" "skipping openvfd-dkms for kernel v${KERNEL_MAJOR_MINOR}" "warn"
		return 0
	fi
	declare -g INSTALL_HEADERS="yes"
	display_alert "Forcing INSTALL_HEADERS=yes; for use with openvfd-dkms" "${EXTENSION}" "debug"
}

function post_install_kernel_debs__build_openvfd_dkms_kernel_module() {
	display_alert "install_kernel_debs__build_openvfd_dkms_kernel_module " "${EXTENSION}" "debug"
	if linux-version compare "${KERNEL_MAJOR_MINOR}" ge 6.12; then
		display_alert "Kernel version is too recent" "skipping openvfd-dkms for kernel v${KERNEL_MAJOR_MINOR}" "warn"
		return 0
	fi
	[[ "${INSTALL_HEADERS}" != "yes" ]] || [[ "${KERNEL_HAS_WORKING_HEADERS}" != "yes" ]] && return 0
	display_alert "Install openvfd-dkms packages, will build kernel module in chroot" "${EXTENSION}" "info"
	declare -g if_error_detail_message="openvfd-dkms build failed, extension 'openvfd-dkms'"
	declare -ag if_error_find_files_sdcard=("/var/lib/dkms/openvfd*/*/build/*.log")
	use_clean_environment="yes" chroot_sdcard_apt_get_install "openvfd-dkms openvfd-utils v4l-utils"
}
