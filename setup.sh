#!/bin/sh
set -eu

GKI_ROOT=$(pwd)

display_usage() {
    echo "Usage: $0 [--cleanup | <commit-or-tag>]"
    echo "  --cleanup:              Cleans up previous modifications made by the script."
    echo "  -h, --help:             Displays this usage information."
    echo "  (no args):              Sets up the lunarkernel_sched_extention environment to the latest commit."
}

initialize_variables() {
    if test -d "$GKI_ROOT/drivers/staging"; then
         DRIVER_STAGING_DIR="$GKI_ROOT/drivers/staging"
    elif test -d "$GKI_ROOT/common/drivers/staging"; then
         DRIVER_STAGING_DIR="$GKI_ROOT/common/drivers/staging"
    else
         echo '[ERROR] "drivers/staging/" directory not found.'
         exit 127
    fi

    DRIVER_STAGING_MAKEFILE=$DRIVER_STAGING_DIR/Makefile
    DRIVER_STAGING_KCONFIG=$DRIVER_STAGING_DIR/Kconfig
}

# Reverts modifications made by this script
perform_cleanup() {
    echo "[+] Cleaning up..."
    [ -L "$DRIVER_STAGING_DIR/lunarkernel_sched_extention" ] && rm "$DRIVER_STAGING_DIR/lunarkernel_sched_extention" && echo "[-] Symlink removed."
    grep -q "lunarkernel_sched_extention" "$DRIVER_STAGING_MAKEFILE" && sed -i '/lunarkernel_sched_extention/d' "$DRIVER_STAGING_MAKEFILE" && echo "[-] Makefile reverted."
    grep -q "drivers/staging/lunarkernel_sched_extention/Kconfig" "$DRIVER_STAGING_KCONFIG" && sed -i '/drivers/staging\/lunarkernel_sched_extention\/Kconfig/d' "$DRIVER_STAGING_KCONFIG" && echo "[-] Kconfig reverted."
    if [ -d "$GKI_ROOT/lunarkernel_sched_extention" ]; then
        rm -rf "$GKI_ROOT/lunarkernel_sched_extention" && echo "[-] lunarkernel_sched_extention directory deleted."
    fi
}

# Sets up or update lunarkernel_sched_extention environment
setup_LSE() {
    echo "[+] Setting up lunarkernel_sched_extention..."
    test -d "$GKI_ROOT/lunarkernel_sched_extention" || git clone https://github.com/LunarKernel-Dev/lunarkernel_sched_extention && echo "[+] Repository cloned."
    cd "$GKI_ROOT/lunarkernel_sched_extention"
    cd "$DRIVER_STAGING_DIR"
    ln -sf "$(realpath --relative-to="$DRIVER_STAGING_DIR" "$GKI_ROOT/lunarkernel_sched_extention")" "lunarkernel_sched_extention" && echo "[+] Symlink created."

    # Add entries in Makefile and Kconfig if not already existing
    grep -q "lunarkernel_sched_extention" "$DRIVER_STAGING_MAKEFILE" || printf "\nobj-\$(CONFIG_LUNAR_SCHED_EXT) += lunarkernel_sched_extention/\n" >> "$DRIVER_STAGING_MAKEFILE" && echo "[+] Modified Makefile."
    grep -q "source \"drivers/staging/lunarkernel_sched_extention/Kconfig\"" "$DRIVER_STAGING_KCONFIG" || sed -i "/endif/i\source \"drivers/staging/lunarkernel_sched_extention/Kconfig\"" "$DRIVER_STAGING_KCONFIG" && echo "[+] Modified Kconfig."
    echo '[+] Done.'
}

# Process command-line arguments
if [ "$#" -eq 0 ]; then
    initialize_variables
    setup_LSE
elif [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    display_usage
elif [ "$1" = "--cleanup" ]; then
    initialize_variables
    perform_cleanup
else
    initialize_variables
    setup_LSE "$@"
fi