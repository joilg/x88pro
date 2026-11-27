# SPDX-License-Identifier: GPL-2.0
# SeekWave EA6521QF SDIO Wi-Fi and Bluetooth DKMS integration for H96 Max.


function extension_finish_config__ea6x21q_kernel_headers() {
    declare -g INSTALL_HEADERS="yes"
    display_alert "EA6521QF DKMS" "Kernel headers enabled for external module build" "info"
}

function post_install_kernel_debs__install_ea6x21q_dkms() {
    local EA6X21Q_EXT="${EXTENSION_DIR}"
    local archive="${EA6X21Q_EXT}/source/ea6621q-1.0.tar.gz"
    (
        cd "${EA6X21Q_EXT}" &&
        sha256sum -c SOURCE-SHA256SUM
    ) || exit_with_error "EA6521QF DKMS source archive checksum validation failed"
    local kernel_version
    local source_dir="${SDCARD}/usr/src/ea6621q-1.0"
    local firmware_dir="${SDCARD}/lib/firmware"
    local -a kernels=()

    [[ -s "${archive}" ]] || exit_with_error "Missing EA6521QF DKMS source archive: ${archive}"
    (cd "${EA6X21Q_EXT}/firmware" && sha256sum -c ../FIRMWARE-SHA256SUMS) || \
        exit_with_error "EA6521QF firmware checksum validation failed"

    mapfile -t kernels < <(
        find "${SDCARD}/lib/modules" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort
    )
    [[ "${#kernels[@]}" -eq 1 ]] || \
        exit_with_error "Expected one installed kernel for EA6521QF DKMS; found: ${kernels[*]:-none}"
    kernel_version="${kernels[0]}"

    install -d "${source_dir}" "${firmware_dir}"
    tar -xzf "${archive}" -C "${source_dir}"

    for fw in EA6521QF_SEEKWAVE_R00005.bin RAM_RW_KERNEL_DRAM.bin ROM_EXEC_KERNEL_IRAM.bin \
              sv6160.nvbin sv6160lite.nvbin sv6316.nvbin; do
        install -m 0644 "${EA6X21Q_EXT}/firmware/${fw}" "${firmware_dir}/${fw}"
    done

    install -d "${SDCARD}/etc/modules-load.d"
    cat > "${SDCARD}/etc/modules-load.d/ea6x21q.conf" <<'MODULES'
# SeekWave EA6521QF SDIO transport, bootstrap, Wi-Fi and Bluetooth.
skw_sdio
skw_bootcoms
skw
skwbt
MODULES

    declare -ag if_error_find_files_sdcard=("/var/lib/dkms/ea6621q*/*/build/*.log")
    display_alert "Installing EA6521QF DKMS build dependencies" "dkms build-essential (${kernel_version})" "info"
    chroot_sdcard_apt_get_install dkms build-essential

    display_alert "Building EA6521QF Wi-Fi/BT modules" "ea6621q 1.0 / ${kernel_version}" "info"
    use_clean_environment="yes" chroot_sdcard dkms add -m ea6621q -v 1.0
    if ! use_clean_environment="yes" chroot_sdcard dkms build -m ea6621q -v 1.0 -k "${kernel_version}"; then
        local dkms_log="${SDCARD}/var/lib/dkms/ea6621q/1.0/build/make.log"

        echo
        echo "========== BEGIN EA6521QF DKMS MAKE.LOG =========="

        if [[ -s "${dkms_log}" ]]; then
            cat "${dkms_log}"
        else
            find "${SDCARD}/var/lib/dkms/ea6621q" \
                -type f -name make.log -print -exec cat {} \; 2>/dev/null || true
        fi

        echo "========== END EA6521QF DKMS MAKE.LOG =========="
        exit_with_error "EA6521QF DKMS compilation failed; see make.log above"
    fi
    use_clean_environment="yes" chroot_sdcard dkms install -m ea6621q -v 1.0 -k "${kernel_version}"
    use_clean_environment="yes" chroot_sdcard depmod -a "${kernel_version}"

    for module in skw_sdio skw_bootcoms skw skwbt; do
        [[ -n "$(chroot_sdcard_with_stdout modinfo -k "${kernel_version}" -F filename "${module}" 2>/dev/null)" ]] || \
            exit_with_error "EA6521QF module is not installed: ${module}"
    done
    display_alert "EA6521QF Wi-Fi and Bluetooth modules installed" "${kernel_version}" "info"
}
