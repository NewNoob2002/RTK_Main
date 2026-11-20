#!/usr/bin/env bash
# 本地一键门禁（docs/acceptance.md §1 G1–G7b 的可自动部分），合并到 main 前由 Architect 运行。
# 用法：tools/ci/run_gates.sh [--base <rev>] [--prefix <构建目录前缀>] [--skip-idf]
#   --base    doc_check 的基线提交（缺省 main 与 HEAD 的 merge-base）
#   --prefix  构建目录名前缀，全部放 builds/<prefix>-*（缺省 ci）
#   --skip-idf 跳过 IDF 目标构建与依赖它的 G5b/G7b（仅主机快速检查）
#   --only-idf 只跑 IDF 目标构建与 G7b（GitHub Actions 的 ESP-IDF 容器作业用）
#   --skip-tidy 跳过 G5b（clang-tidy 结果依赖本机 clang 版本，CI 容器不跑）
# ESP-IDF 环境：优先 $IDF_PATH/export.sh，否则 ~/esp/esp-idf/export.sh。
# 失败不中断：逐项记录 PASS/FAIL，最后汇总，有 FAIL 时退出码 1。日志在 builds/<prefix>-logs/。
set -u -o pipefail

cd "$(dirname "$0")/../.."
ROOT=$PWD
BASE=""
PREFIX=ci
SKIP_IDF=0
ONLY_IDF=0
SKIP_TIDY=0
while [ $# -gt 0 ]; do
    case "$1" in
    --base) BASE=$2; shift 2 ;;
    --prefix) PREFIX=$2; shift 2 ;;
    --skip-idf) SKIP_IDF=1; shift ;;
    --only-idf) ONLY_IDF=1; shift ;;
    --skip-tidy) SKIP_TIDY=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -n "$BASE" ] || BASE=$(git merge-base main HEAD)

LOGS=builds/$PREFIX-logs
mkdir -p "$LOGS"
RESULTS=()

# 运行一项门禁：名称 + 命令；输出写日志，结果记入汇总。
gate() {
    local name=$1
    shift
    local log="$LOGS/$name.log"
    printf '%-28s ' "$name"
    if "$@" >"$log" 2>&1; then
        echo PASS
        RESULTS+=("PASS $name")
    else
        echo "FAIL (see $log)"
        RESULTS+=("FAIL $name")
    fi
}

ASAN_CXX="-fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer -g"
ASAN_C="-fsanitize=address,undefined"

# 主机测试：普通 + ASan/UBSan 各一份；额外 cmake 参数原样透传。
host_suite() {
    local dir=$1 out=$2
    shift 2
    cmake -S "$dir" -B "builds/$out" -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@" &&
        cmake --build "builds/$out" &&
        ctest --test-dir "builds/$out" --output-on-failure
}
host_suite_asan() {
    local dir=$1 out=$2
    shift 2
    host_suite "$dir" "$out" -DCMAKE_CXX_FLAGS="$ASAN_CXX" -DCMAKE_C_FLAGS="$ASAN_C" \
        -DCMAKE_EXE_LINKER_FLAGS="$ASAN_C" "$@"
}

# 0 warning 判定：构建成功且日志里没有 warning:。
idf_build() {
    local out=$1
    shift
    idf.py -B "builds/$out" "$@" build >"$LOGS/$out.build.log" 2>&1 &&
        ! grep -q "warning:" "$LOGS/$out.build.log"
}

fmt_check() {
    git ls-files --cached --others --exclude-standard -z -- '*.c' '*.cpp' '*.h' '*.hpp' |
        grep -zv -e '^reference/' -e 'H2Vectors.h' |
        while IFS= read -r -d '' file; do
            if [ -f "$file" ]; then
                printf '%s\0' "$file"
            fi
        done |
        xargs -0 clang-format --style=file --dry-run --Werror
}

if [ "$ONLY_IDF" -eq 0 ]; then
gate G1-frozen sha256sum -c --quiet docs/frozen.sha256

