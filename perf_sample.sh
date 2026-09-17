#!/bin/bash
# Performance telemetry helper for the board. It is intentionally passive: it
# does not start/stop the application, change governors, or alter NPU settings.
# Output may include operational details, so perf_logs/ is not versioned.
set -eu
duration="${1:-300}"
case "$duration" in ''|*[!0-9]*) echo '用法: bash perf_sample.sh 秒数'; exit 1;; esac
if [ "$duration" -lt 1 ] || [ "$duration" -gt 3600 ]; then
    echo '采样时长应为1至3600秒'; exit 1
fi
root="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$root/perf_logs"
out="$(mktemp -d "$root/perf_logs/sample_$(date +%Y%m%d_%H%M%S)_XXXXXX")"
NPU_DEVFREQ="/sys/devices/platform/fdab0000.npu/devfreq/fdab0000.npu"
printf '输出目录: %s\n' "$out"
{
    date --iso-8601=seconds
    uname -a
    for zone in /sys/class/thermal/thermal_zone*; do
        [ -d "$zone" ] || continue
        printf '%s type=' "$zone"
        cat "$zone/type" 2>/dev/null || true
    done
    for policy in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -d "$policy" ] || continue
        printf '\n%s\n' "$policy"
        for key in related_cpus scaling_governor scaling_min_freq scaling_max_freq; do
            printf '%s=' "$key"
            cat "$policy/$key" 2>/dev/null || true
        done
    done
    if [ -d "$NPU_DEVFREQ" ]; then
        printf '\n%s\n' "$NPU_DEVFREQ"
        for key in governor cur_freq min_freq max_freq available_frequencies load; do
            printf '%s=' "$key"
            cat "$NPU_DEVFREQ/$key" 2>/dev/null || true
        done
    fi
} > "$out/environment.txt"
printf 'elapsed_s,unix_time,metric,source,value\n' > "$out/system.csv"
start=$SECONDS
while [ "$((SECONDS-start))" -lt "$duration" ]; do
    elapsed=$((SECONDS-start))
    stamp=$(date +%s)
    for zone in /sys/class/thermal/thermal_zone*; do
        [ -r "$zone/temp" ] || continue
        value=$(cat "$zone/temp" 2>/dev/null) || continue
        printf '%s,%s,temperature_millicelsius,%s,%s\n' "$elapsed" "$stamp" "${zone##*/}" "$value" >> "$out/system.csv"
    done
    for policy in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -r "$policy/scaling_cur_freq" ] || continue
        value=$(cat "$policy/scaling_cur_freq" 2>/dev/null) || continue
        printf '%s,%s,cpu_frequency_khz,%s,%s\n' "$elapsed" "$stamp" "${policy##*/}" "$value" >> "$out/system.csv"
    done
    # rknpu_ondemand 的 load 格式为 "百分比@频率Hz"，例如 100@1000000000Hz。
    if [ -r "$NPU_DEVFREQ/load" ]; then
        npu_raw=$(cat "$NPU_DEVFREQ/load" 2>/dev/null || true)
        npu_load=${npu_raw%@*}
        npu_frequency=${npu_raw#*@}
        npu_frequency=${npu_frequency%Hz}
        case "$npu_load" in ''|*[!0-9]*) ;; *)
            printf '%s,%s,npu_load_percent,rknpu,%s\n' "$elapsed" "$stamp" "$npu_load" >> "$out/system.csv";; esac
        case "$npu_frequency" in ''|*[!0-9]*) ;; *)
            printf '%s,%s,npu_frequency_hz,rknpu,%s\n' "$elapsed" "$stamp" "$npu_frequency" >> "$out/system.csv";; esac
    fi
    awk -v e="$elapsed" -v t="$stamp" '/MemAvailable:/ {printf "%s,%s,mem_available_kb,system,%s\n",e,t,$2}' /proc/meminfo >> "$out/system.csv"
    # 原始CPU累计计数供后续算区间占用率；保留应用每个线程的统计。
    {
        printf '\nTIME %s ELAPSED %s\n' "$stamp" "$elapsed"
        cat /proc/stat
        ps -C face_attendance -L -o pid,tid,pcpu,rss,stat,comm || true
    } >> "$out/cpu_raw.log"
    sleep 1
done
echo "采样完成: $out"
