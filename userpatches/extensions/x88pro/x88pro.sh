# SPDX-License-Identifier: MIT
# H96 Max RK3528 kernel configuration fragment.

function pre_customize_image__h96max_display() {
    install -D -m 0755 "${EXTENSION_DIR}/files/h96max-display.sh" "${SDCARD}/usr/local/sbin/h96max-display"
    install -D -m 0644 "${EXTENSION_DIR}/files/h96max-display.service" "${SDCARD}/etc/systemd/system/h96max-display.service"
    install -D -m 0644 "${SRC}/config/bootscripts/boot-rockchip64-ttyS0.cmd" "${SDCARD}/boot/h96max-main.cmd"
    mkimage -C none -A arm -T script -d "${EXTENSION_DIR}/files/h96max-early-display.cmd" "${SDCARD}/boot/h96max-early.scr"
    mkimage -C none -A arm -T script -d "${SDCARD}/boot/h96max-main.cmd" "${SDCARD}/boot/h96max-main.scr"
    chroot_sdcard systemctl enable h96max-display.service
}

function pre_customize_image__h96max_audio() {
    install -D -m 0644 "${EXTENSION_DIR}/files/60-h96max-spdif-mirror.conf" \
        "${SDCARD}/etc/pipewire/pipewire.conf.d/60-h96max-spdif-mirror.conf"
}

function custom_kernel_config__h96max() {
    local fragment="${EXTENSION_DIR}/kernel.config"
    local fragment_hash

    [[ -f "$fragment" ]] || exit_with_error "Missing H96 kernel config: ${fragment}"
    [[ -f .config ]] || return 0

    fragment_hash="$(sha256sum "$fragment" | awk '{print $1}')"
    kernel_config_modifying_hashes+=("${fragment_hash}")

    scripts/kconfig/merge_config.sh -m .config "$fragment"
    run_kernel_make olddefconfig
}
