function extension_finish_config__install_kernel_headers_for_open-vfd_dkms() {
	if [[ "${KERNEL_HAS_WORKING_HEADERS}" != "yes" ]]; then
		display_alert "Kernel version has no working headers package" "skipping open-vfd dkms for kernel v${KERNEL_MAJOR_MINOR}" "warn"
		return 0
	fi
	declare -g INSTALL_HEADERS="yes"
	display_alert "Forcing INSTALL_HEADERS=yes; for use with open-vfd dkms" "${EXTENSION}" "debug"
}

function post_install_kernel_debs__install_open-vfd_dkms_package() {
	[[ "${INSTALL_HEADERS}" != "yes" ]] || [[ "${KERNEL_HAS_WORKING_HEADERS}" != "yes" ]] && return 0
	api_url="https://api.github.com/repos/wxzmz/linux_openvfd/releases/latest"
	latest_version=$(curl -s "${api_url}" | jq -r '.tag_name')
	openvfd_url="https://github.com/wxzmz/linux_openvfd/releases/download/${latest_version}/open-vfd-dkms_${latest_version}_arm64.deb"
	if [[ "${GITHUB_MIRROR}" == "ghproxy" ]];then
		ghproxy_header="https://mirror.ghproxy.com/"
		openvfd_url=${ghproxy_header}${openvfd_url}
	fi
	openvfd_dkms_deb_file_name="open-vfd-dkms_${latest_version}_arm64.deb"
	use_clean_environment="yes" chroot_sdcard "wget ${openvfd_url} -P /tmp"

	display_alert "Install open-vfd packages, will build kernel module in chroot" "${EXTENSION}" "info"
	declare -ag if_error_find_files_sdcard=("/var/lib/dkms/open-vfd*/*/build/*.log")
	use_clean_environment="yes" chroot_sdcard_apt_get_install "/tmp/${openvfd_dkms_deb_file_name}"
	use_clean_environment="yes" chroot_sdcard "rm -f /tmp/open-vfd*.deb"
}
