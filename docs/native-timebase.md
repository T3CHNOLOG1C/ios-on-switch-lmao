# Native timebase correction (V19)

V19 is installed and safely ejected. The fix corrects a real backward step in `microuptime` on Tegra's 19.2 MHz counter. [Installation](../out/switch/native-xnu-v19-install.json), [independent new-artifact review](../out/research-native/v19-clock/artifact-install-review.json).

The inherited conversion set `rtclock_usec_divisor = frequency / 1000000`, then divided each second's remaining ticks by that integer. At 19.2 MHz the divisor became 19. The microsecond field reached 1,010,526 immediately before the next second reset it to zero. Combining seconds and microseconds therefore jumped backward by 10,526 microseconds despite an increasing hardware counter.

| Counter ticks | Previous total microseconds | Correct total microseconds |
| ---: | ---: | ---: |
| 19,199,999 | 1,010,526 | 999,999 |
| 19,200,000 | 1,000,000 | 1,000,000 |

The actual APFS directory iterator samples `microuptime` before and during enumeration, subtracts unsigned microsecond totals, and compares the result with a 15-second bound. A backward step wraps to a huge positive number. Dividing that wrapped value for its log reproduces the photographed **18446744073709 seconds** exactly. The branch stops the current iterator callback; the caller checks a separate error field, so the warning alone does not prove every enumeration permanently fails or that it explains the entire UI delay. [Actual APFS/native disassembly and arithmetic evidence](../out/research-native/sol-apfs-iterator-time-review.json).

The fix computes `(remaining_ticks * 1000000) / frequency`. It does not change the counter, clock rate or timebase ratio, and does not modify APFS. Since the remaining ticks are below a 32-bit divisor, the intermediate product fits 64 bits. Native compilation emits the corrected multiply/divide sequence. The source is [rtclock.c](../kernel/xnu-10063.141.1/osfmk/arm/rtclock.c) (`git log -- kernel/xnu-10063.141.1/osfmk/arm/rtclock.c`).

Independent checks execute the actual before/after C with ASan, UBSan and unsigned-overflow checks: 220,354 oracle cases across eleven frequencies and extreme tick values; 80,132 integer-MHz controls retain their results; all 19,200,000 ticks in one real-frequency period stay normalized and monotonic. [Results](../out/research-native/v19-clock/independent-result.json). These are function checks, not CPU models or hardware proof. The next native boot must establish that enumeration and startup progress improve.

V19 also retains caches across the specifically qualified native A57 bare-WFI idle path, preserving barriers, wake/timer handling, deep sleep and other cache maintenance. [Narrow guard and native object review](../out/research-native/v19-idle-cache/REPORT.md). The loader now displays `PREPARE COPY` during cache/table setup, and begins `COPY KERNEL` at zero only once actual copying can start. Existing range maintenance stays in place; the change exposes preparation time instead of attributing it to stalled copying.
