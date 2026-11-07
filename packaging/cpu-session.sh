#!/bin/bash
# Select a supported CPU governor for the NFS5 port; never change frequency
# limits. Adapted (rename only) from the MIT ReConflict / The ReThing scripts.
nfs5_cpu_root=/sys/devices/system/cpu/cpufreq
nfs5_cpu_policies=()
nfs5_cpu_governors=()

nfs5_cpu_begin() {
    [ "${NFS5_CPU_PERFORMANCE:-1}" = 1 ] || return 0
    local policy previous current
    for policy in "$nfs5_cpu_root"/policy*; do
        [ -r "$policy/scaling_available_governors" ] && [ -w "$policy/scaling_governor" ] || continue
        grep -qw performance "$policy/scaling_available_governors" || continue
        previous=$(cat "$policy/scaling_governor") || continue
        [[ "$previous" =~ ^[a-zA-Z0-9_]+$ ]] || continue
        [ "$previous" != performance ] || continue
        if printf 'performance\n' > "$policy/scaling_governor"; then
            current=$(cat "$policy/scaling_governor")
            if [ "$current" = performance ]; then
                nfs5_cpu_policies+=("$policy")
                nfs5_cpu_governors+=("$previous")
                echo "[cpu-session] ${policy##*/}: $previous -> performance; frequency limits unchanged"
            else
                printf '%s\n' "$previous" > "$policy/scaling_governor" || true
            fi
        fi
    done
    return 0
}

nfs5_cpu_restore() {
    local i policy current
    for ((i=${#nfs5_cpu_policies[@]}-1;i>=0;i--)); do
        policy=${nfs5_cpu_policies[i]}
        current=$(cat "$policy/scaling_governor" 2>/dev/null) || continue
        # Respect a later explicit setting made by firmware or the user.
        [ "$current" = performance ] || continue
        if printf '%s\n' "${nfs5_cpu_governors[i]}" > "$policy/scaling_governor"; then
            echo "[cpu-session] ${policy##*/}: restored ${nfs5_cpu_governors[i]}"
        fi
    done
    nfs5_cpu_policies=()
    nfs5_cpu_governors=()
    return 0
}
