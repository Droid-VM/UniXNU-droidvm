# 大頁模組 harness：VM 記憶體與主機當機除錯要點

## 1. VM 記憶體只來自大頁模組

- Guest RAM 一律由 `gh_hugepage_reserve` 的大頁池（2 MiB 頁）供給，**不走 host 的 free memory**。
  - `MemFree` 和 `MemAvailable` 跟 VM 能不能開機無關。
  - 沒開 VM 時 `/proc/meminfo` 裡約 7 GB 的「未列帳」記憶體，就是這個池。
- 池的目標是 `pool_want=2560` 頁（5120 MiB），等於 guest 5110 MB 加上 framebuffer 和 lend 區。
- 開機時 crosvm 的 log（手機上的 `/data/data/cn.classfun.droidvm/cache/console_<vm>_stderr.log`，是會繞回的 ring log，最新一段在前面）會寫出這次拿到的覆蓋率：
  - 正常：`2MB pass 0: 2555 OK, 0 skipped, 0 failed`，以及 `framebuffer folio coverage: 4 / 4 MB ... pool-served`。
  - **供給不足**：出現 `failed (err=12)`，或 framebuffer 顯示 `NOT pool-served`。2026-10-22 那天只有當機前的那一次開機是這樣（缺 13 頁）。

## 2. 觀測點：`/sys/module/gh_hugepage_reserve/parameters/`

| 檔案 | 看什麼 |
|---|---|
| `refill_stat` | `pool_avail`/`pool_want`（目前/目標頁數）、`active_vms`、`acquire_active`（1 = 採集中，此時寫入會回 `-EBUSY`）、`acquire_stop_reason`、`state` |
| `vm_owners` | 每個 VM 行程一行：`pid= vm_count= served= abandoned= comm=`。**這就是 VM 的 pid 列表** |
| `served_summary` | `tracked`/`served`/`released`/`live`/`orphan`/`purged` |
| `reclaim_debug` | 回收路徑計數（`orphan`、`purged`、`in_*`） |
| `acquire`（只能寫） | `0` 中斷；`1` CONTIG_ANY；`2` sweep 加全系統 reclaim；`3` sweep 加逐窗 evict |

模組原始碼在 `DroidVM/gh-hugepage-reserve`，說明見其中的 `README_zh.md`。

## 3. 關機 → 開機的程序（`vm.sh` 已照此實作）

1. **關機**：送 `crosvm stop` 給控制 socket，等 crosvm 結束；逾時才 `kill -9`。
2. **等 10 秒**（`POOL_SETTLE`）讓大頁模組回收：
   - release worker 把 VM 歸還的頁接回池；
   - owner 死亡後有 3 秒寬限，整個回收有 10 秒逾時；
   - 逾時時仍被 Gunyah 釘住的頁會被放棄追蹤（`abandoned++`），**不會自己回到池裡**。
3. 印出池的狀態與 `vm_owners`，並記到 `out/pool.log`。
4. **開機前確認大頁足夠**：`pool_avail >= pool_want`，且 `active_vms = 0`。
5. **不夠就獲取**：`echo 3 > acquire`，等 `acquire_active` 回到 0（最多 `ACQUIRE_WAIT` 秒），再確認一次。仍不足就**拒絕開機**。
6. `vm.sh` 會把 `start|stop <epoch>` 寫進 `out/vm-request`，讓監視器分辨「要求的關機」和「崩潰」。

## 4. 監視器：`hostwatch.py`

開機與測試期間一律開著：

```
python3 hostwatch.py
```

每 3 秒看一次手機，只在有事件時輸出：

| 事件 | 意義 |
|---|---|
| `HOST REBOOTED` | `boot_id` 變了，或 uptime 倒退（歸零）。**host 被打死了** |
| `PHONE UNREACHABLE` / `PHONE BACK` | adb 失聯 / 恢復 |
| `VM UP pid=…` | VM 行程從池拿到頁（出現在 `vm_owners`，且行程還活著） |
| `VM GONE pid=… (requested stop)` | 正常關機 |
| `VM GONE pid=… (UNEXPECTED …)` | **crosvm 崩潰或被殺**：pid 從大頁列表消失，或行程不見了 |
| `POOL 10s after pid …` | VM 走後 10 秒池的狀態，以及被放棄的頁數 |
| `BOOT FAILED` | 已要求開機，超過 `--boot-wait` 秒（預設 90）仍沒有 pid 出現：**開機失敗** |

## 5. 主機當機的已知樣態與取證

- **樣態**：watchdog bite（`Kernel panic - not syncing: Handle a watchdog bite!`，或某顆 CPU 漏了 wdog ping）。
  - 卡住的 CPU 在跑 `crosvm_vcpu*`，停在 `gh_hypercall_vcpu_run`。
  - 其他 CPU 卡在 `smp_call_function*`。
  - 2026-10-22 21:25 那次發生在**關機後立刻重開、新 VM 開機約 10 秒時**。
- **不是原因**：`RM rejected message 51000015 (MEM_RECLAIM) Error 11 (MEM_INUSE)` 和 `gunyah_share: crosvm is gone but the RM still refuses ... leaving it to VM teardown`。正常關機也會出現，是 `gunyah_share` 提前回收的設計路徑。
- **取證**（host 重開後，從手機拉）：
  - `/data/persist_log/DCS/de/minidump/SYSTEM_LAST_KMSG@…@<時間>.dat.gz`：tar 檔，`minidump.bin` 用 `strings` 看，含 bite 時各 CPU 的狀態。
  - `/data/persist_log/backup/SYSTEM_LAST_KMSG.txt`：上一次開機的 kmsg 尾段。
  - `/data/persist_log/criticallog/persist/critical.log`：Oplus 的事件記錄（`kernel_panic`）。
  - crosvm 的 ring log（見第 1 節）。當機那一次的尾段常常來不及寫出，所以監視器一報 `VM GONE`，就該先把它拉回來。