HOST_DIRS=(test/host test/app_fw test/services test/stream_demux test/bt_reconnect test/cli
    test/gnss test/nvs_store test/web_proto test/settings tools/main_host_check tools/t13_host_check)
for d in "${HOST_DIRS[@]}"; do
    n=$(basename "$d")
    gate "G2-$n" host_suite "$d" "$PREFIX-$n"
    gate "G2b-$n-asan" host_suite_asan "$d" "$PREFIX-$n-asan"
done
GNSS_ALL=(-DRTK_GNSS_DRV_COMNAV=ON -DRTK_GNSS_DRV_UBLOX=ON)
gate G2-gnss-all host_suite test/gnss "$PREFIX-gnss-all" "${GNSS_ALL[@]}"
gate G2b-gnss-all-asan host_suite_asan test/gnss "$PREFIX-gnss-all-asan" "${GNSS_ALL[@]}"

gate G5-format fmt_check
gate G6-doc_check python3 -I tools/doc_check/doc_check.py --base "$BASE"
fi

if [ "$SKIP_IDF" -eq 0 ]; then
    # shellcheck disable=SC1091
    . "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null 2>&1 ||
        { echo "ESP-IDF export failed" >&2; exit 2; }
    gate G3-idf idf_build "$PREFIX-idf"
    mkdir -p "builds/$PREFIX-trace-off"
    echo "CONFIG_RTK_TRACE_ENABLE=n" >"builds/$PREFIX-trace-off/trace_off.defaults"
    gate G3-idf-trace-off idf_build "$PREFIX-trace-off" \
        -DSDKCONFIG="$ROOT/builds/$PREFIX-trace-off/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;builds/$PREFIX-trace-off/trace_off.defaults"
    mkdir -p "builds/$PREFIX-wifi-off"
    echo "CONFIG_RTK_WIFI_CONFIG_ENABLE=n" >"builds/$PREFIX-wifi-off/wifi_off.defaults"
    gate G3-idf-wifi-off idf_build "$PREFIX-wifi-off" \
        -DSDKCONFIG="$ROOT/builds/$PREFIX-wifi-off/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;builds/$PREFIX-wifi-off/wifi_off.defaults"
    gate G3-idf-production idf_build "$PREFIX-production" \
        -DSDKCONFIG="$ROOT/builds/$PREFIX-production/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;configs/production.defaults"
    gate G3-idf-console-none idf_build "$PREFIX-console-none" \
        -DSDKCONFIG="$ROOT/builds/$PREFIX-console-none/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;configs/console-none.defaults"
    for t in esp32 esp32c3; do
        gate "G3b-rtk_pure-$t" idf_build "$PREFIX-rtk_pure-$t" -C test_apps/rtk_pure \
            -DIDF_TARGET="$t" -DSDKCONFIG="$ROOT/builds/$PREFIX-rtk_pure-$t/sdkconfig"
    done
    [ "$SKIP_TIDY" -eq 1 ] || [ "$ONLY_IDF" -eq 1 ] ||
        gate G5b-tidy python3 -I tools/tidy_check/tidy_check.py --host-build "builds/$PREFIX-host" \
        --idf-build "builds/$PREFIX-idf" --gnss-build "builds/$PREFIX-gnss-all" \
        --nvs-build "builds/$PREFIX-nvs_store" --test-app-build "builds/$PREFIX-rtk_pure-esp32"
    gate G7b-runtime python3 -I tools/g7b_check/g7b_check.py --build "builds/$PREFIX-idf"
    [ -f "builds/$PREFIX-idf/rtk_main.bin" ] &&
        echo "rtk_main.bin $(stat -c %s "builds/$PREFIX-idf/rtk_main.bin") B"
fi

echo "---- summary (base $BASE) ----"
fail=0
for r in "${RESULTS[@]}"; do
    case "$r" in FAIL*) echo "$r"; fail=1 ;; esac
done
echo "$(printf '%s\n' "${RESULTS[@]}" | grep -c '^PASS') passed, $(printf '%s\n' "${RESULTS[@]}" | grep -c '^FAIL') failed"
exit $fail
