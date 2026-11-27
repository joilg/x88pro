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
	chroot_sdcard_apt_get_install dkms build-essential
	display_alert "Install openvfd-dkms packages, will build kernel module in chroot" "${EXTENSION}" "info"
    use_clean_environment="yes" chroot_sdcard dkms add -m openvfd -v 8fadd10
    use_clean_environment="yes" chroot_sdcard dkms build -m openvfd -v 8fadd10 -k "${kernel_version}"
    use_clean_environment="yes" chroot_sdcard dkms install -m openvfd -v 8fadd10 -k "${kernel_version}"
    use_clean_environment="yes" chroot_sdcard depmod -a "${kernel_version}"
    display_alert "openvfd modules installed" "${kernel_version}" "info"

}
