#!/bin/bash
# Poll RSS of psx-runtime and psx-beetle every 60s.
# One stdout line per sample. Designed to run under the Monitor tool.

rt_first=""
bt_first=""
while true; do
    ts=$(date +%H:%M:%S)
    # tasklist CSV columns: "Image Name","PID","Session Name","Session#","Mem Usage"
    rt=$(tasklist //FI "IMAGENAME eq psx-runtime.exe" //FO CSV //NH 2>/dev/null | head -1)
    bt=$(tasklist //FI "IMAGENAME eq psx-beetle.exe"  //FO CSV //NH 2>/dev/null | head -1)
    rt_mem=$(echo "$rt" | sed -E 's/.*"([0-9, ]+) K".*/\1/' | tr -d ', ')
    bt_mem=$(echo "$bt" | sed -E 's/.*"([0-9, ]+) K".*/\1/' | tr -d ', ')
    [ -z "$rt_mem" ] || ! [[ "$rt_mem" =~ ^[0-9]+$ ]] && rt_mem=0
    [ -z "$bt_mem" ] || ! [[ "$bt_mem" =~ ^[0-9]+$ ]] && bt_mem=0
    [ -z "$rt_first" ] && [ "$rt_mem" -gt 0 ] && rt_first=$rt_mem
    [ -z "$bt_first" ] && [ "$bt_mem" -gt 0 ] && bt_first=$bt_mem
    rt_growth=$((rt_mem - ${rt_first:-0}))
    bt_growth=$((bt_mem - ${bt_first:-0}))
    echo "$ts runtime_rss=${rt_mem}K (+${rt_growth}K from first ${rt_first:-0}K)  beetle_rss=${bt_mem}K (+${bt_growth}K from first ${bt_first:-0}K)"
    sleep 60
done
